#pragma once
// ActionsPanel -- the dock's Actions tab (s084; docs/actions.md). The
// library's actions, each opening onto its steps (a tick switches a step off);
// the footer: Stop, Record, Play, New, Batch…, Delete. A view: the Shell owns
// the library and says what to show; every choice goes back through on().
#include "core/Actions.hpp"
#include "widgets/Widgets.hpp"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace klimt {

class ActionsPanel : public widgets::Box {
public:
    ActionsPanel();
    // What the panel asks for. `action` / `step`: the picked ones (-1: none);
    // `text`: Rename's new name.
    enum class Do { Play, Record, Stop, New, Delete, Rename, Toggle, Batch };
    void set_on(std::function<void(Do, int action, int step, const std::string& text)> f) { m_on = std::move(f); }
    // The library to show; `recording` the action being recorded (-1: none);
    // `playing`: an action running (Play greyed).
    void show(const std::vector<core::actions::Action>& all, int recording, bool playing);
    int picked_action() const { return m_action; }
    int picked_step() const { return m_step; }
    void pick(int action, int step = -1);
    void close_all();                             // s093: every action shut onto its row (the canvas sweep, between scenes)

private:
    widgets::ScrolledWindow m_scroll;
    widgets::ListBox        m_list;
    widgets::Label          m_status;
    Gtk::Button*            m_stop = nullptr;
    Gtk::Button*            m_record = nullptr;
    Gtk::Button*            m_play = nullptr;
    Gtk::Button*            m_delete = nullptr;
    Gtk::Button*            m_batch = nullptr;
    std::vector<core::actions::Action> m_all;
    std::set<int>           m_open;               // actions opened onto their steps
    std::vector<std::pair<int, int>> m_rows;      // row -> (action, step; -1 the action's own row)
    int                     m_action = -1, m_step = -1;
    int                     m_recording = -1;
    bool                    m_playing = false;
    bool                    m_painting = false;
    bool                    m_renaming = false;   // s093: a name being edited -- a double-click there renames, never plays
    std::function<void(Do, int, int, const std::string&)> m_on;
    void paint();
    void sync_buttons();
    void ask(Do d, const std::string& text = {});
};

}  // namespace klimt
