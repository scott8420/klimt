#pragma once
// HistoryPanel -- every step, click to go back to it (s005; REQUIREMENTS
// added 7). A view of the journal: the first row is the document as opened,
// then one row per step; the current point is selected, steps after it
// (redoable) are dimmed. A click asks the Shell to walk the journal there --
// undo() / redo() through the one door, never a second way back.
#include "widgets/Widgets.hpp"
#include "core/Document.hpp"

#include <functional>
#include <string>
#include <vector>
#include <gtkmm/button.h>

namespace klimt {

class HistoryPanel : public widgets::Box {
public:
    explicit HistoryPanel(const core::DocJournal& journal);
    void repaint();      // after every journal change: once per idle, after the writes
    void set_on_go(std::function<void(std::size_t)> f) { m_go = std::move(f); }
    // s077: SNAPSHOTS -- named states of the document, listed above the steps
    // (a click goes back to one: a step, undoable); the footer's camera takes
    // one, its bin deletes the one last clicked. The Shell keeps them.
    enum class Snap { Take, Restore, Delete };
    void set_snapshots(const std::vector<std::string>& names);
    void set_on_snapshot(std::function<void(Snap, std::size_t)> f) { m_snap = std::move(f); }

private:
    const core::DocJournal& m_journal;
    widgets::ScrolledWindow m_scroll;
    widgets::ListBox        m_list;
    bool                    m_painting = false;
    bool                    m_queued = false;
    void paint();
    std::function<void(std::size_t)> m_go;
    std::function<void(Snap, std::size_t)> m_snap;     // s077
    widgets::ListBox        m_snaps{"history.snapshots"};
    Gtk::Button*            m_delete = nullptr;
    std::vector<std::string> m_names;
    int                     m_picked = -1;            // the snapshot last clicked
    bool                    m_snap_painting = false;
};

}  // namespace klimt
