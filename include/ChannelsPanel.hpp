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

private:
    void rename_row(int index);
    std::vector<Item>  m_items;
    Gtk::ListBox       m_list;
    Gtk::Label         m_empty;
    Gtk::Button        m_save_button, m_remove_button;
    sigc::signal<void(const std::string&, int)> m_load;
    sigc::signal<void()> m_save;
    sigc::signal<void(const std::string&)> m_remove;
    sigc::signal<void(const std::string&, const std::string&)> m_rename;
};

}  // namespace klimt
