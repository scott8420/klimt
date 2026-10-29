// core/BrushPresets -- saved brushes (s021). See core/BrushPresets.hpp.
#include "core/BrushPresets.hpp"

#include "PresetJson.hpp"

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
        // s028: two with dynamics.
        p = {}; p.name = "Foliage";     p.size = 40;  p.hardness = 90;  p.roundness = 45; p.pressure_size = false;
                p.size_jitter = 70; p.min_size = 20; p.angle_jitter = 100; p.roundness_jitter = 40; p.scatter = 150; p.both_axes = true;
                p.count = 2; p.count_jitter = 50; p.fg_bg = 100; p.hue = 8; p.bri = 15; d.push_back(p);
        p = {}; p.name = "Spatter";     p.size = 14;  p.hardness = 100; p.pressure_size = false; p.pressure_opacity = true;
                p.size_jitter = 85; p.scatter = 400; p.both_axes = true; p.count = 4; p.count_jitter = 75; d.push_back(p);
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
    // s028: dynamics
    for (int* v : {&p.size_jitter, &p.min_size, &p.angle_jitter, &p.roundness_jitter, &p.min_roundness, &p.count_jitter,
                   &p.fg_bg, &p.hue, &p.sat, &p.bri})
        *v = std::clamp(*v, 0, 100);
    p.scatter = std::clamp(p.scatter, 0, 1000);
    p.tex_scale = std::clamp(p.tex_scale, 10, 1000);   // s033
    p.tex_depth = std::clamp(p.tex_depth, 0, 100);
    if (p.tex_pattern.empty() || p.tex_pattern.size() > 80) p.tex_pattern = "paper";
    p.count = std::clamp(p.count, 1, 16);
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

BrushPreset preset_from_json(const nlohmann::json& e) {
    BrushPreset p;
    const auto num = [&](const char* k, int& to) { if (e.contains(k) && e[k].is_number()) to = static_cast<int>(e[k].get<double>()); };
    const auto flag = [&](const char* k, bool& to) { if (e.contains(k) && e[k].is_boolean()) to = e[k].get<bool>(); };
    num("size", p.size); num("hardness", p.hardness); num("opacity", p.opacity); num("flow", p.flow);
    num("smoothing", p.smoothing); num("tip", p.tip); num("angle", p.angle); num("roundness", p.roundness);
    num("strength", p.strength);
    flag("pressure_size", p.pressure_size); flag("pressure_opacity", p.pressure_opacity); flag("tilt", p.tilt);
    num("size_jitter", p.size_jitter); num("min_size", p.min_size); num("angle_jitter", p.angle_jitter);   // s028
    num("roundness_jitter", p.roundness_jitter); num("min_roundness", p.min_roundness);
    num("scatter", p.scatter); num("count", p.count); num("count_jitter", p.count_jitter); flag("both_axes", p.both_axes);
    num("fg_bg", p.fg_bg); num("hue", p.hue); num("sat", p.sat); num("bri", p.bri); flag("per_tip", p.per_tip);
    flag("tex_on", p.tex_on); num("tex_scale", p.tex_scale); num("tex_depth", p.tex_depth);   // s033
    flag("tex_subtract", p.tex_subtract); flag("tex_invert", p.tex_invert);
    if (e.contains("tex_pattern") && e["tex_pattern"].is_string()) p.tex_pattern = e["tex_pattern"].get<std::string>();
    return clamped(p);
}

nlohmann::json preset_to_json(const BrushPreset& p) {
    nlohmann::json e = {{"name", p.name}, {"size", p.size}, {"hardness", p.hardness}, {"opacity", p.opacity},
                        {"flow", p.flow}, {"smoothing", p.smoothing}, {"tip", p.tip}, {"angle", p.angle},
                        {"roundness", p.roundness}, {"strength", p.strength}, {"pressure_size", p.pressure_size},
                        {"pressure_opacity", p.pressure_opacity}, {"tilt", p.tilt}};
    // s028: dynamics, written only when set (an older Klimt reads the rest as before).
    const BrushPreset plain;
    if (p.size_jitter != plain.size_jitter || p.min_size != plain.min_size || p.angle_jitter != plain.angle_jitter ||
        p.roundness_jitter != plain.roundness_jitter || p.min_roundness != plain.min_roundness || p.scatter != plain.scatter ||
        p.count != plain.count || p.count_jitter != plain.count_jitter || p.both_axes != plain.both_axes || p.fg_bg != plain.fg_bg ||
        p.hue != plain.hue || p.sat != plain.sat || p.bri != plain.bri || p.per_tip != plain.per_tip) {
        e["size_jitter"] = p.size_jitter; e["min_size"] = p.min_size; e["angle_jitter"] = p.angle_jitter;
        e["roundness_jitter"] = p.roundness_jitter; e["min_roundness"] = p.min_roundness;
        e["scatter"] = p.scatter; e["count"] = p.count; e["count_jitter"] = p.count_jitter; e["both_axes"] = p.both_axes;
        e["fg_bg"] = p.fg_bg; e["hue"] = p.hue; e["sat"] = p.sat; e["bri"] = p.bri; e["per_tip"] = p.per_tip;
    }
    if (p.tex_on) {   // s033: the texture, only when on
        e["tex_on"] = true; e["tex_pattern"] = p.tex_pattern; e["tex_scale"] = p.tex_scale; e["tex_depth"] = p.tex_depth;
        e["tex_subtract"] = p.tex_subtract; e["tex_invert"] = p.tex_invert;
    }
    return e;
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
        BrushPreset p = preset_from_json(e);
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
    for (const auto& p : v) {
        j["presets"].push_back(preset_to_json(p));
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
