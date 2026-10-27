#include "EdgeTab.hpp"

#include <gtkmm/gesturedrag.h>
#include <gdkmm/cursor.h>

#include <cmath>

namespace klimt {

EdgeTab::EdgeTab(const char* name, Side side, bool resizes)
    : Gtk::Box(Gtk::Orientation::VERTICAL), m_side(side), m_resizes(resizes) {
    set_name(name);
    add_css_class(side == Side::Right ? "klimt-edge-tab-right" : "klimt-edge-tab-left");
    set_halign(side == Side::Right ? Gtk::Align::END : Gtk::Align::START);
    set_valign(Gtk::Align::CENTER);
    m_glyph.set_vexpand(true);
    m_glyph.set_valign(Gtk::Align::CENTER);
    append(m_glyph);
    set_tooltip_text(resizes ? "Click: hide / show the panel.  Drag: size it."
                             : "Click: hide / show the tools.");
    if (resizes) set_cursor(Gdk::Cursor::create("ew-resize"));
    set_open(true);

    // The tab rides the dock's edge, so it MOVES while it's dragged; GTK's
    // offsets are relative to the widget and would shrink each step (seen
    // under Xvfb, s006: a 150 px drag sized the dock 40 px). Measure in the
    // window's coordinates instead, which hold still.
    auto g = Gtk::GestureDrag::create();
    g->signal_drag_begin().connect([this](double x, double) {
        m_dragging = false;
        m_start_x = window_x(x);
    });
    g->signal_drag_update().connect([this, g](double dx, double) {
        double sx = 0, sy = 0;
        g->get_start_point(sx, sy);
        const double moved = window_x(sx + dx) - m_start_x;
        if (!m_resizes) return;
        if (!m_dragging && std::abs(moved) >= 4) { m_dragging = true; m_begin.emit(); }
        if (m_dragging) m_by.emit(moved);
    });
    g->signal_drag_end().connect([this, g](double dx, double) {
        double sx = 0, sy = 0;
        g->get_start_point(sx, sy);
        const double moved = window_x(sx + dx) - m_start_x;
        if (m_dragging) { m_dragging = false; m_end.emit(); }
        else if (std::abs(moved) < 4) m_toggle.emit();
    });
    add_controller(g);
}

double EdgeTab::window_x(double x) const {
    auto* self = const_cast<GtkWidget*>(GTK_WIDGET(gobj()));
    GtkRoot* root = gtk_widget_get_root(self);
    graphene_point_t in = GRAPHENE_POINT_INIT(static_cast<float>(x), 0.f), out;
    if (root && gtk_widget_compute_point(self, GTK_WIDGET(root), &in, &out)) return out.x;
    return x;
}

void EdgeTab::set_open(bool open) {
    // The glyph points where a click sends the panel.
    const bool right = m_side == Side::Right;
    m_glyph.set_text(open == right ? "›" : "‹");   // › or ‹
}

}  // namespace klimt
