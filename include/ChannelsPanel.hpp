#pragma once
// ChannelsPanel -- the dock's Channels tab (s016): the SAVED SELECTIONS
// (Photoshop's alpha channels). Click a row: it becomes the selection;
// Shift-click adds, Alt-click subtracts, Shift+Alt intersects. Double-click
// renames. The buttons: save the selection as a new channel; delete the
// picked one. The Shell does the verbs (core::select) -- each ONE step.
// docs/selection.md.
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <sigc++/signal.h>

#include <string>
#include <vector>

namespace klimt {

class ChannelsPanel : public Gtk::Box {
public:
    struct Item { std::string id, name; };
    ChannelsPanel();
    // What the document holds now (in order) and whether there is a selection to save.
    void show(const std::vector<Item>& items, bool can_save);

    sigc::signal<void(const std::string&, int)>& signal_load() { return m_load; }      // id, select::Op as int
    sigc::signal<void()>& signal_save() { return m_save; }
    sigc::signal<void(const std::string&)>& signal_remove() { return m_remove; }
    sigc::signal<void(const std::string&, const std::string&)>& signal_rename() { return m_rename; }
    // s026: THE COLOUR CHANNELS above the saved ones: the picture ("RGB") and
    // each channel. Click: view it alone, as grey (the picture: back); Ctrl-
    // click or its button: load it as a selection (Shift adds, Alt
    // subtracts); its copy button: a saved channel of it.
    void show_colour(const std::vector<std::string>& names, const std::string& composite, int viewing);
    sigc::signal<void(int)>& signal_view() { return m_view; }               // -1: the picture
    sigc::signal<void(int, int)>& signal_load_colour() { return m_load_colour; }   // channel, select::Op as int
    sigc::signal<void(int)>& signal_dup_colour() { return m_dup_colour; }

private:
    void rename_row(int index);
    std::vector<Item>  m_items;
    std::vector<std::string> m_colour_names;
    Gtk::ListBox       m_colour;   // s026
    sigc::signal<void(int)> m_view, m_dup_colour;
    sigc::signal<void(int, int)> m_load_colour;
    Gtk::ListBox       m_list;
    Gtk::Label         m_empty;
    Gtk::Button        m_save_button, m_remove_button;
    sigc::signal<void(const std::string&, int)> m_load;
    sigc::signal<void()> m_save;
    sigc::signal<void(const std::string&)> m_remove;
    sigc::signal<void(const std::string&, const std::string&)> m_rename;
};

}  // namespace klimt
