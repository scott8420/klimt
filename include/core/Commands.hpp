#pragma once
// core::Commands -- the COMMAND CATALOG the palette searches (s005, docs/ui.md
// 7: type to find any verb; menus stay, but nobody has to know where something
// lives). Each command names the window action it runs (and a target for a
// parameterised one: win.tool("brush")). Keys come from the shortcut registry
// (one source); tool rows come from the tool registry. GTK-free: the selftest
// guards the catalog and the matcher.
#include <string>
#include <vector>

namespace klimt::core {

struct Command {
    std::string title;      // "Sort by Name"
    std::string section;    // "Layer"
    std::string action;     // "win.layer-sort"
    std::string target;     // parameter for the action ("" = none)
    std::string keywords;   // other words people type: "alphabetical order"
    std::string keys;       // what the palette shows at the right ("Ctrl+J", "B")
};

const std::vector<Command>& commands();

// How well `query` matches: -1 = not at all; higher = better. Every query
// character must appear in order (case-insensitive) in the title or the
// keywords; matches at the start of a word, and runs of matches, score more;
// a title match beats a keyword match. An empty query matches everything (0).
int match_score(const std::string& query, const Command& c);

// The catalog filtered and ranked by match_score (catalog order breaks ties).
std::vector<const Command*> search(const std::string& query);

}  // namespace klimt::core
