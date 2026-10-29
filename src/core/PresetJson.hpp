#pragma once
// core -- a brush preset as JSON (s029: shared by brushes.json and the tool
// defaults). Internal to klimt_core: nlohmann/json is a private include.
#include "core/BrushPresets.hpp"
#include "json.hpp"

namespace klimt::core {
nlohmann::json preset_to_json(const BrushPreset& p);   // every field (dynamics only when set)
BrushPreset    preset_from_json(const nlohmann::json& e);   // what it can read, clamped; the name if it's a string
}  // namespace klimt::core
