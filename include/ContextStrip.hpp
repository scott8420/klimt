#pragma once
// ContextStrip -- the current tool's options, one row under the header (s005,
// docs/ui.md 4; Photoshop's options bar, slimmer). Rebuilt when the tool
// changes: the tool's name, its variants as one linked group, then what it
// does. Each tool remembers its variant (view state). The options that act on
// pixels arrive with pixels; until then the strip says so instead of faking.
#include "widgets/Widgets.hpp"
#include "core/Tools.hpp"

#include <map>
#include <string>

namespace klimt {

class ContextStrip : public widgets::Box {
public:
    ContextStrip();
    void show_tool(const core::ToolSpec& tool);
    int variant(const std::string& tool_id) const;   // 0 if never chosen

private:
    std::map<std::string, int> m_variant;
};

}  // namespace klimt
