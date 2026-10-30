#pragma once
// PathsPanel -- the dock's Paths tab (s035): the WORK PATH and the saved
// paths. docs/paths.md.
//
// Click a row: that path is PICKED -- shown on the canvas, and the Pen
// edits it (click the empty list below the rows: none picked, the canvas
// hides it unless the Pen is in hand). Ctrl-click: load it as the
// selection (Shift adds, Alt subtracts, both intersect). Double-click a
// saved path: rename it. The buttons: Load as Selection, Make Work Path
// (from the selection, at the Tolerance below), Mask from Path (the picked
// layer), Save (the Work Path becomes "Path n"; a saved one: a copy), Delete.
// The Shell does the verbs (core::paths) -- each ONE step.
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/spinbutton.h>
#include <sigc++/signal.h>

#include <string>
#include <vector>

namespace klimt {

class PathsPanel : public Gtk::Box {
public:
    struct Item { int index; std::string name; std::size_t anchors; };   // index -1: the Work Path
    PathsPanel();
    // What the document holds now, which is picked (-2: none), whether there
    // is a selection to trace and a layer to mask.
    void show(const std::vector<Item>& items, int picked, bool can_trace, bool can_mask, double tolerance);

    sigc::signal<void(int)>& signal_pick() { return m_pick; }                 // -1 work, n saved, -2 none
    sigc::signal<void(int, int)>& signal_load() { return m_load; }            // index, select::Op as int
    sigc::signal<void()>& signal_make_work() { return m_make; }
    sigc::signal<void(int)>& signal_mask() { return m_mask; }
    sigc::signal<void(int)>& signal_save() { return m_save; }
    sigc::signal<void(int)>& signal_remove() { return m_remove; }
    sigc::signal<void(int, const std::string&)>& signal_rename() { return m_rename; }
    sigc::signal<void(double)>& signal_tolerance() { return m_tolerance; }

private:
    void rename_row(int row);
    int  picked_index() const;   // the picked row's path index; -2: none
    std::vector<Item> m_items;
    int               m_picked = -2;
    bool              m_syncing = false;
    Gtk::ListBox      m_list;
    Gtk::Label        m_empty;
    Gtk::SpinButton   m_tol;
    Gtk::Button       m_load_button, m_make_button, m_mask_button, m_save_button, m_remove_button;
    sigc::signal<void(int)> m_pick, m_mask, m_save, m_remove;
    sigc::signal<void(int, int)> m_load;
    sigc::signal<void()> m_make;
    sigc::signal<void(int, const std::string&)> m_rename;
    sigc::signal<void(double)> m_tolerance;
};

}  // namespace klimt
