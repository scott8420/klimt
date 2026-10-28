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
    std::ofstream f(file);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}

}  // namespace klimt::core
