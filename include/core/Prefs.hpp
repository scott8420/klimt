#pragma once
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// core/Prefs -- the app's own settings, remembered between runs (s014 m4), and
// their JSON pump. The Recents pattern: GTK-free, encode and decode in ONE
// file; the UI resolves the path (XDG config dir) and hands it in.
//
// On disk: { "dock_split": 360 }. load is first-run and garbage tolerant (the
// defaults), clamps what it reads, and keeps keys it doesn't know when saving
// over them (a newer Klimt's settings survive an older one).
// ─────────────────────────────────────────────────────────────────────────────
namespace klimt::core {

struct Prefs {
    int dock_split = 360;   // the dock's divider: Properties' height, px (120 .. 4000)
    bool operator==(const Prefs&) const = default;
};

Prefs load_prefs(const std::string& file);
bool  save_prefs(const std::string& file, const Prefs& p);

}  // namespace klimt::core
