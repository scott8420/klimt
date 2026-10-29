// klimt_selftest -- TOOL DEFAULTS (s029): the file round trip, nothing saved
// when there's no file or a broken one, clamping, bools, odd values dropped.
#include "core/ToolDefaults.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

namespace core = klimt::core;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

void selftest_tooldefaults(const Check& check) {
    std::cout << "\n-- tool defaults (s029) --\n";
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("klimt_tools_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const std::string file = (dir / "tools.json").string();
    check("defaults: none saved when there's no file", core::load_tool_defaults(file) == core::ToolDefaults{}, "");
    core::ToolDefaults d;
    core::BrushPreset b;
    b.size = 64; b.hardness = 20; b.opacity = 70; b.tip = 2; b.scatter = 300; b.count = 3; b.hue = 15; b.per_tip = false;
    b.name = "brush";
    d.brushes["brush"] = b;
    core::BrushPreset e;
    e.size = 120; e.name = "eraser";
    d.brushes["eraser"] = e;
    d.others["gradient"] = {{"opacity", 60}, {"reverse", 1}, {"to_transparent", 0}};
    d.others["wand"] = {{"tolerance", 12}, {"contiguous", 0}, {"all_layers", 1}};
    check("defaults: saved", core::save_tool_defaults(file, d) && fs::exists(file) && !fs::exists(file + ".tmp"), "");
    check("defaults: read back the same", core::load_tool_defaults(file) == d, "");
    { std::ofstream(file) << "{ broken"; }
    check("defaults: a broken file -- none saved", core::load_tool_defaults(file) == core::ToolDefaults{}, "");
    { std::ofstream(file) << R"({"brushes":{"brush":{"size":99999,"scatter":-4,"count":40},"x":7},
                                 "tools":{"wand":{"tolerance":40,"contiguous":false,"all_layers":true,"note":"hi"},"crop":{"what":"no"},"y":3}})"; }
    const auto g = core::load_tool_defaults(file);
    check("defaults: a brush clamped on reading", g.brushes.count("brush") && g.brushes.at("brush").size == 2500 &&
                                                   g.brushes.at("brush").scatter == 0 && g.brushes.at("brush").count == 16, "");
    check("defaults: rows that aren't objects dropped", !g.brushes.count("x") && !g.others.count("y"), "");
    check("defaults: bools as 0 / 1, strings dropped", g.others.count("wand") && g.others.at("wand").at("contiguous") == 0 &&
                                                       g.others.at("wand").at("all_layers") == 1 && !g.others.at("wand").count("note"), "");
    check("defaults: a group with nothing readable is gone", !g.others.count("crop"), "");
    fs::remove_all(dir);
}
