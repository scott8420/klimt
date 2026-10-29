#pragma once
// core/ToolDefaults -- each tool's SAVED DEFAULTS (s029). The tool's settings
// panel saves the tool as it is ("Save as Default"); Klimt starts every tool
// from them, and "Reset" goes back to them ("Factory": the built-in values,
// and the saved ones forgotten). GTK-free; the BrushPresets pattern: one file
// (~/.config/klimt/tools.json), written whole or not at all, no undo step.
// A missing or broken file: no saved defaults (the factory's).
//
// The brush tools keep a whole BrushPreset each (the presets' own encoding,
// clamped on reading); the others a few named numbers ("gradient": opacity,
// reverse, to_transparent; "crop": delete; "wand": tolerance, contiguous,
// all_layers; "source": aligned, all_layers -- the Clone Stamp's and Heal's).
// The UI clamps those when it applies them.
#include "core/BrushPresets.hpp"

#include <map>
#include <string>

namespace klimt::core {

struct ToolDefaults {
    std::map<std::string, BrushPreset> brushes;                    // by tool id
    std::map<std::string, std::map<std::string, int>> others;      // group -> name -> value (bools 0 / 1)
    bool operator==(const ToolDefaults&) const = default;
};

ToolDefaults load_tool_defaults(const std::string& file);
bool save_tool_defaults(const std::string& file, const ToolDefaults& d);

}  // namespace klimt::core
