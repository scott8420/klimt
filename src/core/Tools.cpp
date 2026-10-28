#include "core/Tools.hpp"

namespace klimt::core {

const std::vector<ToolSpec>& tools() {
    static const std::vector<ToolSpec> t = {
        {"move",       "Move",        "klimt-tool-move-symbolic",       'v', 0, {},
         "Drag to move the selected layers; with a selection, just its pixels. Arrows nudge (Shift: 10 px)."},
        {"select",     "Select",      "klimt-tool-select-symbolic",     'm', 0,
         {"Rectangle", "Ellipse", "Lasso", "Wand", "Colour Range"},
         "Drag to select (the Wand: click). Shift adds, Alt subtracts, both intersect; a click deselects."},
        {"crop",       "Crop",        "klimt-tool-crop-symbolic",       'c', 0, {"Crop", "Straighten"},
         "Crop: drag a box, drag inside / its edges, Enter or double-click crops, Esc drops it. Straighten: drag along what should be level."},
        {"heal",       "Heal",        "klimt-tool-heal-symbolic",       'j', 1, {"Spot", "Healing", "Patch"},
         "Spot: paint over a blemish -- it heals when you let go. Healing: Alt-click (or Ctrl-click) where to take from, then paint. Patch: drag a selection onto good pixels."},
        {"clone",      "Clone Stamp", "klimt-tool-clone-symbolic",      's', 1, {},
         "Alt-click (or Ctrl-click) where to take from, then paint."},
        {"brush",      "Brush",       "klimt-tool-brush-symbolic",      'b', 2, {},
         "Paint with the foreground colour; Shift-click draws a line from the last stroke."},
        {"eraser",     "Eraser",      "klimt-tool-eraser-symbolic",     'e', 2, {},
         "Erase to transparency (on a layer without it, to the background colour)."},
        {"gradient",   "Gradient",    "klimt-tool-gradient-symbolic",   'g', 2, {"Linear", "Radial"},
         "Drag from where the foreground colour is to where the background is (Shift: 45°) -- on the mask when you paint the mask."},
        {"text",       "Text",        "klimt-tool-text-symbolic",       't', 2, {},
         "A live text layer -- rasterize is your choice."},
        {"eyedropper", "Eyedropper",  "klimt-tool-eyedropper-symbolic", 'i', 2, {"Point", "3 x 3", "5 x 5"},
         "Click: the foreground colour from what you see; Alt-click: the background."},
        {"hand",       "Hand",        "klimt-tool-hand-symbolic",       'h', 3, {},
         "Drag to pan the view."},
        {"zoom",       "Zoom",        "klimt-tool-zoom-symbolic",       'z', 3, {"In", "Out"},
         "Click to zoom; Alt-click zooms out."},
    };
    return t;
}

const ToolSpec* find_tool(const std::string& id) {
    for (const auto& t : tools()) if (t.id == id) return &t;
    return nullptr;
}

const ToolSpec* tool_for_key(char key) {
    if (key >= 'A' && key <= 'Z') key = static_cast<char>(key - 'A' + 'a');
    for (const auto& t : tools()) if (t.key == key) return &t;
    return nullptr;
}

}  // namespace klimt::core
