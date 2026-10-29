// core/ToolDefaults -- each tool's saved defaults (s029). See core/ToolDefaults.hpp.
#include "core/ToolDefaults.hpp"
#include "PresetJson.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace klimt::core {

ToolDefaults load_tool_defaults(const std::string& file) {
    ToolDefaults d;
    std::ifstream f(file);
    if (!f) return d;
    nlohmann::json j;
    try { f >> j; } catch (const std::exception&) { return {}; }
    if (!j.is_object()) return {};
    if (j.contains("brushes") && j["brushes"].is_object())
        for (const auto& [tool, e] : j["brushes"].items()) {
            if (!e.is_object()) continue;
            BrushPreset p = preset_from_json(e);
            p.name = tool;
            d.brushes[tool] = p;
        }
    if (j.contains("tools") && j["tools"].is_object())
        for (const auto& [group, e] : j["tools"].items()) {
            if (!e.is_object()) continue;
            auto& m = d.others[group];
            for (const auto& [k, v] : e.items()) {
                if (v.is_boolean()) m[k] = v.get<bool>() ? 1 : 0;
                else if (v.is_number()) m[k] = static_cast<int>(v.get<double>());
            }
            if (m.empty()) d.others.erase(group);
        }
    return d;
}

bool save_tool_defaults(const std::string& file, const ToolDefaults& d) {
    try { fs::create_directories(fs::path(file).parent_path()); } catch (const std::exception&) { return false; }
    nlohmann::json j;
    j["version"] = 1;
    j["brushes"] = nlohmann::json::object();
    for (const auto& [tool, p] : d.brushes) {
        auto e = preset_to_json(p);
        e.erase("name");
        j["brushes"][tool] = e;
    }
    j["tools"] = nlohmann::json::object();
    for (const auto& [group, m] : d.others) {
        nlohmann::json e = nlohmann::json::object();
        for (const auto& [k, v] : m) e[k] = v;
        j["tools"][group] = e;
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
