#pragma once
// core/BrushPresets -- saved brushes (s021). A preset is a brush's settings
// under a name: size, hardness, opacity, flow, smoothing, the tip (shape,
// angle, roundness), what the pen drives (pressure -> size / opacity, tilt),
// Smudge's strength; s028 its dynamics (shape, scatter, colour jitter). GTK-free; the Recents / Prefs pattern: encode and
// decode in ONE file, the UI hands in the path (~/.config/klimt/brushes.json).
//
// App settings, not the document: no undo step (as the dock's divider). A
// missing or broken file gives the defaults; loading clamps what it reads
// and drops what it can't; names stay unique. docs/brush-presets.md.
#include <string>
#include <vector>

namespace klimt::core {

struct BrushPreset {
    std::string name;
    int  size = 30, hardness = 80, opacity = 100, flow = 100, smoothing = 0;
    int  tip = 0, angle = 0, roundness = 100;            // core::paint::Tip, degrees, %
    bool pressure_size = true, pressure_opacity = false, tilt = false;
    int  strength = 50;                                   // Smudge (%)
    // s028: DYNAMICS (core::paint::Dynamics, in %; count 1..16; scatter 0..1000).
    int  size_jitter = 0, min_size = 0, angle_jitter = 0, roundness_jitter = 0, min_roundness = 25;
    int  scatter = 0, count = 1, count_jitter = 0;
    bool both_axes = false;
    int  fg_bg = 0, hue = 0, sat = 0, bri = 0;
    bool per_tip = true;
    // s033: TEXTURE -- a pattern's key (a built-in's, or "u:" + a library name).
    bool tex_on = false;
    std::string tex_pattern = "paper";
    int  tex_scale = 100, tex_depth = 50;   // %, 10..1000 / 0..100
    bool tex_subtract = false, tex_invert = false;
    bool operator==(const BrushPreset&) const = default;
    bool same_brush(const BrushPreset& o) const;          // all but the name
};

const std::vector<BrushPreset>& default_presets();
std::vector<BrushPreset> load_presets(const std::string& file);
bool save_presets(const std::string& file, const std::vector<BrushPreset>& v);

// `want` (trimmed; "Brush" when empty), or "want 2", "want 3"... -- not
// already in `v` (except at `skip`, a rename's own row).
std::string unique_preset_name(const std::vector<BrushPreset>& v, const std::string& want, int skip = -1);
BrushPreset clamped(BrushPreset p);   // every field within its range

}  // namespace klimt::core
