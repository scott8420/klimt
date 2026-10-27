#pragma once
// ToolRail -- the tool column at the window's left (s005, docs/ui.md 3).
// Renders core::tools() in its groups: one toggle per tool, exactly one down.
// Choosing a tool is VIEW state (not a document change, not undoable). The
// colour pair sits at the foot (drawn; it gets its verbs with pixels).
#include "widgets/Widgets.hpp"

#include <gtkmm/drawingarea.h>
#include <gtkmm/togglebutton.h>
#include <sigc++/signal.h>

#include <map>
#include <string>

namespace klimt {

class ToolRail : public widgets::Box {
public:
    ToolRail();
    const std::string& tool() const { return m_tool; }
    void set_tool(const std::string& id);              // from a key: no echo back
    sigc::signal<void(const std::string&)>& signal_tool() { return m_signal; }

private:
    std::map<std::string, Gtk::ToggleButton*> m_buttons;   // managed by the rail
    std::string m_tool;
    bool        m_setting = false;
    sigc::signal<void(const std::string&)> m_signal;
    Gtk::DrawingArea m_colours;
};

}  // namespace klimt
