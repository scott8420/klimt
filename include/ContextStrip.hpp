#pragma once
// ContextStrip -- the current tool's options, one row under the header (s005,
// docs/ui.md 4; Photoshop's options bar, slimmer). Rebuilt when the tool
// changes: the tool's name, its variants as one linked group, then what it
// does. Each tool remembers its variant (view state). The options that act on
// pixels arrive with pixels; until then the strip says so instead of faking.
#include "widgets/Widgets.hpp"
#include "core/Tools.hpp"

#include <map>
#include <sigc++/signal.h>
#include <string>

namespace klimt {

class ContextStrip : public widgets::Box {
public:
    ContextStrip();
    void show_tool(const core::ToolSpec& tool);
    int variant(const std::string& tool_id) const;   // 0 if never chosen
    // s008 m3: the Magic Wand's options (view state, remembered).
    struct Wand { int tolerance = 32; bool contiguous = true; bool all_layers = false; };
    const Wand& wand() const { return m_wand; }
    // s008 m4: the Brush's and the Eraser's own options (each remembers).
    struct BrushOpts {
        int size = 30; int hardness = 80; int opacity = 100;
        bool pressure_size = true, pressure_opacity = false;   // s009 m3: what the pen's pressure drives
    };
    BrushOpts brush(const std::string& tool) const;
    void set_brush_size(const std::string& tool, int size);   // [ and ] -- the strip follows
    sigc::signal<void()>& signal_brush_changed() { return m_brush_signal; }
    // s008: told when the user picks a variant (tool id, index).
    sigc::signal<void(const std::string&, int)>& signal_variant() { return m_variant_signal; }

private:
    std::map<std::string, int> m_variant;
    Wand m_wand;
    std::map<std::string, BrushOpts> m_brush;
    std::string m_shown;                                      // the tool the strip shows
    sigc::signal<void()> m_brush_signal;
    sigc::signal<void(const std::string&, int)> m_variant_signal;
};

}  // namespace klimt
