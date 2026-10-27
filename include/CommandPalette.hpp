#pragma once
// CommandPalette -- Ctrl+K: type to find any verb (s005, docs/ui.md 7). A
// small window over the Shell: a search field and the ranked matches from
// core::search(). Enter (or a click) runs the top / chosen one through the
// window's own action, so a command here is exactly the button / key / menu.
// Hide-on-close singleton (the About window's lifetime stone).
#include "widgets/Widgets.hpp"

#include <gtkmm/searchentry.h>
#include <gtkmm/applicationwindow.h>

namespace klimt {

class CommandPalette : public Gtk::Window {
public:
    explicit CommandPalette(Gtk::ApplicationWindow& parent);
    void open();                         // clear, fill, present, focus the field

private:
    void refill();
    void run_row(Gtk::ListBoxRow* row);
    void move(int by);

    Gtk::ApplicationWindow& m_parent;
    Gtk::SearchEntry        m_query;
    widgets::ScrolledWindow m_scroll;
    widgets::ListBox        m_list;
};

}  // namespace klimt
