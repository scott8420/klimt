// s036 -- the wheel scrolls the panel, not the slider under the pointer.
#include "widgets/Wheel.hpp"

#include <gtkmm/adjustment.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gestureclick.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace klimt::widgets {

namespace {
// A slider / number field at or above `w`, up to the scroller -- its
// "value control", or nullptr.
GtkWidget* value_control(GtkWidget* w, GtkWidget* stop) {
    for (; w && w != stop; w = gtk_widget_get_parent(w))
        if (GTK_IS_RANGE(w) || GTK_IS_SPIN_BUTTON(w)) return w;
    return nullptr;
}
}  // namespace

void wheel_scrolls_panel(Gtk::ScrolledWindow& sw) {
    struct State { double x = -1, y = -1; GtkWidget* armed = nullptr; };   // the pointer; the control last clicked
    auto st = std::make_shared<State>();
    auto motion = Gtk::EventControllerMotion::create();
    motion->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    motion->signal_motion().connect([st](double x, double y) { st->x = x; st->y = y; });
    motion->signal_enter().connect([st](double x, double y) { st->x = x; st->y = y; });
    sw.add_controller(motion);
    // A press arms the control under it (a slider dragged, a number clicked):
    // that one may take the wheel while it keeps the focus. Focus GTK hands
    // out by itself (a dialog closing, a panel rebuilt) arms nothing.
    auto click = Gtk::GestureClick::create();
    click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    click->set_button(0);
    click->signal_pressed().connect([&sw, st](int, double x, double y) {
        GtkWidget* self = GTK_WIDGET(sw.gobj());
        st->armed = value_control(gtk_widget_pick(self, x, y, GTK_PICK_DEFAULT), self);
    });
    sw.add_controller(click);

    auto wheel = Gtk::EventControllerScroll::create();
    wheel->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
    wheel->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    wheel->signal_scroll().connect([&sw, st, w = wheel.get()](double, double dy) -> bool {
        GtkWidget* self = GTK_WIDGET(sw.gobj());
        GtkWidget* ctl = value_control(gtk_widget_pick(self, st->x, st->y, GTK_PICK_DEFAULT), self);
        const bool focused = ctl && (gtk_widget_get_state_flags(ctl) & GTK_STATE_FLAG_FOCUS_WITHIN);
        if (wheel_to_control(ctl && ctl == st->armed, focused)) return false;   // the one you clicked: it takes the wheel
        // Anything else: the panel scrolls itself and keeps the event -- GTK
        // would hand it to whatever was under the pointer when it last moved
        // (content scrolled under a still pointer: a slider), so it never
        // goes on.
        auto adj = sw.get_vadjustment();
        const double page = adj->get_page_size();
        const double step = w->get_unit() == Gdk::ScrollUnit::WHEEL ? std::pow(page, 2.0 / 3.0) : 1.0;
        adj->set_value(std::clamp(adj->get_value() + dy * step, adj->get_lower(), adj->get_upper() - page));
        return true;
    }, false);
    sw.add_controller(wheel);
}

}  // namespace klimt::widgets
