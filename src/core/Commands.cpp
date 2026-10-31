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
            {"Refine to Edges", "Select", "win.refine-edges", "", "snap edge subject figure isolate rough lasso cut out select and mask", "Ctrl+Alt+R"},
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
            {"Surface Blur", "Filter", "win.filter-surface-blur", "", "filter blur smooth skin edge-preserving bilateral retouch", ""},
            {"Lens Blur", "Filter", "win.filter-lens-blur", "", "filter blur bokeh depth of field aperture iris blades highlights camera", ""},   // s037
            {"Radial Blur", "Filter", "win.filter-radial-blur", "", "filter blur spin zoom rotate wheel speed burst centre center motion", ""},   // s040
            {"Twirl", "Filter", "win.filter-twirl", "", "filter distort swirl whirl spiral rotate", ""},   // s040 m4
            {"Shear", "Filter", "win.filter-shear", "", "filter distort shear bend curve skew rows", ""},   // s044 m2
            {"Displace", "Filter", "win.filter-displace", "", "filter distort displace displacement map texture warp layer", ""},   // s043 m4
            {"Pinch", "Filter", "win.filter-pinch", "", "filter distort squeeze bulge shrink", ""},
            {"Spherize", "Filter", "win.filter-spherize", "", "filter distort sphere bulge fisheye lens dent", ""},
            {"Polar Coordinates", "Filter", "win.filter-polar-coordinates", "", "filter distort polar rectangular circle tiny planet unwrap", ""},   // s041 m3
            {"Ripple", "Filter", "win.filter-ripple", "", "filter distort ripple water wobble", ""},
            {"Wave", "Filter", "win.filter-wave", "", "filter distort wave sine triangle square wrap", ""},
            {"ZigZag", "Filter", "win.filter-zigzag", "", "filter distort zigzag pond ripples rings water drop", ""},
            {"Find Edges", "Filter", "win.filter-find-edges", "", "filter stylize edges outline sketch line drawing", ""},   // s042 m1
            {"Emboss", "Filter", "win.filter-emboss", "", "filter stylize emboss relief stamp metal", ""},
            {"Solarize", "Filter", "win.filter-solarize", "", "filter stylize solarize sabattier invert tones", ""},
            {"Diffuse", "Filter", "win.filter-diffuse", "", "filter stylize diffuse scatter frosted glass dither", ""},   // s042 m2
            {"Wind", "Filter", "win.filter-wind", "", "filter stylize wind blast stagger streaks speed", ""},
            {"Clouds", "Filter", "win.filter-clouds", "", "filter render clouds noise fractal smoke sky texture", ""},   // s042 m3
            {"Difference Clouds", "Filter", "win.filter-difference-clouds", "", "filter render clouds difference marble texture", ""},
            {"Lens Flare", "Filter", "win.filter-lens-flare", "", "filter render lens flare sun light glare ghosts anamorphic", ""},   // s044 m1
            {"Fibers", "Filter", "win.filter-fibers", "", "filter render fibers fibres fabric wood grain hair texture", ""},   // s043 m3
            {"Mosaic", "Filter", "win.filter-mosaic", "", "filter pixelate mosaic blocks pixels squares censor", ""},   // s043 m1
            {"Crystallize", "Filter", "win.filter-crystallize", "", "filter pixelate crystallize polygons cells voronoi stained glass", ""},
            {"Pointillize", "Filter", "win.filter-pointillize", "", "filter pixelate pointillize dots seurat painting", ""},
            {"Colour Halftone", "Filter", "win.filter-colour-halftone", "", "filter pixelate color colour halftone dots screen print comic", ""},   // s043 m2
            {"Facet", "Filter", "win.filter-facet", "", "filter pixelate facet blocks painted clump kuwahara", ""},
            {"Mezzotint", "Filter", "win.filter-mezzotint", "", "filter pixelate mezzotint dots lines strokes print grain", ""},
            {"Fragment", "Filter", "win.filter-fragment", "", "filter pixelate fragment shake double vision copies", ""},
            {"Median", "Filter", "win.filter-median", "", "filter noise despeckle dust remove smooth", ""},
            // s034
            {"Smart Sharpen", "Filter", "win.filter-smart-sharpen", "", "filter sharpen crisp detail lens deblur focus halo", ""},
            {"Dust & Scratches", "Filter", "win.filter-dust-scratches", "", "filter noise despeckle dust scratch scan remove", ""},
            {"Reduce Noise", "Filter", "win.filter-reduce-noise", "", "filter noise denoise grain photo colour chroma clean", ""},
            {"Maximum", "Filter", "win.filter-maximum", "", "filter other dilate grow spread light expand mask choke", ""},
            {"Minimum", "Filter", "win.filter-minimum", "", "filter other erode shrink spread dark contract mask choke", ""},
            // s030: filter layers -- the same filters, live, over what's below.
            {"New Filter Layer: Gaussian Blur", "Layer", "win.filter-layer-gaussian-blur", "", "filter layer live non-destructive blur soften", ""},
            {"New Filter Layer: Unsharp Mask", "Layer", "win.filter-layer-unsharp-mask", "", "filter layer live non-destructive sharpen crisp", ""},
            {"New Filter Layer: High Pass", "Layer", "win.filter-layer-high-pass", "", "filter layer live non-destructive sharpen overlay detail", ""},
            {"New Filter Layer: Add Noise", "Layer", "win.filter-layer-add-noise", "", "filter layer live non-destructive grain film", ""},
            {"New Filter Layer: Box Blur", "Layer", "win.filter-layer-box-blur", "", "filter layer live non-destructive blur average", ""},
            {"New Filter Layer: Motion Blur", "Layer", "win.filter-layer-motion-blur", "", "filter layer live non-destructive blur streak speed", ""},
            {"New Filter Layer: Surface Blur", "Layer", "win.filter-layer-surface-blur", "", "filter layer live non-destructive blur smooth skin edge-preserving", ""},
            {"New Filter Layer: Lens Blur", "Layer", "win.filter-layer-lens-blur", "", "filter layer live non-destructive blur bokeh depth of field aperture", ""},   // s037
            {"New Filter Layer: Radial Blur", "Layer", "win.filter-layer-radial-blur", "", "filter layer live non-destructive blur spin zoom centre center", ""},   // s040
            {"New Filter Layer: Twirl", "Layer", "win.filter-layer-twirl", "", "filter layer live non-destructive distort swirl whirl", ""},   // s040 m4
            {"New Filter Layer: Pinch", "Layer", "win.filter-layer-pinch", "", "filter layer live non-destructive distort squeeze", ""},
            {"New Filter Layer: Spherize", "Layer", "win.filter-layer-spherize", "", "filter layer live non-destructive distort sphere bulge", ""},
            {"New Filter Layer: Polar Coordinates", "Layer", "win.filter-layer-polar-coordinates", "", "filter layer live non-destructive distort polar", ""},   // s041 m3
            {"New Filter Layer: Ripple", "Layer", "win.filter-layer-ripple", "", "filter layer live non-destructive distort ripple", ""},
            {"New Filter Layer: Wave", "Layer", "win.filter-layer-wave", "", "filter layer live non-destructive distort wave", ""},
            {"New Filter Layer: ZigZag", "Layer", "win.filter-layer-zigzag", "", "filter layer live non-destructive distort zigzag pond", ""},
            {"New Filter Layer: Find Edges", "Layer", "win.filter-layer-find-edges", "", "filter layer live non-destructive stylize edges", ""},   // s042 m1
            {"New Filter Layer: Emboss", "Layer", "win.filter-layer-emboss", "", "filter layer live non-destructive stylize emboss relief", ""},
            {"New Filter Layer: Solarize", "Layer", "win.filter-layer-solarize", "", "filter layer live non-destructive stylize solarize", ""},
            {"New Filter Layer: Diffuse", "Layer", "win.filter-layer-diffuse", "", "filter layer live non-destructive stylize diffuse", ""},   // s042 m2
            {"New Filter Layer: Wind", "Layer", "win.filter-layer-wind", "", "filter layer live non-destructive stylize wind", ""},
            {"New Filter Layer: Clouds", "Layer", "win.filter-layer-clouds", "", "filter layer live non-destructive render clouds", ""},   // s042 m3
            {"New Filter Layer: Difference Clouds", "Layer", "win.filter-layer-difference-clouds", "", "filter layer live non-destructive render clouds difference", ""},
            {"New Filter Layer: Shear", "Layer", "win.filter-layer-shear", "", "filter layer live non-destructive distort shear bend", ""},   // s044 m2
            {"New Filter Layer: Lens Flare", "Layer", "win.filter-layer-lens-flare", "", "filter layer live non-destructive render lens flare sun", ""},   // s044 m1
            {"New Filter Layer: Fibers", "Layer", "win.filter-layer-fibers", "", "filter layer live non-destructive render fibers fibres", ""},   // s043 m3
            {"New Filter Layer: Mosaic", "Layer", "win.filter-layer-mosaic", "", "filter layer live non-destructive pixelate mosaic blocks", ""},   // s043 m1
            {"New Filter Layer: Crystallize", "Layer", "win.filter-layer-crystallize", "", "filter layer live non-destructive pixelate crystallize cells", ""},
            {"New Filter Layer: Pointillize", "Layer", "win.filter-layer-pointillize", "", "filter layer live non-destructive pixelate pointillize dots", ""},
            {"New Filter Layer: Colour Halftone", "Layer", "win.filter-layer-colour-halftone", "", "filter layer live non-destructive pixelate halftone", ""},   // s043 m2
            {"New Filter Layer: Facet", "Layer", "win.filter-layer-facet", "", "filter layer live non-destructive pixelate facet", ""},
            {"New Filter Layer: Mezzotint", "Layer", "win.filter-layer-mezzotint", "", "filter layer live non-destructive pixelate mezzotint", ""},
            {"New Filter Layer: Fragment", "Layer", "win.filter-layer-fragment", "", "filter layer live non-destructive pixelate fragment", ""},
            {"New Filter Layer: Median", "Layer", "win.filter-layer-median", "", "filter layer live non-destructive despeckle dust", ""},
            {"New Filter Layer: Smart Sharpen", "Layer", "win.filter-layer-smart-sharpen", "", "filter layer live non-destructive sharpen crisp lens deblur", ""},
            {"New Filter Layer: Dust & Scratches", "Layer", "win.filter-layer-dust-scratches", "", "filter layer live non-destructive despeckle dust scratch", ""},
            {"New Filter Layer: Reduce Noise", "Layer", "win.filter-layer-reduce-noise", "", "filter layer live non-destructive denoise grain photo", ""},
            {"New Filter Layer: Maximum", "Layer", "win.filter-layer-maximum", "", "filter layer live non-destructive dilate grow spread light", ""},
            {"New Filter Layer: Minimum", "Layer", "win.filter-layer-minimum", "", "filter layer live non-destructive erode shrink spread dark", ""},
            {"Last Filter", "Filter", "win.filter-last", "", "filter again repeat", "Ctrl+F"},
            // s035: paths
            {"Load Path as Selection", "Select", "win.path-selection", "", "path pen bezier curve selection outline", "Ctrl+Enter"},
            {"Show / Hide Path Outline", "View", "win.path-outline", "", "path outline hide show nodes anchors extras mask effect", "Ctrl+Shift+H"},
            {"Make Path from Selection", "Select", "win.path-make-work", "", "path pen bezier trace outline vector selection to path work path layer", ""},
            {"Last Filter's Settings", "Filter", "win.filter-last-dialog", "", "filter again repeat dialog", "Ctrl+Alt+F"},
            // s019: layer styles.
            {"Layer Style: Drop Shadow", "Layer", "win.style-drop-shadow", "", "effect fx shadow", ""},
            {"Layer Style: Outer Glow", "Layer", "win.style-outer-glow", "", "effect fx glow halo", ""},
            {"Layer Style: Inner Shadow", "Layer", "win.style-inner-shadow", "", "effect fx shadow inset", ""},
            {"Layer Style: Satin", "Layer", "win.style-satin", "", "effect fx satin silk sheen folds", ""},
            {"Layer Style: Gradient Overlay", "Layer", "win.style-gradient-overlay", "", "effect fx gradient blend ramp fade colour", ""},
            {"Layer Style: Pattern Overlay", "Layer", "win.style-pattern-overlay", "", "effect fx pattern texture paper canvas tile", ""},
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
            {"Rulers", "View", "win.view-rulers", "", "ruler units measure inches cm zero origin", "Ctrl+R"},
            {"Define Pattern", "Edit", "win.define-pattern", "", "pattern texture tile save selection library", ""},
            {"Show / Hide Guides", "View", "win.view-guides", "", "guides lines grid align", "Ctrl+;"},
            {"Lock Guides", "View", "win.guides-lock", "", "guides lock fix", "Ctrl+Alt+;"},
            {"Snap", "View", "win.guides-snap", "", "snap guides edges magnet align", "Ctrl+Shift+;"},
            {"Clear Guides", "View", "win.guides-clear", "", "guides remove delete all", ""},
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
