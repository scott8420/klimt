// core/Prefs -- the app's settings pump (s014 m4). See core/Prefs.hpp.
#include "core/Prefs.hpp"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace klimt::core {

namespace {
nlohmann::json read(const std::string& file) {
    std::ifstream f(file);
    if (!f) return nlohmann::json::object();
    try {
        nlohmann::json j;
        f >> j;
        return j.is_object() ? j : nlohmann::json::object();
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}
}  // namespace

Prefs load_prefs(const std::string& file) {
    Prefs p;
    const auto j = read(file);
    if (j.contains("dock_split") && j["dock_split"].is_number_integer())
        p.dock_split = std::clamp(j["dock_split"].get<int>(), 120, 4000);
    if (j.contains("rulers") && j["rulers"].is_boolean()) p.rulers = j["rulers"].get<bool>();   // s031
    if (j.contains("ruler_unit") && j["ruler_unit"].is_string()) p.ruler_unit = j["ruler_unit"].get<std::string>();
    for (const auto& [k, f] : {std::pair<const char*, bool Prefs::*>{"show_guides", &Prefs::show_guides},
                               {"lock_guides", &Prefs::lock_guides}, {"snap", &Prefs::snap}})   // s032
        if (j.contains(k) && j[k].is_boolean()) p.*f = j[k].get<bool>();
    return p;
}

bool save_prefs(const std::string& file, const Prefs& p) {
    try {
        fs::create_directories(fs::path(file).parent_path());
    } catch (const std::exception&) {
        return false;
    }
    auto j = read(file);   // keep what we don't know
    j["dock_split"] = p.dock_split;
    j["rulers"] = p.rulers;            // s031
    j["ruler_unit"] = p.ruler_unit;
    j["show_guides"] = p.show_guides;   // s032
    j["lock_guides"] = p.lock_guides;
    j["snap"] = p.snap;
    std::ofstream f(file);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}

}  // namespace klimt::core
