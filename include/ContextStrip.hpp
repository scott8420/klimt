#pragma once
// ContextStrip -- the current tool's options, one row under the header (s005,
// docs/ui.md 4; Photoshop's options bar, slimmer). Rebuilt when the tool
// changes: the tool's name, its variants as one linked group, then what it
// does. Each tool remembers its variant (view state). The options that act on
// pixels arrive with pixels; until then the strip says so instead of faking.
#include "widgets/Widgets.hpp"
#include "core/Tools.hpp"
#include "core/BrushPresets.hpp"   // s021
#include "core/ToolDefaults.hpp"   // s029

#include <gtkmm/popover.h>

#include <functional>
#include <map>
#include <sigc++/signal.h>
#include <string>

namespace klimt {

class ContextStrip : public widgets::Box {
public:
    ContextStrip();
    void show_tool(const core::ToolSpec& tool);
    int variant(const std::string& tool_id) const;   // 0 if never chosen
    // s035: a key chose the variant (the Pen's P / A / Shift+P): the strip's
    // buttons follow, and the variant is told as if clicked.
    void set_variant(const std::string& tool_id, int v);
    // s035 (Scott): Ctrl / Alt held with the Pen -- the button of the mode in
    // effect lights for as long; the chosen variant stays as it was.
    void show_variant(const std::string& tool_id, int v);
    // s008 m3: the Magic Wand's options (view state, remembered).
    struct Wand { int tolerance = 32; bool contiguous = true; bool all_layers = false; };
    const Wand& wand() const { return m_wand; }
    // s012: the Gradient tool's options (view state, remembered).
    struct GradientOpts { int opacity = 100; bool reverse = false; bool to_transparent = false; };
    const GradientOpts& gradient() const { return m_gradient; }
    // s010: the Crop tool's "Delete cropped pixels" (off: they're kept beyond the image).
    bool crop_deletes() const { return m_crop_delete; }
    // s038: the Refine brush -- how far the rough area may cut into the
    // figure (Edge, px), the soft edge (px); its size is brush("refine").size.
    // s039 m3: `overlay` -- while the Refine tool is in hand, what is NOT
    // selected shows under Quick Mask's red (Pixelmator's overlay).
    struct RefineOpts { int band = 12; int soft = 0; bool overlay = true; };
    const RefineOpts& refine() const { return m_refine; }
    sigc::signal<void()>& signal_refine_overlay() { return m_refine_overlay; }   // s039 m3: the overlay turned on / off
    // s008 m4: the Brush's and the Eraser's own options (each remembers).
    struct BrushOpts {
        int size = 30; int hardness = 80; int opacity = 100;
        bool pressure_size = true, pressure_opacity = false;   // s009 m3: what the pen's pressure drives
        int flow = 100, smoothing = 0;                          // s012 m1: build-up; the stabiliser (%)
        int tip = 0, angle = 0, roundness = 100;                // s017: core::paint::Tip, degrees, %
        bool tilt = false;                                      // s017: the pen's lean shapes the tip
        int strength = 50;                                      // s017: Smudge's strength (%)
        // s028: DYNAMICS (core::paint::Dynamics in %; scatter 0..1000 %, count 1..16).
        int size_jitter = 0, min_size = 0, angle_jitter = 0, roundness_jitter = 0, min_roundness = 25;
        int scatter = 0, count = 1, count_jitter = 0;
        bool both_axes = false;
        int fg_bg = 0, hue = 0, sat = 0, bri = 0;
        bool per_tip = true;
        // s033: TEXTURE (core::paint::Texture): a pattern's key -- a built-in's, or "u:" + yours.
        bool tex_on = false;
        std::string tex_pattern = "paper";
        int tex_scale = 100, tex_depth = 50;
        bool tex_subtract = false, tex_invert = false;
        bool dynamics() const { return size_jitter || angle_jitter || roundness_jitter || scatter || count > 1 || fg_bg || hue || sat || bri; }
    };
    BrushOpts brush(const std::string& tool) const;
    // s015: the Clone Stamp's own options (its brush is brush("clone")).
    struct CloneOpts { bool aligned = true; bool all_layers = false; };
    const CloneOpts& clone() const { return m_clone; }
    sigc::signal<void()>& signal_clone_changed() { return m_clone_signal; }
    void set_brush_size(const std::string& tool, int size);   // [ and ] -- the strip follows
    sigc::signal<void()>& signal_brush_changed() { return m_brush_signal; }
    // s021: BRUSH PRESETS -- the list (app settings, ~/.config/klimt/brushes.json),
    // shown in the brush tools' strip; picking one sets the tool's options.
    void set_presets_file(const std::string& file);
    const std::vector<core::BrushPreset>& presets() const { return m_presets; }
    void apply_preset(const std::string& tool, std::size_t i);   // the tool's options become preset i
    std::size_t save_preset(const std::string& tool, const std::string& name);   // the tool's options as a new preset; its index
    void delete_preset(std::size_t i);
    // s029: the tool's SETTINGS -- every option on tabs (Brush, Tip, Shape,
    // Scatter, Colour, Source; Gradient, Crop, Magic Wand). The strip's
    // dropdown, a right-click on the tool in the rail, a right-click on the
    // canvas all show it. `pop` is the popover it sits in (closed before a
    // rebuild); `changed` is told after each change.
    Gtk::Widget* settings_panel(const std::string& tool, Gtk::Popover* pop, std::function<void()> changed = {});
    static bool has_settings(const std::string& tool);
    // s029: SAVED DEFAULTS (~/.config/klimt/tools.json) -- every tool starts
    // from them. Reset: the saved ones (none: the factory's); Save as
    // Default: the tool as it is; Factory: the built-in values, the saved
    // ones forgotten.
    void set_defaults_file(const std::string& file);
    void reset_tool(const std::string& tool);
    void save_tool_default(const std::string& tool);
    void factory_tool(const std::string& tool);
    bool has_saved_default(const std::string& tool) const;
    static bool is_brush_tool(const std::string& tool);
    static std::string tip_label(const BrushOpts& o);   // "Round 45% · jitter"
    // s008: told when the user picks a variant (tool id, index).
    sigc::signal<void(const std::string&, int)>& signal_variant() { return m_variant_signal; }
    // s035 m5: the Pen's Corner / Smooth buttons (true: Smooth).
    sigc::signal<void(bool)>& signal_pen_point() { return m_pen_point; }

private:
    sigc::signal<void(bool)> m_pen_point;
    sigc::signal<void()> m_refine_overlay;   // s039 m3
    std::vector<Gtk::ToggleButton*> m_variant_buttons;   // the shown tool's (managed by the strip)
    bool m_quiet = false;                                 // lighting a button, not choosing
    std::map<std::string, int> m_variant;
    Wand m_wand;
    bool m_crop_delete = false;
    RefineOpts m_refine;
    GradientOpts m_gradient;
    CloneOpts m_clone;
    sigc::signal<void()> m_clone_signal;
    std::map<std::string, BrushOpts> m_brush;
    std::vector<core::BrushPreset> m_presets = core::default_presets();
    std::string m_presets_file;
    void store_presets();
    void refresh_strip(const std::string& tool);
    core::ToolDefaults m_defaults;                            // s029
    std::string m_defaults_file;
    void apply_defaults(const std::string& tool, bool factory);   // s029: the strip shows `tool` again (at idle)
    Gtk::Widget* presets_button(const std::string& tool);   // the strip's "Brushes" button and its popover
    std::string m_shown;                                      // the tool the strip shows
    sigc::signal<void()> m_brush_signal;
    sigc::signal<void(const std::string&, int)> m_variant_signal;
};

}  // namespace klimt
