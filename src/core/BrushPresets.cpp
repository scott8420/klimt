// core/BrushPresets -- saved brushes (s021). See core/BrushPresets.hpp.
#include "core/BrushPresets.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace klimt::core {

bool BrushPreset::same_brush(const BrushPreset& o) const {
    BrushPreset a = *this;
    a.name = o.name;
    return a == o;
}

const std::vector<BrushPreset>& default_presets() {
    static const std::vector<BrushPreset> v = [] {
        std::vector<BrushPreset> d;
        BrushPreset p;
        p = {}; p.name = "Hard Round";  p.size = 20;  p.hardness = 100; d.push_back(p);
        p = {}; p.name = "Soft Round";  p.size = 60;  p.hardness = 0;   d.push_back(p);
        p = {}; p.name = "Airbrush";    p.size = 120; p.hardness = 0;   p.flow = 10; p.pressure_size = false; p.pressure_opacity = true; d.push_back(p);
        p = {}; p.name = "Pencil";      p.size = 4;   p.hardness = 100; p.tip = 1; p.smoothing = 20; d.push_back(p);
        p = {}; p.name = "Inker";       p.size = 8;   p.hardness = 95;  p.smoothing = 40; d.push_back(p);
        p = {}; p.name = "Chalk";       p.size = 50;  p.hardness = 70;  p.tip = 2; p.pressure_opacity = true; d.push_back(p);
        p = {}; p.name = "Calligraphy"; p.size = 30;  p.hardness = 90;  p.angle = 45; p.roundness = 20; p.smoothing = 30; d.push_back(p);
        p = {}; p.name = "Sketch";      p.size = 12;  p.hardness = 60;  p.opacity = 70; p.pressure_opacity = true; p.tilt = true; d.push_back(p);
        return d;
    }();
    return v;
}

BrushPreset clamped(BrushPreset p) {
    p.size = std::clamp(p.size, 1, 2500);
    p.hardness = std::clamp(p.hardness, 0, 100);
    p.opacity = std::clamp(p.opacity, 1, 100);
    p.flow = std::clamp(p.flow, 1, 100);
    p.smoothing = std::clamp(p.smoothing, 0, 100);
    p.tip = std::clamp(p.tip, 0, 2);
    p.angle = ((p.angle + 180) % 360 + 360) % 360 - 180;
    p.roundness = std::clamp(p.roundness, 5, 100);
    p.strength = std::clamp(p.strength, 1, 100);
    return p;
}

namespace {
std::string trimmed(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
}  // namespace

std::string unique_preset_name(const std::vector<BrushPreset>& v, const std::string& want_in, int skip) {
    std::string want = trimmed(want_in);
    if (want.empty()) want = "Brush";
    if (want.size() > 60) want.resize(60);
    const auto taken = [&](const std::string& n) {
        for (std::size_t i = 0; i < v.size(); ++i)
            if (static_cast<int>(i) != skip && v[i].name == n) return true;
        return false;
    };
    if (!taken(want)) return want;
    for (int k = 2;; ++k) {
        const std::string n = want + " " + std::to_string(k);
        if (!taken(n)) return n;
    }
}

std::vector<BrushPreset> load_presets(const std::string& file) {
    std::ifstream f(file);
    if (!f) return default_presets();
    nlohmann::json j;
    try { f >> j; } catch (const std::exception&) { return default_presets(); }
    if (!j.is_object() || !j.contains("presets") || !j["presets"].is_array()) return default_presets();
    std::vector<BrushPreset> out;
    for (const auto& e : j["presets"]) {
        if (!e.is_object() || !e.contains("name") || !e["name"].is_string()) continue;
        BrushPreset p;
        const auto num = [&](const char* k, int& to) { if (e.contains(k) && e[k].is_number()) to = static_cast<int>(e[k].get<double>()); };
        const auto flag = [&](const char* k, bool& to) { if (e.contains(k) && e[k].is_boolean()) to = e[k].get<bool>(); };
        num("size", p.size); num("hardness", p.hardness); num("opacity", p.opacity); num("flow", p.flow);
        num("smoothing", p.smoothing); num("tip", p.tip); num("angle", p.angle); num("roundness", p.roundness);
        num("strength", p.strength);
        flag("pressure_size", p.pressure_size); flag("pressure_opacity", p.pressure_opacity); flag("tilt", p.tilt);
        p = clamped(p);
        p.name = unique_preset_name(out, e["name"].get<std::string>());
        out.push_back(p);
    }
    return out;   // an empty list stays empty: the user deleted them all
}

bool save_presets(const std::string& file, const std::vector<BrushPreset>& v) {
    try { fs::create_directories(fs::path(file).parent_path()); } catch (const std::exception&) { return false; }
    nlohmann::json j;
    j["version"] = 1;
    j["presets"] = nlohmann::json::array();
    for (const auto& p : v)
        j["presets"].push_back({{"name", p.name}, {"size", p.size}, {"hardness", p.hardness}, {"opacity", p.opacity},
                                {"flow", p.flow}, {"smoothing", p.smoothing}, {"tip", p.tip}, {"angle", p.angle},
                                {"roundness", p.roundness}, {"strength", p.strength}, {"pressure_size", p.pressure_size},
                                {"pressure_opacity", p.pressure_opacity}, {"tilt", p.tilt}});
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
