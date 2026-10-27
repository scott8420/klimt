#include "CanvasView.hpp"

#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <glibmm/main.h>
#include <pangomm/layout.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace klimt {

namespace {
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
    click->signal_pressed().connect([this, click](int, double x, double y) {
        grab_focus();
        if (m_tool == "eyedropper" && !m_space && m_iw > 0) {
            const bool alt = (click->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
            const int ix = static_cast<int>(std::floor(to_image_x(x))), iy = static_cast<int>(std::floor(to_image_y(y)));
            if (ix >= 0 && iy >= 0 && ix < m_iw && iy < m_ih) m_pick_signal.emit(ix, iy, alt);
            return;
        }
        if (m_tool == "zoom" && !m_space) {
            const bool out = (click->get_current_event_state() & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
            double z = m_zoom;
            if (out) { for (auto it = std::rbegin(kSteps); it != std::rend(kSteps); ++it) if (*it < m_zoom - 1e-9) { z = *it; break; } }
            else     { for (double s : kSteps) if (s > m_zoom + 1e-9) { z = s; break; } }
            zoom_at(z, x, y);
        }
    });
    add_controller(click);

    // Pan: middle-drag anywhere; left-drag with Space held or the Hand tool.
    // s008: a left-drag with the Select tool draws a marquee or a lasso.
    for (const unsigned button : {GDK_BUTTON_MIDDLE, GDK_BUTTON_PRIMARY}) {
        auto drag = Gtk::GestureDrag::create();
        drag->set_button(button);
        drag->signal_drag_begin().connect([this, button, drag](double x, double y) {
            m_panning = button == GDK_BUTTON_MIDDLE || m_space || m_tool == "hand";
            if (!m_panning && button == GDK_BUTTON_PRIMARY && painting() && m_iw > 0) {
                const bool shift = (drag->get_current_event_state() & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
                m_stroking = true;
                m_press_x = x;
                m_press_y = y;
                m_stroke_signal.emit(StrokeAt::Begin, {to_image_x(x), to_image_y(y)}, shift);
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
                m_press_x = x;
                m_press_y = y;
                m_rubber = true;
                return;
            }
            if (!m_panning) { drag->set_state(Gtk::EventSequenceState::DENIED); return; }
            m_pan_cx = m_cx;
            m_pan_cy = m_cy;
            update_cursor();
        });
        drag->signal_drag_update().connect([this, drag](double dx, double dy) {
            if (m_stroking) {
                m_px = m_press_x + dx;
                m_py = m_press_y + dy;
                m_stroke_signal.emit(StrokeAt::Move, {to_image_x(m_px), to_image_y(m_py)}, false);
                queue_draw();
                return;
            }
            if (m_rubber && m_drag.kind == Shape::Kind::Point) return;   // the Wand: a click, not a drag
            if (m_rubber) {
                const double sx = m_press_x + dx, sy = m_press_y + dy;
                core::select::Pt p{to_image_x(sx), to_image_y(sy)};
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
        drag->signal_drag_end().connect([this](double dx, double dy) {
            if (m_stroking) {
                m_stroking = false;
                m_stroke_signal.emit(StrokeAt::End, {to_image_x(m_press_x + dx), to_image_y(m_press_y + dy)}, false);
                return;
            }
            if (m_rubber) {
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
    motion->signal_motion().connect([this](double x, double y) {
        m_px = x;
        m_py = y;
        if (painting()) queue_draw();   // the brush outline follows the pointer
    });
    motion->signal_leave().connect([this] { m_px = m_py = -1; if (painting()) queue_draw(); });
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
        if (k != GDK_KEY_space) return false;
        if (!m_space) { m_space = true; update_cursor(); }
        return true;
    }, false);
    keys->signal_key_released().connect([this](guint k, guint, Gdk::ModifierType) {
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
        auto surf = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32, t.w, t.h);
        surf->flush();
        unsigned char* dst = surf->get_data();
        const int stride = surf->get_stride();
        for (int y = 0; y < t.h; ++y)
            std::memcpy(dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                        t.argb->data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(t.w),
                        static_cast<std::size_t>(t.w) * 4);
        surf->mark_dirty();
        h.w = t.w;
        h.h = t.h;
        h.surface = std::move(surf);
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

void CanvasView::update_cursor() {
    if (m_panning) set_cursor("grabbing");
    else if (m_space || m_tool == "hand") set_cursor("grab");
    else if (m_tool == "zoom") set_cursor("zoom-in");
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
    const auto sx = [&](int x) { return std::round(to_screen_x(x)) + 0.5; };
    const auto sy = [&](int y) { return std::round(to_screen_y(y)) + 0.5; };
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
    if (m_drag.kind == Shape::Kind::Lasso) {
        cr->move_to(to_screen_x(a.x), to_screen_y(a.y));
        for (const auto& p : m_drag.pts) cr->line_to(to_screen_x(p.x), to_screen_y(p.y));
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

// ── drawing ─────────────────────────────────────────────────────────────────
void CanvasView::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    if (w != m_vw || h != m_vh) layout(w, h);
    // The surround: neutral (R = G = B), darker under a dark UI.
    const double g = m_dark ? 0.22 : 0.62;
    cr->set_source_rgb(g, g, g);
    cr->paint();
    if (m_iw <= 0 || m_ih <= 0) return;

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
    for (int row = r0; row <= r1; ++row)
        for (int col = c0; col <= c1; ++col) {
            const Held& t = m_grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(m_cols) + static_cast<std::size_t>(col)];
            if (!t.surface) continue;
            const double tx0 = std::round(to_screen_x(col * kTile)), ty0 = std::round(to_screen_y(row * kTile));
            const double tx1 = std::round(to_screen_x(col * kTile + t.w)), ty1 = std::round(to_screen_y(row * kTile + t.h));
            if (tx1 <= tx0 || ty1 <= ty0) continue;
            cr->save();
            cr->rectangle(tx0, ty0, tx1 - tx0, ty1 - ty0);
            cr->clip();
            cr->translate(tx0, ty0);
            cr->scale((tx1 - tx0) / t.w, (ty1 - ty0) / t.h);
            auto pat = Cairo::SurfacePattern::create(t.surface);
            pat->set_filter(filter);
            pat->set_extend(Cairo::Pattern::Extend::PAD);   // the edge pixel, not transparency, at the seam
            cr->set_source(pat);
            cr->paint();
            cr->restore();
        }
    draw_ants(cr, w, h);
    draw_rubber(cr);
    // s008 m4: the brush's outline at the pointer (its real size on screen).
    if (painting() && m_px >= 0 && !m_space) {
        const double r = std::max(1.0, m_brush / 2 * m_zoom);
        if (r >= 3) {
            cr->save();
            cr->set_line_width(1);
            cr->arc(m_px, m_py, r, 0, 2 * M_PI);
            cr->set_source_rgba(0, 0, 0, 0.8);
            cr->stroke();
            cr->arc(m_px, m_py, r + 1, 0, 2 * M_PI);
            cr->set_source_rgba(1, 1, 1, 0.8);
            cr->stroke();
            cr->restore();
        }
    }
    tell_zoom();
}

}  // namespace klimt
