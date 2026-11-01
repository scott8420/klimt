// core/AdjustPresets (s065) -- see the header.
#include "core/AdjustPresets.hpp"
#include "core/File.hpp"

#include "json.hpp"

#include <filesystem>
#include <fstream>

namespace klimt::core {

namespace fs = std::filesystem;

namespace {
std::string trimmed(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
}  // namespace

std::string unique_adjust_preset_name(const std::vector<AdjustPreset>& v, AdjustKind k, const std::string& want) {
    std::string base = trimmed(want);
    if (base.empty()) base = std::string(adjust::label(k));
    if (base.size() > 64) base.resize(64);
    const auto taken = [&](const std::string& n) {
        for (const auto& p : v) if (p.a.kind == k && p.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!taken(n)) return n;
    }
}

std::vector<AdjustPreset> load_adjust_presets(const std::string& file) {
    std::vector<AdjustPreset> out;
    std::ifstream f(file);
    if (!f) return out;
    nlohmann::json j;
    try { j = nlohmann::json::parse(f); } catch (const std::exception&) { return out; }
    if (!j.is_object() || !j.contains("presets") || !j["presets"].is_array()) return out;
    for (const auto& e : j["presets"]) {
        if (!e.is_object() || !e.contains("name") || !e["name"].is_string() || !e.contains("adjust")) continue;
        const auto a = file::adjust_parse(e["adjust"].dump());
        if (!a) continue;
        out.push_back({unique_adjust_preset_name(out, a->kind, e["name"].get<std::string>()), *a});
    }
    return out;
}

bool save_adjust_presets(const std::string& file, const std::vector<AdjustPreset>& v) {
    try { fs::create_directories(fs::path(file).parent_path()); } catch (const std::exception&) { return false; }
    nlohmann::json j;
    j["version"] = 1;
    j["presets"] = nlohmann::json::array();
    for (const auto& p : v) {
        nlohmann::json a;
        try { a = nlohmann::json::parse(file::adjust_text(p.a)); } catch (const std::exception&) { continue; }
        j["presets"].push_back({{"name", p.name}, {"adjust", std::move(a)}});
    }
    const std::string tmp = file + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << j.dump(2) << "\n";
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);   // whole or not at all
    return !ec;
}

}  // namespace klimt::core
