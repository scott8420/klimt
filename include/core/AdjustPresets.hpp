#pragma once
// core/AdjustPresets -- saved adjustment settings (s065; WEAKNESSES 34). A
// preset is one adjustment's settings under a name, for its kind: the
// Preset row on every adjustment's controls saves the settings in hand and
// picks them again, in Properties and in Image > Adjustments alike.
// GTK-free, the BrushPresets pattern: ONE file, the UI hands in the path
// (~/.config/klimt/adjustments.json). App settings, not the document: no
// undo step. A missing or broken file: none; what a preset holds is checked
// as a file's adjustment is (adjust::refusal) -- a bad one is dropped.
#include "core/Adjust.hpp"

#include <string>
#include <vector>

namespace klimt::core {

struct AdjustPreset {
    std::string name;
    Adjust      a;
    bool operator==(const AdjustPreset&) const = default;
};

std::vector<AdjustPreset> load_adjust_presets(const std::string& file);
bool save_adjust_presets(const std::string& file, const std::vector<AdjustPreset>& v);
// `want` (trimmed; the kind's label when empty), or "want 2", "want 3"... --
// not already a preset of that kind in `v`.
std::string unique_adjust_preset_name(const std::vector<AdjustPreset>& v, AdjustKind k, const std::string& want);

}  // namespace klimt::core
