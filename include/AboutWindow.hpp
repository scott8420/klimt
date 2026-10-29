#pragma once
#include <gtkmm/window.h>
#include <gtkmm/label.h>

// ─────────────────────────────────────────────────────────────────────────────
// AboutWindow -- the DIALOG-LIFETIME exemplar (a stone the README backlog named:
// "hide-on-close vs transient dialog lifetime"). It carries two paradigms at
// once so a single running window exercises both:
//
//   1. Custom window, NOT Gtk::AboutDialog. That stock dialog is DEPRECATED in
//      GTK4 -- a custom Gtk::Window is the forward path, and it's what you'd
//      reach for anyway the moment the About box wants app-specific styling.
//
//   2. Hide-on-close SINGLETON lifetime (CANON: "Lifetime shape is design").
//      Shell holds one, built once and re-presented; the X hides it, it never
//      destructs mid-session, so its named children never re-register -- no
//      self-rebuild registry collision, no leak. This is the born-modern shape;
//      the older heap-allocated self-deleting dialog is the legacy-rescue shape,
//      not the default.
//
// s027: two columns -- the app icon's painting (Klimt's The Dancer), whole and
// full height on the left, a JPEG in the compiled-in gresource bundle (the
// RESOURCE-PIPELINE paradigm -- App::on_activate registers it); name, build
// and credit on the right. Until s027 the hero was klimt-logo-symbolic, which
// still serves the headerbar's logo button.
//
// The right column is four tabs (s027): About, Credits, License, System;
// the painting stays. See docs/resources-and-dialog-lifetime.md.
// ─────────────────────────────────────────────────────────────────────────────
namespace klimt {

class AboutWindow : public Gtk::Window {
public:
    explicit AboutWindow(Gtk::Window& parent);
    ~AboutWindow() override;

private:
    // s027: the System tab is filled each time the window maps -- the scale
    // and the display are only known once it is on a screen.
    void fill_system();
    Gtk::Label* m_system = nullptr;
};

}  // namespace klimt
