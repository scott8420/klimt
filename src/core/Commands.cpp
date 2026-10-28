#include "core/Commands.hpp"
#include "core/Shortcuts.hpp"
#include "core/Tools.hpp"

#include <algorithm>

namespace klimt::core {

namespace {
// The key a wired action has in the shortcut registry, displayed.
std::string keys_for(const std::string& action) {
    for (const auto& s : shortcut_registry())
        if (s.action == action && !s.accels.empty()) return s.display_keys();
    return {};
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Subsequence score of q in text; -1 if q isn't a subsequence.
int score_in(const std::string& q, const std::string& text) {
    int score = 0, run = 0;
    std::size_t at = 0;
    for (const char qc : q) {
        if (qc == ' ') { run = 0; continue; }
        bool found = false;
        for (; at < text.size(); ++at) {
            if (lower(text[at]) != lower(qc)) { run = 0; continue; }
            const bool word_start = at == 0 || text[at - 1] == ' ' || text[at - 1] == '/' || text[at - 1] == '-';
            score += 1 + (word_start ? 6 : 0) + run * 3;
            ++run;
            ++at;
            found = true;
            break;
        }
        if (!found) return -1;
    }
    return score;
}
}  // namespace

const std::vector<Command>& commands() {
    static const std::vector<Command> c = [] {
        std::vector<Command> v = {
            {"New Image", "File", "win.new", "", "create blank canvas document", ""},
            {"Open", "File", "win.open", "", "load file image photo png jpeg tiff", ""},
            {"Save", "File", "win.save", "", "write keep klimt", ""},
            {"Save As", "File", "win.save-as", "", "write copy rename klimt", ""},
            {"Export", "File", "win.export", "", "png jpeg jpg tiff flatten write image", ""},
            {"Close Tab", "File", "win.tab-close", "", "close document image window", "Ctrl+W"},
            {"Next Tab", "File", "win.tab-next", "", "switch document image other", "Ctrl+Tab"},
            {"Previous Tab", "File", "win.tab-prev", "", "switch document image other back", "Ctrl+Shift+Tab"},
            {"Undo", "Edit", "win.undo", "", "back revert", "Ctrl+Z"},
            {"Fade", "Edit", "win.fade", "", "last filter stroke opacity blend mode lessen weaken", "Ctrl+Shift+F"},
            {"Fill with Foreground", "Edit", "win.fill-fg", "", "paint colour bucket", "Alt+Backspace"},
            {"Fill with Background", "Edit", "win.fill-bg", "", "paint colour bucket", "Ctrl+Backspace"},
            {"Delete Selected Pixels", "Edit", "win.delete-selection", "", "clear erase cut", "Delete"},
            {"Select All", "Select", "win.select-all", "", "everything whole", "Ctrl+A"},
            {"Deselect", "Select", "win.deselect", "", "none clear drop", ""},
            {"Inverse", "Select", "win.select-invert", "", "invert flip opposite", ""},
            {"Feather", "Select", "win.select-feather", "", "soften blur edge radius", ""},
            {"Expand Selection", "Select", "win.select-expand", "", "modify grow enlarge bigger", ""},
            {"Contract Selection", "Select", "win.select-contract", "", "modify shrink smaller", ""},
            {"Border Selection", "Select", "win.select-border", "", "modify edge band outline ring", ""},
            {"Color Range", "Select", "win.select-range", "", "colour color pick sample similar fuzziness range", ""},
            {"Save Selection", "Select", "win.select-save", "", "channel alpha keep store remember", ""},
            {"Quick Mask", "Select", "win.quick-mask", "", "paint selection mask red overlay brush", "Q"},
            {"Smooth Selection", "Select", "win.select-smooth", "", "modify clean specks holes round", ""},
            {"Open Demo", "Help", "win.open-demo", "", "sample example portrait", ""},
            // s013: adjustments.
            {"New Adjustment Layer: Brightness/Contrast", "Layer", "win.adjust-new-brightness-contrast", "", "adjustment layer light dark", ""},
            {"New Adjustment Layer: Levels", "Layer", "win.adjust-new-levels", "", "adjustment layer histogram black white point gamma", ""},
            {"New Adjustment Layer: Curves", "Layer", "win.adjust-new-curves", "", "adjustment layer tone contrast", ""},
            {"New Adjustment Layer: Hue/Saturation", "Layer", "win.adjust-new-hue-saturation", "", "adjustment layer colour colorize tint", ""},
            {"Brightness/Contrast", "Image", "win.adjust-apply-brightness-contrast", "", "adjustments light dark pixels", ""},
            {"Levels", "Image", "win.adjust-apply-levels", "", "adjustments histogram black white point gamma pixels", ""},
            {"Curves", "Image", "win.adjust-apply-curves", "", "adjustments tone contrast pixels", ""},
            {"Hue/Saturation", "Image", "win.adjust-apply-hue-saturation", "", "adjustments colour colorize tint pixels", ""},
            // s014: the Filter menu.
            {"Gaussian Blur", "Filter", "win.filter-gaussian-blur", "", "filter blur soften smooth", ""},
            {"Unsharp Mask", "Filter", "win.filter-unsharp-mask", "", "filter sharpen crisp detail", ""},
            {"High Pass", "Filter", "win.filter-high-pass", "", "filter other detail edges sharpen grey", ""},
            {"Add Noise", "Filter", "win.filter-add-noise", "", "filter grain film speckle dither", ""},
            {"Box Blur", "Filter", "win.filter-box-blur", "", "filter blur average square soften", ""},
            {"Motion Blur", "Filter", "win.filter-motion-blur", "", "filter blur streak speed movement angle", ""},
            {"Median", "Filter", "win.filter-median", "", "filter noise despeckle dust remove smooth", ""},
            {"Last Filter", "Filter", "win.filter-last", "", "filter again repeat", "Ctrl+F"},
            {"Last Filter's Settings", "Filter", "win.filter-last-dialog", "", "filter again repeat dialog", "Ctrl+Alt+F"},
            // s019: layer styles.
            {"Layer Style: Drop Shadow", "Layer", "win.style-drop-shadow", "", "effect fx shadow", ""},
            {"Layer Style: Outer Glow", "Layer", "win.style-outer-glow", "", "effect fx glow halo", ""},
            {"Layer Style: Inner Shadow", "Layer", "win.style-inner-shadow", "", "effect fx shadow inset", ""},
            {"Layer Style: Inner Glow", "Layer", "win.style-inner-glow", "", "effect fx glow inside light", ""},
            {"Layer Style: Bevel & Emboss", "Layer", "win.style-bevel-emboss", "", "effect fx bevel emboss raise relief 3d", ""},
            {"Layer Style: Color Overlay", "Layer", "win.style-colour-overlay", "", "effect fx colour fill tint", ""},
            {"Layer Style: Stroke", "Layer", "win.style-stroke", "", "effect fx outline border", ""},
            {"Copy Layer Style", "Layer", "win.style-copy", "", "effect fx styles", ""},
            {"Paste Layer Style", "Layer", "win.style-paste", "", "effect fx styles", ""},
            {"Clear Layer Style", "Layer", "win.style-clear", "", "effect fx styles remove", ""},
            {"Rasterize Layer Style", "Layer", "win.style-rasterize", "", "effect fx styles bake flatten pixels", ""},
            // s012: layer masks.
            {"Add Layer Mask", "Layer", "win.mask-add", "", "mask reveal selection hide", ""},
            {"Layer Mask: Reveal All", "Layer", "win.mask-reveal-all", "", "mask add white", ""},
            {"Layer Mask: Hide All", "Layer", "win.mask-hide-all", "", "mask add black", ""},
            {"Layer Mask: Reveal Selection", "Layer", "win.mask-reveal-sel", "", "mask add", ""},
            {"Layer Mask: Hide Selection", "Layer", "win.mask-hide-sel", "", "mask add", ""},
            {"Paint the Mask / the Pixels", "Layer", "win.mask-edit", "", "mask edit target switch", ""},
            {"Disable / Enable Layer Mask", "Layer", "win.mask-toggle", "", "mask off on", ""},
            {"Invert Layer Mask", "Layer", "win.mask-invert", "", "mask negative", ""},
            {"Apply Layer Mask", "Layer", "win.mask-apply", "", "mask bake merge", ""},
            {"Delete Layer Mask", "Layer", "win.mask-delete", "", "mask remove discard", ""},
            // s011: transforms.
            {"Free Transform", "Edit", "win.free-transform", "", "scale rotate resize skew handles ctrl+t", ""},
            {"Flip Horizontal", "Edit", "win.flip-h", "", "mirror transform", ""},
            {"Flip Vertical", "Edit", "win.flip-v", "", "mirror upside down transform", ""},
            {"Rotate 90° Clockwise", "Edit", "win.rotate-cw", "", "turn right transform", ""},
            {"Rotate 90° Counter Clockwise", "Edit", "win.rotate-ccw", "", "turn left anticlockwise transform", ""},
            {"Rotate 180°", "Edit", "win.rotate-180", "", "turn upside down transform", ""},
            // s009 m2: the Image menu.
            {"Image Size", "Image", "win.image-size", "", "resize resample scale resolution dpi pixels", ""},
            {"Canvas Size", "Image", "win.canvas-size", "", "extend border expand trim anchor", ""},
            {"Crop to Selection", "Image", "win.crop-selection", "", "trim cut bounds", ""},
            {"Reveal All", "Image", "win.reveal-all", "", "show hidden outside beyond canvas expand", ""},
            {"Rotate Canvas 180°", "Image", "win.canvas-180", "", "image rotation turn upside down", ""},
            {"Rotate Canvas 90° Clockwise", "Image", "win.canvas-cw", "", "image rotation turn right portrait landscape", ""},
            {"Rotate Canvas 90° Counter Clockwise", "Image", "win.canvas-ccw", "", "image rotation turn left portrait landscape", ""},
            {"Rotate Canvas…", "Image", "win.canvas-rotate", "", "image rotation arbitrary angle degrees", ""},
            {"Flip Canvas Horizontal", "Image", "win.canvas-flip-h", "", "image rotation mirror", ""},
            {"Flip Canvas Vertical", "Image", "win.canvas-flip-v", "", "image rotation mirror upside down", ""},
            {"Redo", "Edit", "win.redo", "", "again forward", "Ctrl+Shift+Z"},
            {"New Layer", "Layer", "win.layer-new", "", "add create", ""},
            {"New Group", "Layer", "win.layer-new-group", "", "folder set add", ""},
            {"Duplicate", "Layer", "win.layer-duplicate", "", "copy clone", ""},
            {"Group Layers", "Layer", "win.layer-group", "", "folder combine", ""},
            {"Ungroup", "Layer", "win.layer-ungroup", "", "release folder", ""},
            {"Clip / Release", "Layer", "win.layer-clip", "", "clipping mask", ""},
            {"Rename", "Layer", "win.layer-rename", "", "name title", "F2"},
            {"Sort by Name", "Layer", "win.layer-sort", "", "alphabetical order arrange", ""},
            {"Bring Forward", "Layer", "win.layer-raise", "", "raise up front order", ""},
            {"Send Backward", "Layer", "win.layer-lower", "", "lower down back order", ""},
            {"Delete", "Layer", "win.layer-delete", "", "remove trash erase", "Delete"},
            {"Hide / Show Panels", "View", "win.toggle-panels", "", "focus fullscreen distraction", "Tab"},
            {"Zoom In", "View", "win.zoom-in", "", "magnify bigger closer", ""},
            {"Zoom Out", "View", "win.zoom-out", "", "smaller further", ""},
            {"Fit on Screen", "View", "win.zoom-fit", "", "zoom whole window", ""},
            {"Actual Size", "View", "win.zoom-actual", "", "100% zoom pixels one", ""},
            {"Keyboard Shortcuts", "Help", "win.shortcuts", "", "keys reference", ""},
            {"About Klimt", "Help", "win.about", "", "version build", ""},
            {"Dump Registry", "Diagnostics", "win.dump-registry", "", "debug widgets log", ""},
            {"Slow Job: Sort by Name", "Diagnostics", "win.layer-sort-slow", "", "test progress cancel", ""},
            {"Quit", "App", "win.quit", "", "exit close", ""},
        };
        for (auto& cmd : v)
            if (cmd.keys.empty()) cmd.keys = keys_for(cmd.action);
        // One row per tool, from the tool registry.
        for (const auto& t : tools())
            v.push_back({t.name + " Tool", "Tools", "win.tool", t.id,
                         "tool " + [&] { std::string s; for (const auto& x : t.variants) s += x + " "; return s; }(),
                         std::string(1, static_cast<char>(t.key - 'a' + 'A'))});
        return v;
    }();
    return c;
}

int match_score(const std::string& query, const Command& c) {
    if (query.find_first_not_of(' ') == std::string::npos) return 0;
    const int t = score_in(query, c.title);
    if (t >= 0) return t * 2 + 10;            // the title is what people mean
    return score_in(query, c.keywords + " " + c.section);
}

std::vector<const Command*> search(const std::string& query) {
    struct Hit { int score; std::size_t order; const Command* c; };
    std::vector<Hit> hits;
    const auto& all = commands();
    for (std::size_t i = 0; i < all.size(); ++i) {
        const int s = match_score(query, all[i]);
        if (s >= 0) hits.push_back({s, i, &all[i]});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    std::vector<const Command*> out;
    for (const auto& h : hits) out.push_back(h.c);
    return out;
}

}  // namespace klimt::core
