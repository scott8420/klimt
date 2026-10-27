#pragma once
// core::Tools -- the TOOL REGISTRY (s005): every tool Klimt offers, once.
// Three consumers read it and none keeps its own list: the tool rail (its
// buttons, in groups), the window's tool keys (V, M, C ...), and the shortcut
// reference (core::shortcut_registry appends a row per tool). GTK-free, so the
// selftest guards it: ids and keys unique, every icon in the resource bundle.
//
// Klimt's tools only -- the scope wall: no shape tools, no pen-as-art
// (REQUIREMENTS "Never"). A tool's VARIANTS (Rectangle / Lasso / Wand ...)
// are not separate tools: the context strip offers them (docs/ui.md 3-4).
#include <string>
#include <vector>

namespace klimt::core {

struct ToolSpec {
    std::string              id;        // "select" -- stable
    std::string              name;      // "Select" -- what the tooltip says
    std::string              icon;      // "klimt-tool-select-symbolic" (resources/)
    char                     key = 0;   // 'm' -- lower case; the window hears it
    int                      group = 0; // 0 move/select/crop, 1 retouch, 2 paint/type/sample, 3 view
    std::vector<std::string> variants;  // the context strip's choices; empty = none
    std::string              note;      // what the strip says while the tool has no pixels to act on
};

const std::vector<ToolSpec>& tools();               // in rail order
const ToolSpec* find_tool(const std::string& id);   // nullptr if none
const ToolSpec* tool_for_key(char key);             // case-insensitive; nullptr if none

}  // namespace klimt::core
