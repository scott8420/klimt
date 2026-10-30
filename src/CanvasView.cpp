#include "CanvasView.hpp"
#include "core/Rulers.hpp"   // s032: guides::snap

#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gdkmm/event.h>
#include <gdkmm/device.h>
#include <gdkmm/devicetool.h>
#include <glibmm/main.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace klimt {

namespace {
// s009 m3: the pen's pressure in the event being handled -- read from a PEN
// only (a tablet's stylus or eraser); anything else is 1. A mouse can carry a
// pressure axis that reads 0 (X11's test pointer does: under Xvfb every stroke
// came out a hairline), so the axis alone is not trusted. Clamped to 0..1.
double pressure_of(const Glib::RefPtr<Gtk::GestureDrag>& g) {
    const auto ev = g->get_current_event();
    if (!ev) return 1;
    const auto dev = ev->get_device();
    const bool pen = (dev && dev->get_source() == Gdk::InputSource::PEN) || ev->get_device_tool();
    double p = 1;
    if (!pen || !ev->get_axis(Gdk::AxisUse::PRESSURE, p)) return 1;
    return std::clamp(p, 0.0, 1.0);
}
// s017: the pen's tilt in the event being handled (0, 0 for anything else).
void tilt_of(const Glib::RefPtr<Gtk::GestureDrag>& g, float& tx, float& ty) {
    tx = ty = 0;
    const auto ev = g->get_current_event();
    if (!ev) return;
    const auto dev = ev->get_device();
    const bool pen = (dev && dev->get_source() == Gdk::InputSource::PEN) || ev->get_device_tool();
    double x = 0, y = 0;
    if (!pen) return;
    if (ev->get_axis(Gdk::AxisUse::XTILT, x)) tx = static_cast<float>(std::clamp(x, -1.0, 1.0));
    if (ev->get_axis(Gdk::AxisUse::YTILT, y)) ty = static_cast<float>(std::clamp(y, -1.0, 1.0));
}
// s012 m1: the pen's ERASER end (a tablet tool of type eraser) is down?
bool eraser_of(const Glib::RefPtr<Gtk::GestureDrag>& g) {
    const auto ev = g->get_current_event();
    if (!ev) return false;
    GdkDeviceTool* tool = gdk_event_get_device_tool(const_cast<GdkEvent*>(ev->gobj()));
    return tool && gdk_device_tool_get_tool_type(tool) == GDK_DEVICE_TOOL_TYPE_ERASER;
}
// The zoom steps zoom_in / zoom_out walk (Photoshop's, roughly).
constexpr double kSteps[] = {1.0 / 64, 1.0 / 48, 1.0 / 32, 1.0 / 24, 1.0 / 16, 1.0 / 12, 1.0 / 8, 1.0 / 6,
                             0.25, 1.0 / 3, 0.5, 2.0 / 3, 1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
constexpr double kMin = 1.0 / 64, kMax = 64;
constexpr double kMargin = 48;   // the fit's breathing room
constexpr int    kTile = 256;
}  // namespace

CanvasView::CanvasView() {
    set_name("canvas");
    set_hexpand(true);
    set_vexpand(true);
    set_focusable(true);
    set_draw_func(sigc::mem_fun(*this, &CanvasView::draw));

    // The checkerboard: one 2 x 2-cell tile, repeated by Cairo.
    m_checker = Cairo::ImageSurface::create(Cairo::Surface::Format::RGB24, 20, 20);
    {
        auto c = Cairo::Context::create(m_checker);
        c->set_source_rgb(0.96, 0.96, 0.96);
        c->paint();
        c->set_source_rgb(0.80, 0.80, 0.80);
        c->rectangle(10, 0, 10, 10);
        c->rectangle(0, 10, 10, 10);
        c->fill();
    }

    // A click takes the keyboard; the Zoom tool zooms where it clicks.
    auto click = Gtk::GestureClick::create();
    click->set_button(GDK_BUTTON_PRIMARY);
    click->signal_pressed().connect([this, click](int n, double x, double y) {
        grab_focus();
        if (m_tool == "crop" && n == 2 && m_crop && crop_hit(x, y) == -1) { commit_crop(); return; }
        if (m_xf.on) {   // s011: in a transform, a click is the box's; a double-click inside commits
            if (n == 2 && xf_inside(x, y)) m_xf_commit.emit();
            return;
        }
        if (m_tool == "eyedropper" && !m_space && m_iw > 0) {
            const bool alt = (click->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
            const int ix = static_cast<int>(std::floor(to_image_x(x))), iy = static_cast<int>(std::floor(to_image_y(y)));
            if (ix >= 0 && iy >= 0 && ix < m_iw && iy < m_ih) m_pick_signal.emit(ix, iy, alt);
            return;
        }
    });
    // The Zoom tool: a CLICK steps in (Alt: out) where it clicks -- on the
    // release, so that a drag (a box to zoom to, s030) isn't also a click.
    click->signal_released().connect([this, click](int, double x, double y) {
        if (m_tool != "zoom" || m_space || m_zoom_box_shown) return;   // a box was drawn: not a click
        const bool out = (click->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
        double z = m_zoom;
        if (out) { for (auto it = std::rbegin(kSteps); it != std::rend(kSteps); ++it) if (*it < m_zoom - 1e-9) { z = *it; break; } }
        else     { for (double s : kSteps) if (s > m_zoom + 1e-9) { z = s; break; } }
        zoom_at(z, x, y);
    });
    add_controller(click);
    {   // s029: right-click -- the tool's settings at the pointer
        auto rc = Gtk::GestureClick::create();
        rc->set_button(GDK_BUTTON_SECONDARY);
        // On the release: a popover opened while the button is still down
        // (the canvas holding the pointer) closed at once (seen under X11).
        rc->signal_released().connect([this](int, double x, double y) { m_settings_at_signal.emit(x, y); });
        add_controller(rc);
    }

    // Pan: middle-drag anywhere; left-drag with Space held or the Hand tool.
    // s008: a left-drag with the Select tool draws a marquee or a lasso.
    for (const unsigned button : {GDK_BUTTON_MIDDLE, GDK_BUTTON_PRIMARY}) {
        auto drag = Gtk::GestureDrag::create();
        drag->set_button(button);
        drag->signal_drag_begin().connect([this, button, drag](double x, double y) {
            m_panning = button == GDK_BUTTON_MIDDLE || m_space || m_tool == "hand";
            if (!m_panning && button == GDK_BUTTON_PRIMARY && lasso_open()) {   // s038: a corner, or the close
                const bool alt = (drag->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
                m_press_x = x;
                m_press_y = y;
                m_lasso.set_scale(6 / m_zoom, 1.5 / m_zoom);
                m_lasso.press({to_image_x(x), to_image_y(y)}, alt);
                if (m_lasso.done()) { lasso_land(); drag->set_state(Gtk::EventSequenceState::DENIED); }
                queue_draw();
                return;
            }
            if (!m_panning && button == GDK_BUTTON_PRIMARY && m_xf.on) {   // s011: the transform box first
                m_press_x = x;
                m_press_y = y;
                xf_press(x, y);
                return;
            }
            if (!m_panning && button == GDK_BUTTON_PRIMARY && m_hint_mode && m_iw > 0) {   // s037: an Isolate stroke, whatever the tool
                const bool alt = (drag->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
                m_press_x = x;
                m_press_y = y;
                m_hint_drag = true;
                m_hint_cur = {(m_hint_mode == 1) != alt, m_hint_r, {{to_image_x(x), to_image_y(y)}}};
                queue_draw();
                return;
            }
            if (!m_panning && button == GDK_BUTTON_PRIMARY && m_tool == "pen" && m_iw > 0) {   // s035: the Pen
                const auto st = drag->get_current_event_state();
                m_press_x = x;
                m_press_y = y;
                pen_press(x, y, (st & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{},
                          (st & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{},
                          (st & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{});
                return;
            }
            if (!m_panning && button == GDK_BUTTON_PRIMARY && ((m_tool == "crop" && m_variant == 1) || m_tool == "gradient") && m_iw > 0) {
                m_line = true;   // s011: Straighten; s012: the Gradient
                m_line_gradient = m_tool == "gradient";
                m_line_a = m_line_b = {to_image_x(x), to_image_y(y)};
                m_press_x = x;
                m_press_y = y;
                queue_draw();
                return;
            }
            if (!m_panning && button == GDK_BUTTON_PRIMARY && m_tool == "crop" && m_iw > 0) {
                m_press_x = x;
                m_press_y = y;
                crop_press(x, y);
                return;
            }
            // s032: the Move tool on a guide drags the guide.
            if (!m_panning && button == GDK_BUTTON_PRIMARY && m_tool == "move" && m_iw > 0 && !m_xf.on) {
                const int g = guide_near(x, y);
                if (g >= 0) {
                    m_guide_drag = g;
                    m_guide_at = m_guides[static_cast<std::size_t>(g)].at;
                    m_press_x = x;
                    m_press_y = y;
                    return;
                }
            }
            if (!m_panning && button == GDK_BUTTON_PRIMARY && m_tool == "move" && m_iw > 0) {
                m_moving = true;
                m_press_x = x;
                m_press_y = y;
                m_move_signal.emit(MoveAt::Begin, 0, 0);
                return;
            }
            // s012: the pen's eraser end erases, whatever the tool (Photoshop's way).
            m_pen_eraser = button == GDK_BUTTON_PRIMARY && eraser_of(drag);
            // s016: Heal > Patch drags the selection's outline to the source.
            if (!m_panning && button == GDK_BUTTON_PRIMARY && m_tool == "heal" && m_variant == 2 && m_iw > 0) {
                m_patching = true;
                m_press_x = x;
                m_press_y = y;
                m_patch_signal.emit(MoveAt::Begin, 0, 0);
                return;
            }
            // s015: Alt- or Ctrl-click with the Clone Stamp picks the source.
            if (!m_panning && button == GDK_BUTTON_PRIMARY && sourced() && !m_pen_eraser && m_iw > 0 &&
                (drag->get_current_event_state() & (Gdk::ModifierType::ALT_MASK | Gdk::ModifierType::CONTROL_MASK)) != Gdk::ModifierType{}) {
                m_source_signal.emit({to_image_x(x), to_image_y(y)});
                drag->set_state(Gtk::EventSequenceState::DENIED);
                return;
            }
            if (!m_panning && button == GDK_BUTTON_PRIMARY && (painting() || m_pen_eraser) && m_iw > 0) {
                const bool shift = (drag->get_current_event_state() & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
                m_stroking = true;
                m_press_x = x;
                m_press_y = y;
                tilt_of(drag, m_tilt_x, m_tilt_y);   // s017
                m_stroke_signal.emit(StrokeAt::Begin, {to_image_x(x), to_image_y(y)}, shift, pressure_of(drag));
                return;
            }
            if (!m_panning && button == GDK_BUTTON_PRIMARY && selecting() && m_iw > 0) {
                // The op is the modifiers AT THE PRESS (Photoshop's).
                const auto st = drag->get_current_event_state();
                const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
                const bool alt = (st & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
                using Op = core::select::Op;
                m_drag = {};
                m_drag.op = shift && alt ? Op::Intersect : shift ? Op::Add : alt ? Op::Subtract : Op::Replace;
                m_drag.kind = m_variant == 1 ? Shape::Kind::Ellipse : m_variant == 2 ? Shape::Kind::Lasso
                            : m_variant == 3 ? Shape::Kind::Point : Shape::Kind::Rect;
                m_drag.pts = {{to_image_x(x), to_image_y(y)}};
                if (m_drag.kind == Shape::Kind::Rect || m_drag.kind == Shape::Kind::Ellipse)   // s032: snap the corner
                    m_drag.pts[0] = {snap_x(m_drag.pts[0].x), snap_y(m_drag.pts[0].y)};
                if (m_drag.kind == Shape::Kind::Lasso) m_lasso.start(m_drag.pts[0], alt, 6 / m_zoom, 1.5 / m_zoom);   // s038
                m_press_x = x;
                m_press_y = y;
                m_rubber = true;
                return;
            }
            // s030: the Zoom tool's drag -- a box; the release zooms to fit it.
            if (!m_panning && button == GDK_BUTTON_PRIMARY && m_tool == "zoom" && m_iw > 0) {
                m_zoom_box = true;
                m_zoom_box_shown = false;
                m_press_x = m_zbx = x;
                m_press_y = m_zby = y;
                return;
            }
            if (!m_panning) { drag->set_state(Gtk::EventSequenceState::DENIED); return; }
            m_pan_cx = m_cx;
            m_pan_cy = m_cy;
            update_cursor();
        });
        // The offset in whole image pixels; Shift keeps one axis (the larger).
        const auto offset = [this, drag](double dx, double dy, int& ox, int& oy) {
            double ix = dx / m_zoom, iy = dy / m_zoom;
            if ((drag->get_current_event_state() & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{}) {
                if (std::abs(ix) >= std::abs(iy)) iy = 0; else ix = 0;
            }
            ox = static_cast<int>(std::lround(ix));
            oy = static_cast<int>(std::lround(iy));
        };
        drag->signal_drag_update().connect([this, drag, offset](double dx, double dy) {
            if (m_hint_drag) {   // s037: a point every quarter of the radius
                const core::path::P q{to_image_x(m_press_x + dx), to_image_y(m_press_y + dy)};
                const auto& last = m_hint_cur.pts.back();
                if (std::hypot(q.x - last.x, q.y - last.y) >= std::max(0.75, m_hint_cur.radius / 4)) { m_hint_cur.pts.push_back(q); queue_draw(); }
                return;
            }
            if (m_pen_drag) {   // s035
                pen_drag(m_press_x + dx, m_press_y + dy, (drag->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{});
                return;
            }
            if (m_xf.on && m_xf.mode != Xf::Mode::None) {
                const auto st = drag->get_current_event_state();
                xf_drag(m_press_x + dx, m_press_y + dy, (st & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{},
                        (st & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{});
                return;
            }
            if (m_moving || m_patching) {
                int ox = 0, oy = 0;
                offset(dx, dy, ox, oy);
                (m_patching ? m_patch_signal : m_move_signal).emit(MoveAt::Move, ox, oy);
                return;
            }
            if (m_line) {
                m_line_b = {to_image_x(m_press_x + dx), to_image_y(m_press_y + dy)};
                // s012: Shift -- the gradient's line in 45-degree steps.
                if (m_line_gradient && (drag->get_current_event_state() & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{}) {
                    const double ldx = m_line_b.x - m_line_a.x, ldy = m_line_b.y - m_line_a.y, len = std::hypot(ldx, ldy);
                    const double ang = std::round(std::atan2(ldy, ldx) / (M_PI / 4)) * (M_PI / 4);
                    m_line_b = {m_line_a.x + len * std::cos(ang), m_line_a.y + len * std::sin(ang)};
                }
                queue_draw();
                return;
            }
            if (m_crop_mode != CropMode::None) {
                const bool shift = (drag->get_current_event_state() & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
                crop_drag(m_press_x + dx, m_press_y + dy, shift);
                return;
            }
            if (m_stroking) {
                m_px = m_press_x + dx;
                m_py = m_press_y + dy;
                // s012 m1: every sample the device sent since the last frame
                // (GTK merges motion per frame: a fast pen curve would come
                // out as straight segments). History is in SURFACE coords:
                // shift it by where this event sits in the widget.
                if (const auto ev = drag->get_current_event()) {
                    GdkEvent* ce = const_cast<GdkEvent*>(ev->gobj());
                    double ex = 0, ey = 0;
                    if (gdk_event_get_event_type(ce) == GDK_MOTION_NOTIFY && gdk_event_get_position(ce, &ex, &ey)) {
                        const double ox = m_px - ex, oy = m_py - ey;
                        const bool pen = pressure_of(drag) != 1.0 || gdk_event_get_device_tool(ce);
                        guint n = 0;
                        GdkTimeCoord* h = gdk_event_get_history(ce, &n);
                        for (guint i = 0; h && i + 1 < n; ++i) {   // the last is this event itself
                            if (!(h[i].flags & GDK_AXIS_FLAG_X) || !(h[i].flags & GDK_AXIS_FLAG_Y)) continue;
                            double p = 1;
                            if (pen && (h[i].flags & GDK_AXIS_FLAG_PRESSURE)) p = std::clamp(h[i].axes[GDK_AXIS_PRESSURE], 0.0, 1.0);
                            m_tilt_x = pen && (h[i].flags & GDK_AXIS_FLAG_XTILT) ? static_cast<float>(std::clamp(h[i].axes[GDK_AXIS_XTILT], -1.0, 1.0)) : 0.f;
                            m_tilt_y = pen && (h[i].flags & GDK_AXIS_FLAG_YTILT) ? static_cast<float>(std::clamp(h[i].axes[GDK_AXIS_YTILT], -1.0, 1.0)) : 0.f;
                            m_stroke_signal.emit(StrokeAt::Move, {to_image_x(h[i].axes[GDK_AXIS_X] + ox), to_image_y(h[i].axes[GDK_AXIS_Y] + oy)},
                                                 false, p);
                        }
                        g_free(h);
                    }
                }
                tilt_of(drag, m_tilt_x, m_tilt_y);   // s017
                m_stroke_signal.emit(StrokeAt::Move, {to_image_x(m_px), to_image_y(m_py)}, false, pressure_of(drag));
                queue_draw();
                return;
            }
            if (m_guide_drag >= 0) {   // s032: a guide follows the pointer (snapping to the edges / centre)
                const auto& g = m_guides[static_cast<std::size_t>(m_guide_drag)];
                m_guide_at = g.vertical ? snap_x(to_image_x(m_press_x + dx)) : snap_y(to_image_y(m_press_y + dy));
                queue_draw();
                return;
            }
            if (m_zoom_box) {   // s030
                m_zbx = m_press_x + dx;
                m_zby = m_press_y + dy;
                m_zoom_box_shown = m_zoom_box_shown || std::hypot(dx, dy) >= 4;
                queue_draw();
                return;
            }
            if (m_rubber && m_drag.kind == Shape::Kind::Point) return;   // the Wand: a click, not a drag
            if (m_rubber && !m_panning && m_drag.kind == Shape::Kind::Lasso) {   // s038: Alt -- a straight side
                const bool alt = (drag->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
                m_lasso.move({to_image_x(m_press_x + dx), to_image_y(m_press_y + dy)}, alt);
                queue_draw();
                return;
            }
            if (m_rubber && !m_panning) {
                const double sx = m_press_x + dx, sy = m_press_y + dy;
                core::select::Pt p{to_image_x(sx), to_image_y(sy)};
                if (m_drag.kind != Shape::Kind::Lasso) p = {snap_x(p.x), snap_y(p.y)};   // s032
                if (m_drag.kind == Shape::Kind::Lasso) {
                    // A new point once the pointer has moved a screen pixel.
                    const auto& last = m_drag.pts.back();
                    if (std::hypot((p.x - last.x) * m_zoom, (p.y - last.y) * m_zoom) >= 1.5) m_drag.pts.push_back(p);
                } else {
                    // Shift while dragging (not at the press): a square / circle.
                    const auto st = drag->get_current_event_state();
                    if (m_drag.op == core::select::Op::Replace &&
                        (st & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{}) {
                        const auto& a = m_drag.pts.front();
                        const double side = std::max(std::abs(p.x - a.x), std::abs(p.y - a.y));
                        p = {a.x + (p.x < a.x ? -side : side), a.y + (p.y < a.y ? -side : side)};
                    }
                    m_drag.pts.resize(1);
                    m_drag.pts.push_back(p);
                }
                queue_draw();
                return;
            }
            if (!m_panning || m_zoom <= 0) return;
            m_fit = false;
            m_cx = m_pan_cx - dx / m_zoom;
            m_cy = m_pan_cy - dy / m_zoom;
            clamp_centre();
            queue_draw();
        });
        drag->signal_drag_end().connect([this, offset, drag](double dx, double dy) {
            if (m_hint_drag) {   // s037: the stroke told; the Shell lands it
                m_hint_drag = false;
                const core::path::P q{to_image_x(m_press_x + dx), to_image_y(m_press_y + dy)};
                if (!(q == m_hint_cur.pts.back()) && (dx != 0 || dy != 0)) m_hint_cur.pts.push_back(q);
                auto st = std::move(m_hint_cur);
                m_hint_cur = {};
                queue_draw();
                m_hint_stroke.emit(st);
                return;
            }
            if (m_pen_drag) { pen_release(); return; }   // s035
            if (m_guide_drag >= 0) {   // s032: its new place told (off the picture: removed)
                const int g = m_guide_drag;
                m_guide_drag = -1;
                if (std::hypot(dx, dy) >= 2) m_guide_moved.emit(g, std::round(m_guide_at));
                queue_draw();
                return;
            }
            if (m_zoom_box) {   // s030: zoom so the box fills the view (a tiny one: the click does it)
                m_zoom_box = false;
                if (m_zoom_box_shown) {
                    const bool out = (drag->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
                    zoom_to_box(m_press_x, m_press_y, m_press_x + dx, m_press_y + dy, out);
                    // kept until the click's release (either order) has seen it
                    Glib::signal_idle().connect_once([this] { m_zoom_box_shown = false; queue_draw(); });
                }
                queue_draw();
                return;
            }
            if (m_moving || m_patching) {
                const bool patching = m_patching;
                m_moving = m_patching = false;
                int ox = 0, oy = 0;
                offset(dx, dy, ox, oy);
                (patching ? m_patch_signal : m_move_signal).emit(MoveAt::End, ox, oy);
                return;
            }
            if (m_line) {
                m_line = false;
                if (!m_line_gradient) m_line_b = {to_image_x(m_press_x + dx), to_image_y(m_press_y + dy)};   // the gradient keeps its snap
                queue_draw();
                if (std::hypot(dx, dy) >= 4) (m_line_gradient ? m_gradient_signal : m_straighten_signal).emit(m_line_a, m_line_b);
                return;
            }
            if (m_crop_mode != CropMode::None) { crop_release(); return; }
            if (m_xf.on && m_xf.mode != Xf::Mode::None) { m_xf.mode = Xf::Mode::None; return; }
            if (m_stroking) {
                m_stroking = false;
                m_stroke_signal.emit(StrokeAt::End, {to_image_x(m_press_x + dx), to_image_y(m_press_y + dy)}, false, pressure_of(drag));
                return;
            }
            if (m_rubber && !m_panning && m_drag.kind == Shape::Kind::Lasso) {   // s038
                const bool alt = (drag->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
                const bool click = !m_lasso.went_straight() && m_lasso.points().size() <= 2 && std::hypot(dx, dy) < 3;
                m_lasso.release({to_image_x(m_press_x + dx), to_image_y(m_press_y + dy)}, alt);
                if (m_lasso.done()) {
                    if (click) m_drag.kind = Shape::Kind::None;   // a click deselects
                    lasso_land();
                }
                queue_draw();
                return;
            }
            if (m_rubber && !m_panning) {
                m_rubber = false;
                Shape s = m_drag;
                m_drag = {};
                // A click (no real drag): Kind None -- the Shell deselects.
                // The Wand is always a click, wherever the press was.
                if (s.kind != Shape::Kind::Point && std::hypot(dx, dy) < 3) s.kind = Shape::Kind::None;
                if (s.kind == Shape::Kind::Rect || s.kind == Shape::Kind::Ellipse) {
                    if (s.pts.size() < 2) s.kind = Shape::Kind::None;
                }
                queue_draw();
                m_shape_signal.emit(s);
                return;
            }
            m_panning = false;
            update_cursor();
        });
        add_controller(drag);
    }

    // The pointer, for zooming about it.
    auto motion = Gtk::EventControllerMotion::create();
    motion->signal_motion().connect([this, motion](double x, double y) {
        m_px = x;
        m_py = y;
        if (lasso_open() && m_lasso.open()) {   // s038: the band follows; Alt gone -- it closes
            const bool alt = (motion->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
            m_lasso.move({to_image_x(x), to_image_y(y)}, alt);
            if (m_lasso.done()) lasso_land();
            queue_draw();
        }
        if (m_tool == "pen" && !m_pen_drag) {   // s035: the modifiers as they are now (the keys may have gone to another widget)
            const auto st = motion->get_current_event_state();
            const bool c = (st & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{}, a = (st & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
            if (c != m_pen_ctrl || a != m_pen_alt) { m_pen_ctrl = c; m_pen_alt = a; pen_modifiers(); }
        }
        if (painting() || m_hint_mode || (m_tool == "pen" && m_pen_sub >= 0)) queue_draw();   // the brush outline / the Pen's rubber band follows the pointer
        if (m_on_pointer) m_on_pointer(x, y);   // s031: the rulers' marks
        if (m_tool == "move" && !m_space && !m_panning && m_guide_drag < 0) {   // s032: over a guide
            const int g = guide_near(x, y);
            if (g >= 0) set_cursor(m_guides[static_cast<std::size_t>(g)].vertical ? "col-resize" : "row-resize");
            else update_cursor();
        }
    });
    motion->signal_leave().connect([this] { m_px = m_py = -1; if (painting() || m_hint_mode) queue_draw(); if (m_on_pointer) m_on_pointer(-1, -1); });
    add_controller(motion);

    // Ctrl+scroll zooms at the pointer; scroll pans.
    auto scroll = Gtk::EventControllerScroll::create();
    scroll->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
    scroll->signal_scroll().connect([this, scroll](double dx, double dy) {
        const auto st = scroll->get_current_event_state();
        if ((st & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{}) {
            const double f = std::pow(1.2, -dy);   // a wheel notch: x1.2
            const double sx = m_px >= 0 ? m_px : m_vw / 2.0, sy = m_py >= 0 ? m_py : m_vh / 2.0;
            zoom_at(m_zoom * f, sx, sy);
            return true;
        }
        if ((st & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{} && dx == 0) std::swap(dx, dy);
        pan_by(dx * 40, dy * 40);
        return true;
    }, false);
    add_controller(scroll);

    // Space held: the hand, for as long as it's down.
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        // s035 (Scott): the Pen's cursor follows Ctrl / Alt held.
        if (k == GDK_KEY_Control_L || k == GDK_KEY_Control_R || k == GDK_KEY_Alt_L || k == GDK_KEY_Alt_R) {
            if (k == GDK_KEY_Control_L || k == GDK_KEY_Control_R) m_pen_ctrl = true; else m_pen_alt = true;
            if (m_tool == "pen") pen_modifiers();
            if (m_hint_mode) queue_draw();   // s038: the Refine ring's colour
            return false;
        }
        if (k != GDK_KEY_space) return false;
        if (!m_space) { m_space = true; update_cursor(); }
        return true;
    }, false);
    keys->signal_key_released().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Control_L || k == GDK_KEY_Control_R) { m_pen_ctrl = false; if (m_tool == "pen") pen_modifiers(); }
        if (k == GDK_KEY_Alt_L || k == GDK_KEY_Alt_R) {
            m_pen_alt = false;
            if (m_tool == "pen") pen_modifiers();
            if (lasso_open() && m_lasso.open()) { m_lasso.alt_up(); if (m_lasso.done()) lasso_land(); queue_draw(); }   // s038
            if (m_hint_mode) queue_draw();
        }
        if (k == GDK_KEY_space) { m_space = false; update_cursor(); }
    });
    add_controller(keys);
}

void CanvasView::set_dark(bool dark) {
    if (dark == m_dark) return;
    m_dark = dark;
    queue_draw();
}

void CanvasView::set_document(int width, int height) {
    const bool resized = width != m_iw || height != m_ih;
    m_iw = std::max(0, width);
    m_ih = std::max(0, height);
    m_cols = (m_iw + kTile - 1) / kTile;
    m_rows = (m_ih + kTile - 1) / kTile;
    m_grid.assign(static_cast<std::size_t>(m_cols) * static_cast<std::size_t>(m_rows), Held{});
    if (resized) fit();
    queue_draw();
}

void CanvasView::set_tiles(const std::vector<Tile>& tiles) {
    for (const auto& t : tiles) {
        if (t.col < 0 || t.row < 0 || t.col >= m_cols || t.row >= m_rows || !t.argb || t.w <= 0 || t.h <= 0 ||
            t.argb->size() < static_cast<std::size_t>(t.w) * static_cast<std::size_t>(t.h))
            continue;
        Held& h = m_grid[static_cast<std::size_t>(t.row) * static_cast<std::size_t>(m_cols) + static_cast<std::size_t>(t.col)];
        // Cairo's own memory, the tile's rows copied in (its stride may be
        // wider than w * 4).
        const auto surface_of = [](const std::vector<std::uint32_t>& px, int w, int hh) {
            auto surf = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, w, hh);
            surf->flush();
            unsigned char* dst = surf->get_data();
            const int stride = surf->get_stride();
            for (int y = 0; y < hh; ++y)
                std::memcpy(dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                            px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w),
                            static_cast<std::size_t>(w) * 4);
            surf->mark_dirty();
            return surf;
        };
        h.w = t.w;
        h.h = t.h;
        h.surface = surface_of(*t.argb, t.w, t.h);
        h.mips.clear();
        for (const auto& m : t.mips)
            if (m.argb && m.w > 0 && m.h > 0 && m.argb->size() >= static_cast<std::size_t>(m.w) * static_cast<std::size_t>(m.h))
                h.mips.push_back(surface_of(*m.argb, m.w, m.h));
    }
    queue_draw();
}

int CanvasView::tiles_shown() const {
    return static_cast<int>(std::count_if(m_grid.begin(), m_grid.end(), [](const Held& h) { return h.surface; }));
}

// ── the view ────────────────────────────────────────────────────────────────
void CanvasView::fit() {
    m_fit = true;
    if (m_vw > 0 && m_vh > 0) layout(m_vw, m_vh);
    queue_draw();
}

void CanvasView::set_view(const View& v) {
    if (v.fit || v.zoom <= 0) { fit(); return; }
    m_fit = false;
    m_zoom = v.zoom;
    m_cx = v.cx;
    m_cy = v.cy;
    tell_zoom();
    queue_draw();
}

void CanvasView::actual_size() { zoom_at(1.0, m_vw / 2.0, m_vh / 2.0); }

void CanvasView::zoom_in() {
    for (double s : kSteps)
        if (s > m_zoom + 1e-9) { zoom_at(s, m_vw / 2.0, m_vh / 2.0); return; }
}

void CanvasView::zoom_out() {
    for (auto it = std::rbegin(kSteps); it != std::rend(kSteps); ++it)
        if (*it < m_zoom - 1e-9) { zoom_at(*it, m_vw / 2.0, m_vh / 2.0); return; }
}

void CanvasView::layout(int w, int h) {
    m_vw = w;
    m_vh = h;
    if (!m_fit || m_iw <= 0 || m_ih <= 0) return;
    const double aw = std::max(1.0, w - 2 * kMargin), ah = std::max(1.0, h - 2 * kMargin);
    m_zoom = std::clamp(std::min(aw / m_iw, ah / m_ih), kMin, kMax);
    m_cx = m_iw / 2.0;
    m_cy = m_ih / 2.0;
}

void CanvasView::zoom_at(double z, double sx, double sy) {
    if (m_iw <= 0 || m_vw <= 0) return;
    z = std::clamp(z, kMin, kMax);
    // The image point under (sx, sy) stays there.
    const double ix = m_cx + (sx - m_vw / 2.0) / m_zoom, iy = m_cy + (sy - m_vh / 2.0) / m_zoom;
    m_zoom = z;
    m_cx = ix - (sx - m_vw / 2.0) / m_zoom;
    m_cy = iy - (sy - m_vh / 2.0) / m_zoom;
    m_fit = false;
    clamp_centre();
    queue_draw();
}

// s032: the guide within 4 screen px of (sx, sy), when they show and aren't locked.
int CanvasView::guide_near(double sx, double sy) const {
    if (!m_show_guides || m_lock_guides) return -1;
    int best = -1;
    double dist = 4;
    for (std::size_t i = 0; i < m_guides.size(); ++i) {
        const auto& g = m_guides[i];
        const double d = std::abs(g.vertical ? sx - to_screen_x(g.at) : sy - to_screen_y(g.at));
        if (d <= dist) { dist = d; best = static_cast<int>(i); }
    }
    return best;
}

// s032 m3: an image position pulled to a guide / the picture's edges or
// centre within 6 screen px (when snapping is on; guides only while shown).
double CanvasView::snap_x(double ix) const {
    if (!m_snap || m_zoom <= 0) return ix;
    core::Image im;
    im.width = m_iw;
    im.height = m_ih;
    if (m_show_guides) im.guides = m_guides;
    return core::guides::snap(ix, true, im, 6 / m_zoom);
}
double CanvasView::snap_y(double iy) const {
    if (!m_snap || m_zoom <= 0) return iy;
    core::Image im;
    im.width = m_iw;
    im.height = m_ih;
    if (m_show_guides) im.guides = m_guides;
    return core::guides::snap(iy, false, im, 6 / m_zoom);
}

// s030: the screen box (x0, y0) - (x1, y1) fills the view -- the Zoom tool's
// drag. `out` (Alt): the other way -- the whole view shrinks into the box.
void CanvasView::zoom_to_box(double x0, double y0, double x1, double y1, bool out) {
    if (m_iw <= 0 || m_vw <= 0 || m_vh <= 0) return;
    const double w = std::abs(x1 - x0), h = std::abs(y1 - y0);
    if (w < 4 && h < 4) return;
    const double bx = (x0 + x1) / 2, by = (y0 + y1) / 2;
    if (out) {
        const double z = std::clamp(m_zoom * std::min(std::max(1.0, w) / m_vw, std::max(1.0, h) / m_vh), kMin, kMax);
        // What was at the view's centre lands at the box's centre.
        m_cx = m_cx - (bx - m_vw / 2.0) / z;
        m_cy = m_cy - (by - m_vh / 2.0) / z;
        m_zoom = z;
        m_fit = false;
        clamp_centre();
        queue_draw();
        return;
    }
    const double cx = to_image_x(bx), cy = to_image_y(by);
    const double z = std::clamp(m_zoom * std::min(m_vw / std::max(1.0, w), m_vh / std::max(1.0, h)), kMin, kMax);
    m_zoom = z;
    m_cx = cx;
    m_cy = cy;
    m_fit = false;
    clamp_centre();
    queue_draw();
}

void CanvasView::pan_by(double dx, double dy) {
    if (m_zoom <= 0 || m_iw <= 0) return;
    m_fit = false;
    m_cx += dx / m_zoom;
    m_cy += dy / m_zoom;
    clamp_centre();
    queue_draw();
}

// The image never leaves the window: the centre stays on the image.
void CanvasView::clamp_centre() {
    m_cx = std::clamp(m_cx, 0.0, static_cast<double>(m_iw));
    m_cy = std::clamp(m_cy, 0.0, static_cast<double>(m_ih));
}

void CanvasView::tell_zoom() {
    if (m_zoom == m_told) return;
    m_told = m_zoom;
    // NOT from inside draw: the listener relabels the status line, and a
    // resize queued mid-frame left it unallocated and blank (seen under
    // Xvfb, s006: "snapshot ... without a current allocation") -- at idle.
    const double z = m_zoom;
    if (m_on_zoom) Glib::signal_idle().connect_once([this, z] { if (m_on_zoom) m_on_zoom(z); });
}

void CanvasView::pen_modifiers() {
    update_cursor();
    // Held modifiers only bend Draw; in Select / Convert the strip shows the mode itself.
    const int shown = m_variant != 0 ? m_variant : m_pen_ctrl ? 1 : m_pen_alt ? 2 : 0;
    if (shown == m_pen_shown) return;
    m_pen_shown = shown;
    m_pen_mode_signal.emit(shown);
}

void CanvasView::update_cursor() {
    if (m_panning) set_cursor("grabbing");
    else if (m_space || m_tool == "hand") set_cursor("grab");
    else if (m_hint_mode) set_cursor("crosshair");   // s037: painting Isolate strokes (the ring shows the size)
    else if (m_tool == "zoom") set_cursor("zoom-in");
    else if (m_tool == "move") set_cursor("move");
    else if (m_tool == "pen" && (m_variant == 1 || (m_variant == 0 && m_pen_ctrl))) set_cursor("default");    // s035: Select (or Ctrl held): the arrow
    else if (m_tool == "pen" && (m_variant == 2 || (m_variant == 0 && m_pen_alt))) set_cursor("pointer");     // Convert (or Alt held): the hand
    else if (m_tool == "crop" || m_tool == "gradient" || m_tool == "pen") set_cursor("crosshair");
    else if (selecting() || m_tool == "eyedropper" || painting()) set_cursor("crosshair");
    else set_cursor("");
}

// ── s008: the ants ──────────────────────────────────────────────────────────
void CanvasView::set_outline(std::shared_ptr<const std::vector<core::select::Seg>> segs) {
    m_ants = (segs && !segs->empty()) ? std::move(segs) : nullptr;
    // March while there is something to march: a dash step every 120 ms.
    if (m_ants && !m_ant_timer.connected())
        m_ant_timer = Glib::signal_timeout().connect([this] {
            m_ant_phase = std::fmod(m_ant_phase + 1, 8);
            queue_draw();
            return true;
        }, 120);
    if (!m_ants && m_ant_timer.connected()) m_ant_timer.disconnect();
    queue_draw();
}

void CanvasView::draw_ants(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    if (!m_ants) return;
    cr->save();
    cr->set_line_width(1);
    cr->set_antialias(Cairo::Antialias::ANTIALIAS_NONE);
    // On whole device pixels (+0.5: a 1 px line sits on the pixel, crisp).
    const auto sx = [&](int x) { return std::round(to_screen_x(x + m_ants_dx)) + 0.5; };   // s016: + the patch's drag
    const auto sy = [&](int y) { return std::round(to_screen_y(y + m_ants_dy)) + 0.5; };
    std::size_t n = 0;
    for (const auto& s : *m_ants) {
        const double x0 = sx(s.x0), y0 = sy(s.y0), x1 = sx(s.x1), y1 = sy(s.y1);
        if (std::max(x0, x1) < -1 || std::min(x0, x1) > w + 1 || std::max(y0, y1) < -1 || std::min(y0, y1) > h + 1) continue;
        cr->move_to(x0, y0);
        cr->line_to(x1, y1);
        ++n;
    }
    if (n) {
        cr->set_source_rgb(1, 1, 1);
        cr->stroke_preserve();
        cr->set_source_rgb(0, 0, 0);
        cr->set_dash(std::vector<double>{4, 4}, m_ant_phase);
        cr->stroke();
    }
    cr->restore();
}

// The marquee / lasso while it's dragged: the same ants, not yet marching.
void CanvasView::draw_rubber(const Cairo::RefPtr<Cairo::Context>& cr) {
    if (!m_rubber || m_drag.pts.empty()) return;
    cr->save();
    cr->set_line_width(1);
    const auto& a = m_drag.pts.front();
    if (m_drag.kind == Shape::Kind::Lasso) {   // s038: the trace, and Alt's band to the pointer
        const auto& pts = m_lasso.points();
        if (pts.empty()) { cr->restore(); return; }
        cr->move_to(to_screen_x(pts.front().x), to_screen_y(pts.front().y));
        for (const auto& p : pts) cr->line_to(to_screen_x(p.x), to_screen_y(p.y));
        if (m_lasso.straight()) {
            const auto t = m_lasso.tip();
            cr->line_to(to_screen_x(t.x), to_screen_y(t.y));
            // the start, as a target: a click there closes
            const double sx = std::round(to_screen_x(pts.front().x)) + 0.5, sy = std::round(to_screen_y(pts.front().y)) + 0.5;
            cr->move_to(sx + 3.5, sy);
            cr->arc(sx, sy, 3.5, 0, 2 * M_PI);
        }
    } else if (m_drag.pts.size() >= 2) {
        const auto& b = m_drag.pts.back();
        const double x0 = std::round(to_screen_x(std::min(a.x, b.x))) + 0.5, y0 = std::round(to_screen_y(std::min(a.y, b.y))) + 0.5;
        const double x1 = std::round(to_screen_x(std::max(a.x, b.x))) + 0.5, y1 = std::round(to_screen_y(std::max(a.y, b.y))) + 0.5;
        if (m_drag.kind == Shape::Kind::Rect) cr->rectangle(x0, y0, x1 - x0, y1 - y0);
        else if (x1 > x0 && y1 > y0) {
            cr->save();
            cr->translate((x0 + x1) / 2, (y0 + y1) / 2);
            cr->scale((x1 - x0) / 2, (y1 - y0) / 2);
            cr->arc(0, 0, 1, 0, 2 * M_PI);
            cr->restore();
        }
    }
    cr->set_source_rgb(1, 1, 1);
    cr->stroke_preserve();
    cr->set_source_rgb(0, 0, 0);
    cr->set_dash(std::vector<double>{4, 4}, 0);
    cr->stroke();
    cr->restore();
}

// ── s038: the Lasso's trace ─────────────────────────────────────────────────
void CanvasView::lasso_land() {
    Shape s = m_drag;
    s.pts = m_lasso.points();
    m_lasso = {};
    m_drag = {};
    m_rubber = false;
    queue_draw();
    m_shape_signal.emit(s);
}

void CanvasView::lasso_finish() {
    if (!lasso_open()) return;
    m_lasso.finish();
    lasso_land();
}

void CanvasView::lasso_cancel() {
    if (!lasso_open()) return;
    m_lasso.cancel();
    m_lasso = {};
    m_drag = {};
    m_rubber = false;
    queue_draw();
}

bool CanvasView::lasso_back() {
    if (!lasso_open() || !m_lasso.back()) return false;
    queue_draw();
    return true;
}

// ── s009 m2: the crop box ─────────────────────────────────────────────────────
namespace {
constexpr double kGrab = 8;   // screen px: how near an edge counts as on it
}

int CanvasView::crop_hit(double sx, double sy) const {
    if (!m_crop) return 0;
    const double x0 = to_screen_x(m_cx0), x1 = to_screen_x(m_cx1), y0 = to_screen_y(m_cy0), y1 = to_screen_y(m_cy1);
    if (sx < x0 - kGrab || sx > x1 + kGrab || sy < y0 - kGrab || sy > y1 + kGrab) return 0;
    int e = 0;
    if (std::abs(sx - x0) <= kGrab) e |= 1;
    else if (std::abs(sx - x1) <= kGrab) e |= 2;
    if (std::abs(sy - y0) <= kGrab) e |= 4;
    else if (std::abs(sy - y1) <= kGrab) e |= 8;
    if (e) return e;
    return (sx > x0 && sx < x1 && sy > y0 && sy < y1) ? -1 : 0;
}

void CanvasView::crop_press(double sx, double sy) {
    const int hit = crop_hit(sx, sy);
    m_crop_from[0] = m_cx0; m_crop_from[1] = m_cy0; m_crop_from[2] = m_cx1; m_crop_from[3] = m_cy1;
    if (hit == -1) { m_crop_mode = CropMode::Move; return; }
    if (hit > 0) { m_crop_mode = CropMode::Resize; m_crop_edges = hit; return; }
    m_crop_mode = CropMode::New;
    m_crop = true;
    m_cx0 = m_cx1 = std::round(snap_x(to_image_x(sx)));   // s032: snapped
    m_cy0 = m_cy1 = std::round(snap_y(to_image_y(sy)));
    m_crop_from[0] = m_cx0; m_crop_from[1] = m_cy0;
    queue_draw();
}

void CanvasView::crop_drag(double sx, double sy, bool shift) {
    const double ix = std::round(snap_x(to_image_x(sx))), iy = std::round(snap_y(to_image_y(sy)));   // s032: snapped
    const double dx = std::round((sx - m_press_x) / m_zoom), dy = std::round((sy - m_press_y) / m_zoom);
    if (m_crop_mode == CropMode::New) {
        double x1 = ix, y1 = iy;
        if (shift) {
            const double side = std::max(std::abs(x1 - m_crop_from[0]), std::abs(y1 - m_crop_from[1]));
            x1 = m_crop_from[0] + (x1 < m_crop_from[0] ? -side : side);
            y1 = m_crop_from[1] + (y1 < m_crop_from[1] ? -side : side);
        }
        m_cx0 = m_crop_from[0]; m_cy0 = m_crop_from[1]; m_cx1 = x1; m_cy1 = y1;
    } else if (m_crop_mode == CropMode::Move) {
        m_cx0 = m_crop_from[0] + dx; m_cy0 = m_crop_from[1] + dy;
        m_cx1 = m_crop_from[2] + dx; m_cy1 = m_crop_from[3] + dy;
    } else if (m_crop_mode == CropMode::Resize) {
        m_cx0 = m_crop_from[0]; m_cy0 = m_crop_from[1]; m_cx1 = m_crop_from[2]; m_cy1 = m_crop_from[3];
        // s032: a dragged edge snaps
        if (m_crop_edges & 1) m_cx0 = std::min(std::round(snap_x(m_crop_from[0] + dx)), m_cx1 - 1);
        if (m_crop_edges & 2) m_cx1 = std::max(std::round(snap_x(m_crop_from[2] + dx)), m_cx0 + 1);
        if (m_crop_edges & 4) m_cy0 = std::min(std::round(snap_y(m_crop_from[1] + dy)), m_cy1 - 1);
        if (m_crop_edges & 8) m_cy1 = std::max(std::round(snap_y(m_crop_from[3] + dy)), m_cy0 + 1);
    }
    queue_draw();
}

void CanvasView::crop_release() {
    if (m_crop_mode == CropMode::New) {
        if (m_cx1 < m_cx0) std::swap(m_cx0, m_cx1);
        if (m_cy1 < m_cy0) std::swap(m_cy0, m_cy1);
        // A click, not a box: no box (it was a click to clear one).
        if ((m_cx1 - m_cx0) * m_zoom < 3 || (m_cy1 - m_cy0) * m_zoom < 3) m_crop = false;
    }
    m_crop_mode = CropMode::None;
    queue_draw();
}

void CanvasView::set_crop(double x0, double y0, double x1, double y1) {
    m_crop = true;
    m_cx0 = std::min(x0, x1); m_cx1 = std::max(x0, x1);
    m_cy0 = std::min(y0, y1); m_cy1 = std::max(y0, y1);
    queue_draw();
}

bool CanvasView::commit_crop() {
    if (!m_crop || m_cx1 - m_cx0 < 1 || m_cy1 - m_cy0 < 1) return false;
    const core::select::Pt a{m_cx0, m_cy0}, b{m_cx1, m_cy1};
    m_crop = false;
    queue_draw();
    m_crop_signal.emit(a, b);
    return true;
}

void CanvasView::draw_crop(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    if (!m_crop) return;
    const double x0 = std::round(to_screen_x(std::min(m_cx0, m_cx1))), x1 = std::round(to_screen_x(std::max(m_cx0, m_cx1)));
    const double y0 = std::round(to_screen_y(std::min(m_cy0, m_cy1))), y1 = std::round(to_screen_y(std::max(m_cy0, m_cy1)));
    cr->save();
    // What goes: dimmed (the whole view minus the box).
    cr->set_fill_rule(Cairo::Context::FillRule::EVEN_ODD);
    cr->rectangle(0, 0, w, h);
    cr->rectangle(x0, y0, x1 - x0, y1 - y0);
    cr->set_source_rgba(0, 0, 0, 0.55);
    cr->fill();
    // The thirds, faint; the box, crisp.
    cr->set_line_width(1);
    cr->set_source_rgba(1, 1, 1, 0.35);
    for (int i = 1; i <= 2; ++i) {
        const double gx = std::round(x0 + (x1 - x0) * i / 3) + 0.5, gy = std::round(y0 + (y1 - y0) * i / 3) + 0.5;
        cr->move_to(gx, y0); cr->line_to(gx, y1);
        cr->move_to(x0, gy); cr->line_to(x1, gy);
    }
    cr->stroke();
    cr->rectangle(x0 + 0.5, y0 + 0.5, x1 - x0 - 1, y1 - y0 - 1);
    cr->set_source_rgb(1, 1, 1);
    cr->stroke();
    // Handles at the corners and the middles of the edges.
    const double mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
    for (const auto& [hx, hy] : {std::pair{x0, y0}, std::pair{mx, y0}, std::pair{x1, y0}, std::pair{x0, my},
                                 std::pair{x1, my}, std::pair{x0, y1}, std::pair{mx, y1}, std::pair{x1, y1}}) {
        cr->rectangle(std::round(hx) - 3.5, std::round(hy) - 3.5, 7, 7);
        cr->set_source_rgb(1, 1, 1);
        cr->fill_preserve();
        cr->set_source_rgb(0.1, 0.1, 0.1);
        cr->stroke();
    }
    // The size, above the box (inside it when the box is at the top).
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d × %d px", static_cast<int>(std::abs(m_cx1 - m_cx0)), static_cast<int>(std::abs(m_cy1 - m_cy0)));
    auto layout = create_pango_layout(buf);
    int tw = 0, th = 0;
    layout->get_pixel_size(tw, th);
    const double ty = y0 - th - 8 >= 0 ? y0 - th - 8 : y0 + 6;
    cr->set_source_rgba(0, 0, 0, 0.7);
    cr->rectangle(x0, ty - 2, tw + 12, th + 4);
    cr->fill();
    cr->set_source_rgb(1, 1, 1);
    cr->move_to(x0 + 6, ty);
    layout->show_in_cairo_context(cr);
    cr->restore();
}

// ── s011 m2: the transform box ───────────────────────────────────────────────
namespace {
constexpr double kHandle = 8;   // screen px: how near a handle counts
}

void CanvasView::begin_transform(double x, double y, double w, double h) {
    m_xf = {};
    m_xf.on = true;
    m_xf.bx = x; m_xf.by = y; m_xf.bw = std::max(1.0, w); m_xf.bh = std::max(1.0, h);
    set_cursor("default");
    queue_draw();
}

void CanvasView::end_transform() {
    m_xf = {};
    update_cursor();
    queue_draw();
}

std::array<double, 6> CanvasView::xf_matrix() const {
    const double cx = m_xf.bx + m_xf.bw / 2, cy = m_xf.by + m_xf.bh / 2;
    const double cs = std::cos(m_xf.ang), sn = std::sin(m_xf.ang);
    // R S: [cs*sx, sn*sx, -sn*sy, cs*sy]
    const double a = cs * m_xf.sx, b = sn * m_xf.sx, c = -sn * m_xf.sy, d = cs * m_xf.sy;
    const double e = cx + m_xf.tx - (a * cx + c * cy), f = cy + m_xf.ty - (b * cx + d * cy);
    return {a, b, c, d, e, f};
}

core::select::Pt CanvasView::xf_world(double lx, double ly) const {
    const auto m = xf_matrix();
    const double x = m_xf.bx + m_xf.bw / 2 + lx, y = m_xf.by + m_xf.bh / 2 + ly;
    return {m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5]};
}

bool CanvasView::xf_inside(double sx, double sy) const {
    const auto m = xf_matrix();
    const double det = m[0] * m[3] - m[1] * m[2];
    if (std::abs(det) < 1e-12) return false;
    const double ix = to_image_x(sx) - m[4], iy = to_image_y(sy) - m[5];
    const double x = (m[3] * ix - m[2] * iy) / det, y = (-m[1] * ix + m[0] * iy) / det;
    return x >= m_xf.bx && x <= m_xf.bx + m_xf.bw && y >= m_xf.by && y <= m_xf.by + m_xf.bh;
}

void CanvasView::xf_press(double sx, double sy) {
    m_xf.tx0 = m_xf.tx; m_xf.ty0 = m_xf.ty; m_xf.sx0 = m_xf.sx; m_xf.sy0 = m_xf.sy; m_xf.ang0 = m_xf.ang;
    m_xf.px0 = to_image_x(sx);
    m_xf.py0 = to_image_y(sy);
    m_xf.mode = Xf::Mode::None;
    for (int hy = -1; hy <= 1; ++hy)
        for (int hx = -1; hx <= 1; ++hx) {
            if (!hx && !hy) continue;
            const auto p = xf_world(hx * m_xf.bw / 2, hy * m_xf.bh / 2);
            if (std::hypot(to_screen_x(p.x) - sx, to_screen_y(p.y) - sy) <= kHandle) {
                m_xf.mode = Xf::Mode::Scale;
                m_xf.hx = hx;
                m_xf.hy = hy;
                return;
            }
        }
    m_xf.mode = xf_inside(sx, sy) ? Xf::Mode::Move : Xf::Mode::Rotate;
}

void CanvasView::xf_drag(double sx, double sy, bool shift, bool alt) {
    const double px = to_image_x(sx), py = to_image_y(sy);
    const double cx = m_xf.bx + m_xf.bw / 2, cy = m_xf.by + m_xf.bh / 2;
    if (m_xf.mode == Xf::Mode::Move) {
        double dx = px - m_xf.px0, dy = py - m_xf.py0;
        if (shift) { if (std::abs(dx) >= std::abs(dy)) dy = 0; else dx = 0; }
        m_xf.tx = m_xf.tx0 + dx;
        m_xf.ty = m_xf.ty0 + dy;
    } else if (m_xf.mode == Xf::Mode::Rotate) {
        const double wcx = cx + m_xf.tx0, wcy = cy + m_xf.ty0;
        double a = m_xf.ang0 + std::atan2(py - wcy, px - wcx) - std::atan2(m_xf.py0 - wcy, m_xf.px0 - wcx);
        if (shift) a = std::round(a / (M_PI / 12)) * (M_PI / 12);
        m_xf.ang = a;
    } else if (m_xf.mode == Xf::Mode::Scale) {
        const int hx = m_xf.hx, hy = m_xf.hy;
        // The anchor: the opposite handle (Alt: the centre), local, box-centred.
        const double ax = alt ? 0 : -hx * m_xf.bw / 2, ay = alt ? 0 : -hy * m_xf.bh / 2;
        const double lx = hx * m_xf.bw / 2 - ax, ly = hy * m_xf.bh / 2 - ay;   // anchor -> handle, unscaled
        const double cs = std::cos(m_xf.ang), sn = std::sin(m_xf.ang);
        // The anchor in the image, at the press's scale.
        const double wax = cx + m_xf.tx0 + cs * m_xf.sx0 * ax - sn * m_xf.sy0 * ay;
        const double way = cy + m_xf.ty0 + sn * m_xf.sx0 * ax + cs * m_xf.sy0 * ay;
        // The pointer from the anchor, in the box's (rotated) frame.
        const double qx = cs * (px - wax) + sn * (py - way), qy = -sn * (px - wax) + cs * (py - way);
        double nsx = m_xf.sx0, nsy = m_xf.sy0;
        if (hx && hy && !shift) {   // a corner: keep the proportions
            const double dx0 = m_xf.sx0 * lx, dy0 = m_xf.sy0 * ly;
            const double k = (qx * dx0 + qy * dy0) / std::max(1e-9, dx0 * dx0 + dy0 * dy0);
            nsx = m_xf.sx0 * k;
            nsy = m_xf.sy0 * k;
        } else {
            if (hx) nsx = qx / lx;
            if (hy) nsy = qy / ly;
        }
        // Never quite zero (a flat box can't be inverted).
        const auto keep = [](double v) { return std::abs(v) < 1e-3 ? (v < 0 ? -1e-3 : 1e-3) : v; };
        m_xf.sx = keep(nsx);
        m_xf.sy = keep(nsy);
        // The anchor stays where it was.
        m_xf.tx = wax - cx - (cs * m_xf.sx * ax - sn * m_xf.sy * ay);
        m_xf.ty = way - cy - (sn * m_xf.sx * ax + cs * m_xf.sy * ay);
    }
    queue_draw();
    m_xf_signal.emit(xf_matrix());
}

void CanvasView::nudge_transform(double dx, double dy) {
    if (!m_xf.on) return;
    m_xf.tx += dx;
    m_xf.ty += dy;
    queue_draw();
    m_xf_signal.emit(xf_matrix());
}

void CanvasView::draw_transform(const Cairo::RefPtr<Cairo::Context>& cr) {
    if (!m_xf.on) return;
    cr->save();
    const double hw = m_xf.bw / 2, hh = m_xf.bh / 2;
    const core::select::Pt q[4] = {xf_world(-hw, -hh), xf_world(hw, -hh), xf_world(hw, hh), xf_world(-hw, hh)};
    cr->set_line_width(1);
    cr->move_to(to_screen_x(q[0].x), to_screen_y(q[0].y));
    for (int i = 1; i < 4; ++i) cr->line_to(to_screen_x(q[i].x), to_screen_y(q[i].y));
    cr->close_path();
    cr->set_source_rgb(0, 0, 0);
    cr->set_line_width(3);
    cr->stroke_preserve();
    cr->set_source_rgb(1, 1, 1);
    cr->set_line_width(1);
    cr->stroke();
    for (int hy = -1; hy <= 1; ++hy)
        for (int hx = -1; hx <= 1; ++hx) {
            if (!hx && !hy) continue;
            const auto p = xf_world(hx * hw, hy * hh);
            cr->rectangle(std::round(to_screen_x(p.x)) - 3.5, std::round(to_screen_y(p.y)) - 3.5, 7, 7);
            cr->set_source_rgb(1, 1, 1);
            cr->fill_preserve();
            cr->set_source_rgb(0.1, 0.1, 0.1);
            cr->stroke();
        }
    const auto c = xf_world(0, 0);   // the centre the box turns about
    const double ccx = to_screen_x(c.x), ccy = to_screen_y(c.y);
    cr->arc(ccx, ccy, 4, 0, 2 * M_PI);
    cr->set_source_rgb(1, 1, 1);
    cr->stroke();
    // What it's doing, above the box's top.
    char buf[96];
    double deg = m_xf.ang * 180 / M_PI;
    deg = std::remainder(deg, 360.0);
    std::snprintf(buf, sizeof buf, "W %.0f%%  H %.0f%%  %.1f°", m_xf.sx * 100, m_xf.sy * 100, deg);
    auto layout = create_pango_layout(buf);
    int tw = 0, th = 0;
    layout->get_pixel_size(tw, th);
    double top = std::min({to_screen_y(q[0].y), to_screen_y(q[1].y), to_screen_y(q[2].y), to_screen_y(q[3].y)});
    double left = std::min({to_screen_x(q[0].x), to_screen_x(q[1].x), to_screen_x(q[2].x), to_screen_x(q[3].x)});
    const double ty = top - th - 10 >= 0 ? top - th - 10 : top + 8;
    cr->set_source_rgba(0, 0, 0, 0.7);
    cr->rectangle(left, ty - 2, tw + 12, th + 4);
    cr->fill();
    cr->set_source_rgb(1, 1, 1);
    cr->move_to(left + 6, ty);
    layout->show_in_cairo_context(cr);
    cr->restore();
}

// ── drawing ─────────────────────────────────────────────────────────────────
void CanvasView::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    if (w != m_vw || h != m_vh) layout(w, h);
    // The surround: neutral (R = G = B), darker under a dark UI.
    const double g = m_dark ? 0.22 : 0.62;
    cr->set_source_rgb(g, g, g);
    cr->paint();
    if (m_iw <= 0 || m_ih <= 0) {
        // s009: no image yet -- say how to get one.
        auto layout = create_pango_layout("No image\nFile > New (Ctrl+N)  ·  Open (Ctrl+O)");
        layout->set_alignment(Pango::Alignment::CENTER);
        int tw = 0, th = 0;
        layout->get_pixel_size(tw, th);
        cr->set_source_rgba(m_dark ? 1 : 0, m_dark ? 1 : 0, m_dark ? 1 : 0, 0.45);
        cr->move_to((w - tw) / 2.0, (h - th) / 2.0);
        layout->show_in_cairo_context(cr);
        return;
    }

    // The sheet on screen, snapped to device pixels.
    const double x0 = std::round(to_screen_x(0)), y0 = std::round(to_screen_y(0));
    const double x1 = std::round(to_screen_x(m_iw)), y1 = std::round(to_screen_y(m_ih));
    if (x1 - x0 < 1 || y1 - y0 < 1) return;
    // The part of it in the window -- all that is ever drawn.
    const double vx0 = std::max(0.0, x0), vy0 = std::max(0.0, y0);
    const double vx1 = std::min<double>(w, x1), vy1 = std::min<double>(h, y1);
    if (vx1 <= vx0 || vy1 <= vy0) return;

    // A soft edge: a few widening translucent rings (only when the edge shows).
    for (int i = 6; i >= 1; --i) {
        cr->set_source_rgba(0, 0, 0, 0.05);
        cr->rectangle(x0 - i, y0 - i + 3, (x1 - x0) + 2 * i, (y1 - y0) + 2 * i);
        cr->fill();
    }
    // Transparency: the checkerboard, in screen space, inside the view.
    cr->save();
    cr->rectangle(vx0, vy0, vx1 - vx0, vy1 - vy0);
    cr->clip();
    {
        auto pat = Cairo::SurfacePattern::create(m_checker);
        pat->set_extend(Cairo::Pattern::Extend::REPEAT);
        Cairo::Matrix m = Cairo::identity_matrix();
        m.translate(-x0, -y0);   // cells anchored to the sheet, so they scroll with it
        pat->set_matrix(m);
        cr->set_source(pat);
        cr->paint();
    }
    cr->restore();

    // The tiles in view, each snapped to whole device pixels (no seams).
    const int c0 = std::max(0, static_cast<int>(std::floor(((vx0 - x0) / m_zoom) / kTile)));
    const int c1 = std::min(m_cols - 1, static_cast<int>(std::floor(((vx1 - x0) / m_zoom) / kTile)));
    const int r0 = std::max(0, static_cast<int>(std::floor(((vy0 - y0) / m_zoom) / kTile)));
    const int r1 = std::min(m_rows - 1, static_cast<int>(std::floor(((vy1 - y0) / m_zoom) / kTile)));
    const auto filter = m_zoom < 1 ? Cairo::SurfacePattern::Filter::GOOD : Cairo::SurfacePattern::Filter::NEAREST;
    // s010 m4: which mip: 1/2^level >= zoom (level 0: the tile itself).
    int level = 0;
    while (level < kMips && std::ldexp(1.0, -(level + 1)) >= m_zoom - 1e-9) ++level;
    for (int row = r0; row <= r1; ++row)
        for (int col = c0; col <= c1; ++col) {
            const Held& t = m_grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(m_cols) + static_cast<std::size_t>(col)];
            if (!t.surface) continue;
            const double tx0 = std::round(to_screen_x(col * kTile)), ty0 = std::round(to_screen_y(row * kTile));
            const double tx1 = std::round(to_screen_x(col * kTile + t.w)), ty1 = std::round(to_screen_y(row * kTile + t.h));
            if (tx1 <= tx0 || ty1 <= ty0) continue;
            // s010 m4: zoomed out, the mip nearest above the zoom (never
            // scaled down more than 2x), not the whole tile.
            Cairo::RefPtr<Cairo::ImageSurface> src = t.surface;
            int sw = t.w, sh = t.h;
            if (level > 0 && !t.mips.empty()) {
                src = t.mips[static_cast<std::size_t>(std::min<int>(level, static_cast<int>(t.mips.size())) - 1)];
                sw = src->get_width();
                sh = src->get_height();
            }
            cr->save();
            // s030: the clip snapped to DEVICE pixels, not antialiased. On a
            // fractionally scaled screen (125 / 150 / 175 %) a whole logical
            // pixel falls between device pixels: each neighbour's clip half
            // covered the shared edge, and the checkerboard showed through
            // as a thin grid (Scott, on Fedora; reproduced in cairo at 1.25 -
            // 1.75). Unantialiased, both sides round the same way: no seam.
            cr->set_antialias(Cairo::Antialias::ANTIALIAS_NONE);
            cr->rectangle(tx0, ty0, tx1 - tx0, ty1 - ty0);
            cr->clip();
            cr->translate(tx0, ty0);
            cr->scale((tx1 - tx0) / sw, (ty1 - ty0) / sh);
            auto pat = Cairo::SurfacePattern::create(src);
            pat->set_filter(filter);
            pat->set_extend(Cairo::Pattern::Extend::PAD);   // the edge pixel, not transparency, at the seam
            cr->set_source(pat);
            cr->paint();
            cr->restore();
        }
    draw_hints(cr);  // s037: Isolate's strokes, under the outline
    draw_path(cr);   // s035 -- under the ants, so a path loaded as the selection still shows its ants
    draw_ants(cr, w, h);
    draw_rubber(cr);
    if (m_zoom_box && m_zoom_box_shown) {   // s030: the Zoom tool's box (while dragging)
        const double x0 = std::round(std::min(m_press_x, m_zbx)) + 0.5, y0 = std::round(std::min(m_press_y, m_zby)) + 0.5;
        const double x1 = std::round(std::max(m_press_x, m_zbx)) + 0.5, y1 = std::round(std::max(m_press_y, m_zby)) + 0.5;
        cr->save();
        cr->set_line_width(1);
        cr->rectangle(x0, y0, x1 - x0, y1 - y0);
        cr->set_source_rgb(1, 1, 1);
        cr->stroke_preserve();
        cr->set_source_rgb(0, 0, 0);
        cr->set_dash(std::vector<double>{4, 4}, 0);
        cr->stroke();
        cr->restore();
    }
    draw_crop(cr, w, h);
    draw_transform(cr);
    if (m_line) {   // s011: the Straighten line, and its angle
        const double ax = to_screen_x(m_line_a.x), ay = to_screen_y(m_line_a.y);
        const double bx = to_screen_x(m_line_b.x), by = to_screen_y(m_line_b.y);
        cr->save();
        cr->move_to(ax, ay);
        cr->line_to(bx, by);
        cr->set_line_width(3);
        cr->set_source_rgb(0, 0, 0);
        cr->stroke_preserve();
        cr->set_line_width(1);
        cr->set_source_rgb(1, 1, 1);
        cr->stroke();
        char buf[48];
        std::snprintf(buf, sizeof buf, "%.1f°", -std::atan2(m_line_b.y - m_line_a.y, m_line_b.x - m_line_a.x) * 180 / M_PI);
        if (m_line_gradient) std::snprintf(buf, sizeof buf, "%.0f px", std::hypot(m_line_b.x - m_line_a.x, m_line_b.y - m_line_a.y));
        auto layout = create_pango_layout(buf);
        int tw = 0, th = 0;
        layout->get_pixel_size(tw, th);
        cr->set_source_rgba(0, 0, 0, 0.7);
        cr->rectangle(bx + 12, by - th - 6, tw + 12, th + 4);
        cr->fill();
        cr->set_source_rgb(1, 1, 1);
        cr->move_to(bx + 18, by - th - 4);
        layout->show_in_cairo_context(cr);
        cr->restore();
    }
    // s008 m4: the brush's outline at the pointer (its real size on screen).
    if (painting() && m_px >= 0 && !m_space) {
        const double r = std::max(1.0, m_brush / 2 * m_zoom);
        if (r >= 3) {
            // s017: the tip's own shape -- an ellipse at its angle, or a square.
            const auto shape = [&](double rr) {
                cr->save();
                cr->translate(m_px, m_py);
                cr->rotate(-m_tip_angle * M_PI / 180);   // counter-clockwise on screen
                cr->scale(1, std::clamp(m_tip_round, 0.05, 1.0));
                if (m_tip == 1) cr->rectangle(-rr, -rr, 2 * rr, 2 * rr);
                else cr->arc(0, 0, rr, 0, 2 * M_PI);
                cr->restore();
            };
            cr->save();
            cr->set_line_width(1);
            shape(r);
            cr->set_source_rgba(0, 0, 0, 0.8);
            cr->stroke();
            shape(r + 1);
            cr->set_source_rgba(1, 1, 1, 0.8);
            cr->stroke();
            cr->restore();
        }
    }
    // s015: the Clone Stamp's source -- a crosshair where it takes from.
    if (sourced() && m_clone_set && !m_space && (!m_clone_fixed || m_px >= 0)) {
        double sx = to_screen_x(m_clone_pt.x), sy = to_screen_y(m_clone_pt.y);
        if (m_clone_fixed) {   // it walks with the pointer
            sx = m_px + m_clone_dx * m_zoom;
            sy = m_py + m_clone_dy * m_zoom;
        }
        cr->save();
        for (int pass = 0; pass < 2; ++pass) {
            cr->set_line_width(pass ? 1 : 3);
            cr->set_source_rgba(pass, pass, pass, 0.85);
            cr->move_to(sx - 9, sy); cr->line_to(sx + 9, sy);
            cr->move_to(sx, sy - 9); cr->line_to(sx, sy + 9);
            cr->stroke();
        }
        cr->restore();
    }
    tell_zoom();
    // s032: the guides -- cyan lines across the view (the one dragged where it is
    // going; one coming out of a ruler).
    if ((m_show_guides && !m_guides.empty()) || m_guide_preview) {
        cr->save();
        cr->set_line_width(1);
        const auto line = [&](const core::Guide& g) {
            if (g.vertical) { const double sx = std::round(to_screen_x(g.at)) + 0.5; cr->move_to(sx, 0); cr->line_to(sx, h); }
            else { const double sy = std::round(to_screen_y(g.at)) + 0.5; cr->move_to(0, sy); cr->line_to(w, sy); }
        };
        cr->set_source_rgba(0.0, 0.78, 1.0, 0.9);
        if (m_show_guides)
            for (std::size_t i = 0; i < m_guides.size(); ++i) {
                core::Guide g = m_guides[i];
                if (static_cast<int>(i) == m_guide_drag) g.at = m_guide_at;
                line(g);
            }
        if (m_guide_preview) line(*m_guide_preview);
        cr->stroke();
        cr->restore();
    }
    // s031: the zero point being dragged -- a cross over the picture.
    if (m_cross) {
        cr->save();
        cr->set_line_width(1);
        const double cx = std::round(m_cross->x) + 0.5, cy = std::round(m_cross->y) + 0.5;
        for (int pass = 0; pass < 2; ++pass) {
            cr->set_source_rgba(pass, pass, pass, 0.85);
            if (pass) cr->set_dash(std::vector<double>{4, 4}, 0);
            cr->move_to(0, cy); cr->line_to(w, cy);
            cr->move_to(cx, 0); cr->line_to(cx, h);
            cr->stroke();
        }
        cr->restore();
    }
    // s031: the rulers follow the view -- told at idle, not inside the draw.
    if (m_on_view && (m_zoom != m_told_vz || m_cx != m_told_cx || m_cy != m_told_cy || w != m_told_vw || h != m_told_vh)) {
        m_told_vz = m_zoom; m_told_cx = m_cx; m_told_cy = m_cy; m_told_vw = w; m_told_vh = h;
        Glib::signal_idle().connect_once([this] { if (m_on_view) m_on_view(); });
    }
}

pixel::Rect CanvasView::visible() const {
    if (m_zoom <= 0 || m_vw <= 0 || m_vh <= 0) return {0, 0, m_iw, m_ih};
    const int x0 = static_cast<int>(std::floor(to_image_x(0))), y0 = static_cast<int>(std::floor(to_image_y(0)));
    const int x1 = static_cast<int>(std::ceil(to_image_x(m_vw))), y1 = static_cast<int>(std::ceil(to_image_y(m_vh)));
    return pixel::Rect{x0, y0, x1 - x0, y1 - y0}.intersect({0, 0, m_iw, m_ih});
}

}  // namespace klimt
