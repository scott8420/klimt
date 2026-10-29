// klimt_selftest -- BRUSH PRESETS (s021): the defaults, the file round trip,
// garbage tolerated, clamping, unique names.
#include "core/BrushPresets.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

namespace core = klimt::core;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

void selftest_presets(const Check& check) {
    std::cout << "\n-- brush presets (s021) --\n";
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("klimt_presets_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const std::string file = (dir / "brushes.json").string();
    const auto& d = core::default_presets();
    check("presets: defaults when there is no file", core::load_presets(file) == d && d.size() >= 6, std::to_string(d.size()));
    bool unique = true;
    for (std::size_t i = 0; i < d.size(); ++i)
        for (std::size_t k = i + 1; k < d.size(); ++k) unique = unique && d[i].name != d[k].name;
    check("presets: the defaults' names are unique and clamped", unique && core::clamped(d[3]) == d[3], "");

    auto v = d;
    core::BrushPreset p;
    p.name = core::unique_preset_name(v, "Hard Round");
    check("unique name: \"Hard Round 2\"", p.name == "Hard Round 2", p.name);
    p.size = 77; p.tip = 2; p.angle = -30; p.roundness = 40; p.tilt = true; p.pressure_size = false; p.strength = 80;
    v.push_back(p);
    check("save: written", core::save_presets(file, v) && fs::exists(file) && !fs::exists(file + ".tmp"), "");
    check("load: the same list back", core::load_presets(file) == v, "");
    check("unique name: a rename keeps its own name", core::unique_preset_name(v, "Hard Round 2", static_cast<int>(v.size() - 1)) == "Hard Round 2", "");
    check("unique name: blank -> \"Brush\"", core::unique_preset_name(v, "   ") == "Brush", "");
    check("same_brush: all but the name", [&] { auto q = p; q.name = "x"; return q.same_brush(p) && !(q == p); }(), "");

    // An empty list stays empty (all deleted), not the defaults again.
    core::save_presets(file, {});
    check("load: an emptied list stays empty", core::load_presets(file).empty(), "");
    // Garbage: the defaults. Out-of-range values: clamped. Duplicate names: made unique.
    { std::ofstream(file) << "{ not json"; }
    check("load: garbage -> the defaults", core::load_presets(file) == d, "");
    { std::ofstream(file) << R"({"presets":[{"name":"A","size":99999,"hardness":-5,"angle":270,"roundness":0},{"name":"A"},{"size":3},"x"]})"; }
    const auto g = core::load_presets(file);
    check("load: bad rows dropped, duplicates renamed", g.size() == 2 && g[0].name == "A" && g[1].name == "A 2", std::to_string(g.size()));
    check("load: clamped", g.size() == 2 && g[0].size == 2500 && g[0].hardness == 0 && g[0].angle == -90 && g[0].roundness == 5,
          g.empty() ? "" : std::to_string(g[0].angle));
    // s028: dynamics -- round trip, clamped, and a plain preset writes none.
    {
        auto w = d;
        core::BrushPreset q;
        q.name = "Jittery";
        q.size_jitter = 60; q.min_size = 10; q.angle_jitter = 100; q.roundness_jitter = 30; q.min_roundness = 40;
        q.scatter = 350; q.count = 5; q.count_jitter = 20; q.both_axes = true;
        q.fg_bg = 50; q.hue = 10; q.sat = 20; q.bri = 30; q.per_tip = false;
        w.push_back(q);
        core::save_presets(file, w);
        check("dynamics: saved and read back", core::load_presets(file) == w, "");
        std::ifstream in(file);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        core::save_presets(file, {d[0]});
        std::ifstream in2(file);
        const std::string plain((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
        check("dynamics: a plain preset writes no dynamics keys", plain.find("scatter") == std::string::npos && text.find("scatter") != std::string::npos, "");
        { std::ofstream(file) << R"({"presets":[{"name":"X","scatter":5000,"count":40,"hue":-3,"size_jitter":250}]})"; }
        const auto c = core::load_presets(file);
        check("dynamics: clamped on reading", c.size() == 1 && c[0].scatter == 1000 && c[0].count == 16 && c[0].hue == 0 && c[0].size_jitter == 100, "");
        bool some = false;
        for (const auto& e : d) some = some || e.scatter > 0;
        check("dynamics: a default with scatter (Foliage / Spatter)", some, "");
    }
    // s033: the texture -- round trip; a preset without it writes none.
    {
        core::BrushPreset q;
        q.name = "Grainy";
        q.tex_on = true; q.tex_pattern = "u:My Paper"; q.tex_scale = 250; q.tex_depth = 80; q.tex_subtract = true; q.tex_invert = true;
        core::save_presets(file, {q});
        const auto back = core::load_presets(file);
        check("texture: saved and read back", back.size() == 1 && back[0] == q, "");
        { std::ofstream(file) << R"({"presets":[{"name":"T","tex_on":true,"tex_scale":5000,"tex_depth":-3,"tex_pattern":""}]})"; }
        const auto t = core::load_presets(file);
        check("texture: clamped on reading", t.size() == 1 && t[0].tex_scale == 1000 && t[0].tex_depth == 0 && t[0].tex_pattern == "paper", "");
    }
    fs::remove_all(dir);
}
