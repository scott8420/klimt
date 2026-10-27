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
            {"Undo", "Edit", "win.undo", "", "back revert", "Ctrl+Z"},
            {"Fill with Foreground", "Edit", "win.fill-fg", "", "paint colour bucket", "Alt+Backspace"},
            {"Fill with Background", "Edit", "win.fill-bg", "", "paint colour bucket", "Ctrl+Backspace"},
            {"Delete Selected Pixels", "Edit", "win.delete-selection", "", "clear erase cut", "Delete"},
            {"Select All", "Select", "win.select-all", "", "everything whole", "Ctrl+A"},
            {"Deselect", "Select", "win.deselect", "", "none clear drop", ""},
            {"Inverse", "Select", "win.select-invert", "", "invert flip opposite", ""},
            {"Feather", "Select", "win.select-feather", "", "soften blur edge radius", ""},
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
