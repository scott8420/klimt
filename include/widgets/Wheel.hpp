#pragma once
// s036 -- THE WHEEL SCROLLS THE PANEL (Scott's Inbox, s034: "the mouse wheel
// over Properties changes a slider"). GTK gives the wheel to a slider or a
// number field under the pointer even while you're scrolling the panel past
// it. Guarded, a scrolled panel keeps the wheel for itself unless you have
// CLICKED that control and it still has the focus (then the wheel sets it,
// as before; focus GTK hands out on its own doesn't count). Capture phase on
// the scroller: it sees the wheel first.
#include <gtkmm/scrolledwindow.h>

namespace klimt::widgets {

void wheel_scrolls_panel(Gtk::ScrolledWindow& sw);

// The rule: a wheel over a control goes to it only when it's the one you
// clicked and it still holds the focus; otherwise the panel scrolls.
constexpr bool wheel_to_control(bool clicked, bool focused) { return clicked && focused; }

}  // namespace klimt::widgets
