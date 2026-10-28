// klimt_selftest -- SELECT > COLOR RANGE (s024).
#include "core/ColourRange.hpp"
#include "core/Document.hpp"

#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace S = klimt::core::select;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

void selftest_range(const Check& check) {
    std::cout << "\n-- select > color range (s024) --\n";
    S::ColourRange r;
    r.samples = {{1, 0, 0}};
    r.fuzziness = 40;
    check("weight: the sample itself -- all", S::colour_weight(r, 1, 0, 0) == 1.f, "");
    check("weight: within fuzz / 2 -- all", S::colour_weight(r, 1, 15.f / 255, 0) == 1.f, "");
    check("weight: between -- part", [&] { const float w = S::colour_weight(r, 1, 30.f / 255, 0); return w > 0.f && w < 1.f; }(), "");
    check("weight: past fuzz -- none", S::colour_weight(r, 1, 45.f / 255, 0) == 0.f, "");
    r.samples.push_back({0, 0, 1});
    check("weight: two samples -- the nearest", S::colour_weight(r, 0, 0, 1) == 1.f, "");
    r.invert = true;
    check("weight: inverted", S::colour_weight(r, 0, 0, 1) == 0.f && S::colour_weight(r, 0, 1, 0) == 1.f, "");

    core::Document d;
    core::DocJournal j{d};
    d.set_door([&] { return j.open(); });
    L::load_fixture(d);   // the red square (100, 100) 200 x 150 on white
    S::ColourRange red;
    red.samples = {{1, 0, 0}};
    check("colour_range: ONE step, \"Color Range\"", S::colour_range(j, d, red) && j.size() == 1 && j.undo_label() == "Color Range", j.undo_label());
    const auto* sel = d.selection();
    check("colour_range: the square selected, the white not", sel && S::coverage(*sel, 200, 175) == 1.f && S::coverage(*sel, 50, 50) == 0.f &&
              S::coverage(*sel, 99, 175) == 0.f, "");
    // Add white: everything.
    S::ColourRange white;
    white.samples = {{1, 1, 1}};
    check("colour_range Add: white too -- all of it", S::colour_range(j, d, white, S::Op::Add) && S::coverage(*d.selection(), 50, 50) == 1.f &&
              S::coverage(*d.selection(), 200, 175) == 1.f, "");
    j.undo();
    // Subtract red: nothing left.
    S::colour_range(j, d, red, S::Op::Subtract);
    check("colour_range Subtract: the square out again -- nothing", d.selection() == nullptr, "");
    check("colour_range: the tree holds, no strays", d.check().empty() && d.strays() == 0, "");
}
