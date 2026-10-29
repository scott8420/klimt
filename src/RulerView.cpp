// RulerView -- the rulers beside the canvas (s031). See RulerView.hpp.
#include "RulerView.hpp"
#include "CanvasView.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/box.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <pangomm/layout.h>
#include <glibmm/main.h>

#include <cmath>

namespace klimt {

namespace R = core::rulers;

RulerView::RulerView(CanvasView& canvas, bool horizontal) : m_canvas(canvas), m_h(horizontal) {
    set_name(horizontal ? "canvas.ruler.top" : "canvas.ruler.left");
    if (horizontal) { set_content_height(kThick); set_hexpand(true); }
    else { set_content_width(kThick); set_vexpand(true); }
    set_draw_func(sigc::mem_fun(*this, &RulerView::draw));
    // No tooltip: it covered the unit menu. The corner's tooltip says how.
    auto rc = Gtk::GestureClick::create();
    rc->set_button(GDK_BUTTON_SECONDARY);
    rc->signal_released().connect([this](int, double x, double y) { pick_unit(x, y); });
    add_controller(rc);
    // s032: drag a guide out onto the picture.
    auto gd = Gtk::GestureDrag::create();
    gd->set_button(GDK_BUTTON_PRIMARY);
    gd->signal_drag_begin().connect([this](double x, double y) { m_gx0 = x; m_gy0 = y; });
    gd->signal_drag_update().connect([this](double dx, double dy) { m_guide_drag.emit(m_gx0 + dx, m_gy0 + dy); });
    gd->signal_drag_end().connect([this](double dx, double dy) {
        if (std::hypot(dx, dy) >= 3) m_guide_drop.emit(m_gx0 + dx, m_gy0 + dy);
        else m_guide_drop.emit(-1e9, -1e9);
    });
    add_controller(gd);
    set_cursor(Gdk::Cursor::create(horizontal ? "row-resize" : "col-resize"));
}

void RulerView::pick_unit(double x, double y) {
    auto* pop = new Gtk::Popover();
    pop->set_parent(*this);
    pop->set_pointing_to(Gdk::Rectangle(static_cast<int>(x), static_cast<int>(y), 1, 1));
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    box->set_margin(6);
    auto head = Gtk::make_managed<Gtk::Label>();
    head->set_markup("<b>Ruler units</b>");
    head->set_xalign(0.f);
    box->append(*head);
    Gtk::CheckButton* first = nullptr;
    for (const auto u : R::units()) {
        auto b = Gtk::make_managed<Gtk::CheckButton>(std::string(R::label(u)));
        if (first) b->set_group(*first); else first = b;
        b->set_active(u == m_unit);
        b->signal_toggled().connect([this, b, u, pop] {
            if (!b->get_active()) return;
            m_unit_signal.emit(u);
            pop->popdown();
        });
        box->append(*b);
    }
    pop->set_child(*box);
    pop->signal_closed().connect([pop] { Glib::signal_idle().connect_once([pop] { pop->unparent(); delete pop; }); });
    pop->popup();
}

void RulerView::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const double bg = m_dark ? 0.16 : 0.90, fg = m_dark ? 0.78 : 0.25;
    cr->set_source_rgb(bg, bg, bg);
    cr->paint();
    // The edge against the canvas.
    cr->set_source_rgb(fg * 0.6 + bg * 0.4, fg * 0.6 + bg * 0.4, fg * 0.6 + bg * 0.4);
    cr->set_line_width(1);
    if (m_h) { cr->move_to(0, h - 0.5); cr->line_to(w, h - 0.5); }
    else { cr->move_to(w - 0.5, 0); cr->line_to(w - 0.5, h); }
    cr->stroke();
    if (!m_doc.any || m_canvas.zoom() <= 0) return;
    const double len = m_h ? w : h;
    const double from = m_h ? m_canvas.image_x_at(0) : m_canvas.image_y_at(0);
    const double to = m_h ? m_canvas.image_x_at(len) : m_canvas.image_y_at(len);
    const auto ticks = R::ticks(m_unit, m_doc.dpi, m_doc.extent, m_doc.origin, from, to, m_canvas.zoom(), 64);
    cr->set_source_rgb(fg, fg, fg);
    auto layout = create_pango_layout("");
    Pango::FontDescription font;
    font.set_size(7 * PANGO_SCALE);
    layout->set_font_description(font);
    const double thick = m_h ? h : w;
    for (const auto& t : ticks) {
        const double s = std::round(m_h ? m_canvas.screen_x_of(t.at) : m_canvas.screen_y_of(t.at)) + 0.5;
        if (s < -1 || s > len + 1) continue;
        const double l = t.level == 0 ? thick : t.level == 1 ? thick * 0.45 : thick * 0.25;
        if (m_h) { cr->move_to(s, thick); cr->line_to(s, thick - l); }
        else { cr->move_to(thick, s); cr->line_to(thick - l, s); }
        cr->stroke();
        if (t.level == 0 && !t.label.empty()) {
            layout->set_text(t.label);
            if (m_h) {
                cr->move_to(s + 2, 1);
                layout->show_in_cairo_context(cr);
            } else {   // the left ruler's labels read upwards
                int tw = 0, th = 0;
                layout->get_pixel_size(tw, th);
                cr->save();
                cr->move_to(1, s - 2);
                cr->rotate(-M_PI / 2);
                layout->show_in_cairo_context(cr);
                cr->restore();
            }
        }
    }
    // Where the pointer is.
    if (m_pointer >= 0) {
        cr->set_source_rgba(0.95, 0.55, 0.15, 0.95);
        const double s = std::round(m_pointer) + 0.5;
        if (m_h) { cr->move_to(s, 0); cr->line_to(s, thick); }
        else { cr->move_to(0, s); cr->line_to(thick, s); }
        cr->stroke();
    }
}

RulerCorner::RulerCorner() {
    set_name("canvas.ruler.corner");
    set_content_width(RulerView::kThick);
    set_content_height(RulerView::kThick);
    set_tooltip_text("Drag onto the picture: the rulers' zero point there. Double-click: back to the top-left corner");
    set_cursor(Gdk::Cursor::create("crosshair"));
    set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        const double bg = m_dark ? 0.16 : 0.90, fg = m_dark ? 0.78 : 0.25;
        cr->set_source_rgb(bg, bg, bg);
        cr->paint();
        cr->set_source_rgb(fg, fg, fg);
        cr->set_line_width(1);
        cr->move_to(w / 2.0 + 0.5, 4); cr->line_to(w / 2.0 + 0.5, h - 4);
        cr->move_to(4, h / 2.0 + 0.5); cr->line_to(w - 4, h / 2.0 + 0.5);
        cr->stroke();
    });
    auto drag = Gtk::GestureDrag::create();
    drag->signal_drag_begin().connect([this](double x, double y) { m_x0 = x; m_y0 = y; });
    drag->signal_drag_update().connect([this](double dx, double dy) { m_drag.emit(m_x0 + dx, m_y0 + dy); });
    drag->signal_drag_end().connect([this](double dx, double dy) {
        if (std::hypot(dx, dy) >= 3) m_drop.emit(m_x0 + dx, m_y0 + dy);
        else m_drop.emit(-1e9, -1e9);   // a click: nothing (the preview goes)
    });
    add_controller(drag);
    auto dbl = Gtk::GestureClick::create();
    dbl->signal_pressed().connect([this](int n, double, double) { if (n == 2) m_reset.emit(); });
    add_controller(dbl);
}

}  // namespace klimt
