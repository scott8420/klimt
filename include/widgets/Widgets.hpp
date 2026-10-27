#pragma once
#include "widgets/Named.hpp"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stack.h>
#include <gtkmm/stacksidebar.h>
#include <gtkmm/togglebutton.h>

// The wrapper aliases. Widen on demand, not for symmetry (CANON). Each line is
// the whole cost of making a GTK widget name-mandatory in Cairn's app code.
namespace klimt::widgets {

using Box            = Named<Gtk::Box>;
using Button         = Named<Gtk::Button>;
using DropDown       = Named<Gtk::DropDown>;      // s003: the blend-mode menu
using Entry          = Named<Gtk::Entry>;
using Image          = Named<Gtk::Image>;
using Label          = Named<Gtk::Label>;
using ListBox        = Named<Gtk::ListBox>;
using MenuButton     = Named<Gtk::MenuButton>;
using Scale          = Named<Gtk::Scale>;
using ScrolledWindow = Named<Gtk::ScrolledWindow>;
using SpinButton     = Named<Gtk::SpinButton>;    // s003: fill
using Stack          = Named<Gtk::Stack>;
using StackSidebar   = Named<Gtk::StackSidebar>;
using ToggleButton   = Named<Gtk::ToggleButton>;  // s003: the lock buttons

}  // namespace klimt::widgets
