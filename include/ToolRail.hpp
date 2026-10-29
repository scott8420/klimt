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
#include <utility>
#include <string>

namespace klimt {

class ToolRail : public widgets::Box {
public:
    ToolRail();
    const std::string& tool() const { return m_tool; }
    void set_tool(const std::string& id);              // from a key: no echo back
    sigc::signal<void(const std::string&)>& signal_tool() { return m_signal; }
    // s029: a right-click on a tool (the tool, its button) -- its settings.
    sigc::signal<void(const std::string&, Gtk::Widget*)>& signal_settings() { return m_settings_signal; }

    // s008: the colour pair is real -- what Fill (Alt+Backspace) and Delete
    // on a layer with no alpha use. sRGB 0..1. X swaps, D resets (black /
    // white); m3 makes them pickable. View state, not a document change.
    struct RGB { float r = 0, g = 0, b = 0; };
    RGB foreground() const { return m_fg; }
    RGB background() const { return m_bg; }
    void set_foreground(RGB c) { m_fg = c; m_colours.queue_draw(); m_colours_signal.emit(); }
    void set_background(RGB c) { m_bg = c; m_colours.queue_draw(); m_colours_signal.emit(); }
    void swap_colours() { std::swap(m_fg, m_bg); m_colours.queue_draw(); m_colours_signal.emit(); }
    void reset_colours() { m_fg = {0, 0, 0}; m_bg = {1, 1, 1}; m_colours.queue_draw(); m_colours_signal.emit(); }
    // s024: told when the pair changes (the Eyedropper, X, D, a swatch) -- Color Range follows it.
    sigc::signal<void()>& signal_colours() { return m_colours_signal; }
    // s008 m3: a click on a swatch picks it (GTK's colour dialog).
    void pick(bool foreground);

private:
    std::map<std::string, Gtk::ToggleButton*> m_buttons;   // managed by the rail
    std::string m_tool;
    bool        m_setting = false;
    sigc::signal<void(const std::string&)> m_signal;
    sigc::signal<void(const std::string&, Gtk::Widget*)> m_settings_signal;   // s029
    Gtk::DrawingArea m_colours;
    RGB m_fg{0, 0, 0}, m_bg{1, 1, 1};
    sigc::signal<void()> m_colours_signal;
};

}  // namespace klimt
