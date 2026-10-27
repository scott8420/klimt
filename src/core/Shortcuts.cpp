#include "core/Shortcuts.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include "core/Tools.hpp"

namespace klimt::core {

// ─────────────────────────────────────────────────────────────────────────────
// Accel formatting
// ─────────────────────────────────────────────────────────────────────────────
namespace {

std::string lower(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) o += char(std::tolower((unsigned char)c));
    return o;
}

// GTK key name -> display glyph. Single alpha keys uppercase; named keys map to a
// glyph where one reads better, otherwise the name is prettified.
std::string map_key(const std::string& key) {
    if (key.empty()) return "";
    static const std::map<std::string, std::string> named = {
        {"comma", ","},     {"period", "."},     {"question", "?"},
        {"slash", "/"},     {"minus", "\u2212"}, {"plus", "+"},
        {"equal", "="},     {"space", "Space"},
        {"Return", "Enter"},{"Escape", "Esc"},
        {"Left", "\u2190"}, {"Right", "\u2192"}, {"Up", "\u2191"}, {"Down", "\u2193"},
        {"Delete", "Del"},  {"BackSpace", "Backspace"},
        {"Page_Up", "Page Up"}, {"Page_Down", "Page Down"},
        {"KP_Add", "Keypad +"}, {"KP_Subtract", "Keypad \u2212"}, {"KP_0", "Keypad 0"},
    };
    auto it = named.find(key);
    if (it != named.end()) return it->second;
    if (key.size() == 1) {
        std::string s = key;
        s[0] = char(std::toupper((unsigned char)s[0]));
        return s;
    }
    std::string s = key;
    for (char& c : s)
        if (c == '_') c = ' ';
    return s;
}

// Split an accel into its modifier flags + bare key name. Shared by
// format_accel and any future consumer so they can't disagree.
struct Parsed { bool ctrl = false, alt = false, shift = false, super = false; std::string key; };

Parsed parse_accel(const std::string& accel) {
    Parsed p;
    std::size_t i = 0;
    while (i < accel.size()) {
        if (accel[i] == '<') {
            std::size_t j = accel.find('>', i);
            if (j == std::string::npos) break;
            const std::string ml = lower(accel.substr(i + 1, j - i - 1));
            if (ml == "ctrl" || ml == "control" || ml == "primary") p.ctrl = true;
            else if (ml == "alt" || ml == "mod1")                   p.alt = true;
            else if (ml == "shift")                                 p.shift = true;
            else if (ml == "super" || ml == "meta" || ml == "mod4") p.super = true;
            i = j + 1;
        } else {
            p.key = accel.substr(i);
            break;
        }
    }
    return p;
}

}  // namespace

std::string format_accel(const std::string& accel) {
    const Parsed p = parse_accel(accel);
    const std::string k = map_key(p.key);
    std::string out;
    auto add = [&](const std::string& s) {
        if (!out.empty()) out += "+";
        out += s;
    };
    if (p.ctrl)  add("Ctrl");
    if (p.alt)   add("Alt");
    if (p.shift) add("Shift");
    if (p.super) add("Super");
    if (!k.empty()) add(k);
    return out;
}

std::string ShortcutSpec::display_keys() const {
    if (!keys.empty()) return keys;
    std::string out;
    for (const auto& a : accels) {
        if (!out.empty()) out += "  /  ";
        out += format_accel(a);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// The registry  (authored section [A-Z] -> row)
// ─────────────────────────────────────────────────────────────────────────────
const std::vector<ShortcutSpec>& shortcut_registry() {
    // Fields: {section, action, accels, keys, description}
    // action non-empty => wired via set_accels_for_action. keys empty => derived.
    // Deliberately small -- a seed proves the SHAPE, not a product's key set. It
    // carries one doc-only row so that half of the paradigm is exercised too.
    static const std::vector<ShortcutSpec> kReg = [] {
    std::vector<ShortcutSpec> reg = {
        // ── Diagnostics ───────────────────────────────────────────────────────
        {"Diagnostics", "win.dump-registry", {"<Ctrl>d"}, "",
         "Dump the widget registry to the log"},

        // ── Edit (s046) ───────────────────────────────────────────────────────
        // Doc-only ON PURPOSE: an accelerator would take Ctrl+Z from every text
        // box. The window hears these instead (Shell_bindings.cpp).
        {"Edit", "", {}, "Ctrl+Z", "Undo -- anywhere but a text box (which keeps its own)"},
        {"Edit", "", {}, "Ctrl+Shift+Z  /  Ctrl+Y", "Redo"},

        // ── General ───────────────────────────────────────────────────────────
        {"General", "win.command-palette", {"<Ctrl>k"}, "", "Find a command (the palette)"},
        {"General", "win.shortcuts",     {"<Ctrl>question", "<Ctrl>slash"}, "",
         "Keyboard shortcuts (this window)"},
        {"General", "win.about",         {"F1"}, "", "About Klimt"},
        {"General", "win.quit",          {"<Ctrl>q"}, "", "Quit"},

        // ── Layer (s003) ──────────────────────────────────────────────────────
        {"Layer", "win.layer-new",       {"<Ctrl><Shift>n"}, "", "New layer above the selection"},
        {"Layer", "win.layer-duplicate", {"<Ctrl>j"}, "", "Duplicate the selection"},
        {"Layer", "win.layer-group",     {"<Ctrl>g"}, "", "Group the selection"},
        {"Layer", "win.layer-ungroup",   {"<Ctrl><Shift>g"}, "", "Ungroup"},
        {"Layer", "win.layer-clip",      {"<Ctrl><Alt>g"}, "", "Clip to the layer below / release"},
        {"Layer", "win.layer-raise",     {"<Ctrl>bracketright"}, "", "Bring forward"},
        {"Layer", "win.layer-lower",     {"<Ctrl>bracketleft"}, "", "Send backward"},
        // Doc-only: the panel's list hears these itself, so a text box keeps them.
        {"Layer", "", {}, "Delete  /  Backspace", "Delete the selection (in the Layers list)"},
        {"Layer", "", {}, "F2", "Rename (in the Layers list)"},

        // ── Mouse ─────────────────────────────────────────────────────────────
        // Doc-only: a gesture is not a GAction, but the reader still needs it.
        {"Mouse", "", {}, "Double-click a layer's name", "Rename it (Enter keeps, Escape drops)"},
        {"Mouse", "", {}, "Ctrl / Shift + click a layer", "Select several"},
        {"Mouse", "", {}, "Drag a layer (or the selection)",
         "Move it -- the line shows above / below; a lit group takes it inside"},
    };
    // ── Tools (s005): one row per tool, FROM the tool registry -- the rail,
    // the keys and this reference read one list. Doc-only: the window hears
    // the letter (an accelerator would take it from every text box).
    for (const auto& t : tools()) {
        std::string k(1, static_cast<char>(t.key - 'a' + 'A'));
        reg.push_back({"Tools", "", {}, k, t.name});
    }
    // ── View (s005) -- after Tools: sections are authored A-Z.
    reg.push_back({"View", "", {}, "Tab  (on the canvas)", "Hide / show the panels"});
    return reg;
    }();
    return kReg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Collision detection
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::string> find_accel_collisions() {
    std::map<std::string, std::pair<int, bool>> seen;  // accel -> {count, any_action}
    for (const auto& s : shortcut_registry())
        for (const auto& a : s.accels) {
            auto& e = seen[a];
            e.first += 1;
            if (!s.action.empty()) e.second = true;
        }
    std::vector<std::string> out;
    for (const auto& [accel, e] : seen)
        if (e.first >= 2 && e.second) out.push_back(accel);  // std::map keeps it sorted+unique
    return out;
}

}  // namespace klimt::core
