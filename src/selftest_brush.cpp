// klimt_selftest -- PEN BRUSHES (s017): tips (Round, Square, Chalk), angle and
// roundness, the pen's tilt; m3: Smudge.
#include "core/Document.hpp"
#include "core/Paint.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace Pa = klimt::core::paint;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
bool near(double a, double b, double eps = 0.01) { return std::abs(a - b) <= eps; }
std::string num(double v) { return std::to_string(v); }
Pa::Brush hard(double size) { Pa::Brush b; b.size = size; b.hardness = 1; return b; }
// How far a painted run reaches from y0 along a column / x0 along a row (coverage > 0.5).
int extent_y(const P::TileMap& cov, int x, int) { int n = 0; for (int y = 0; y < cov.height(); ++y) n += cov.get(x, y).c[0] > 0.5f; return n; }
int extent_x(const P::TileMap& cov, int, int y) { int n = 0; for (int x = 0; x < cov.width(); ++x) n += cov.get(x, y).c[0] > 0.5f; return n; }
}  // namespace

void selftest_brush(const Check& check) {
    std::cout << "\n-- pen brushes: the tip (s017) --\n";
    const Pa::Dab at{0, 0, 1.f};
    {
        Pa::Brush b = hard(40);
        b.hardness = 0.5;
        bool same = true;
        for (double d = 0; d < 22; d += 1.3) same = same && near(Pa::tip_alpha(d * 0.6, d * 0.8, 20, b, at, 0, 0), Pa::dab_alpha(d, 20, 0.5), 1e-6);
        check("tip: Round, upright, is the s008 dab", same, "");
    }
    {
        Pa::Brush b = hard(40);
        b.roundness = 0.25;
        check("tip: flat (roundness 1/4) is wide along its angle", Pa::tip_alpha(16, 0, 20, b, at, 0, 0) > 0.9f, "");
        check("tip: ... and thin across it", Pa::tip_alpha(0, 8, 20, b, at, 0, 0) == 0.f && Pa::tip_alpha(0, 4, 20, b, at, 0, 0) > 0.9f, "");
        b.angle = 90;
        check("tip: angle 90 stands it up", Pa::tip_alpha(0, 16, 20, b, at, 0, 0) > 0.9f && Pa::tip_alpha(16, 0, 20, b, at, 0, 0) == 0.f, "");
        b.angle = 45;   // counter-clockwise: up and to the right (image y down)
        check("tip: angle 45 leans up-right", Pa::tip_alpha(11, -11, 20, b, at, 0, 0) > 0.9f && Pa::tip_alpha(11, 11, 20, b, at, 0, 0) == 0.f, "");
    }
    {
        Pa::Brush b = hard(40);
        b.tip = Pa::Tip::Square;
        check("tip: Square fills its corners", Pa::tip_alpha(18, 18, 20, b, at, 0, 0) > 0.9f, "");
        b.tip = Pa::Tip::Round;
        check("tip: (Round doesn't)", Pa::tip_alpha(18, 18, 20, b, at, 0, 0) == 0.f, "");
        check("tip: Square reaches its corners", Pa::tip_reach(20, [] { Pa::Brush s; s.tip = Pa::Tip::Square; return s; }()) > 28, "");
    }
    {
        Pa::Brush b = hard(60);
        b.tip = Pa::Tip::Chalk;
        double sum = 0, lo = 1, hi = 0;
        int n = 0;
        for (int y = -20; y <= 20; ++y)
            for (int x = -20; x <= 20; ++x) {
                const float a = Pa::tip_alpha(x, y, 30, b, at, x + 100, y + 100);
                sum += a; lo = std::min(lo, static_cast<double>(a)); hi = std::max(hi, static_cast<double>(a)); ++n;
            }
        check("tip: Chalk is broken by grain", lo < 0.1 && hi > 0.9 && sum / n > 0.3 && sum / n < 0.8, num(sum / n));
        check("tip: Chalk's grain is the paper's (the same pixel, the same grain)",
              Pa::tip_alpha(3, 4, 30, b, at, 103, 104) == Pa::tip_alpha(-7, 1, 30, b, Pa::Dab{10, 3, 1.f}, 103, 104), "");
    }
    {
        Pa::Brush b = hard(40);
        b.tilt = true;
        const Pa::Dab lean_right{0, 0, 1.f, 0.9f, 0.f}, lean_up{0, 0, 1.f, 0.f, -0.9f}, upright{0, 0, 1.f, 0.f, 0.f};
        check("tilt: leaning right, the tip lies along x", Pa::tip_alpha(16, 0, 20, b, lean_right, 0, 0) > 0.9f &&
                                                           Pa::tip_alpha(0, 10, 20, b, lean_right, 0, 0) == 0.f, "");
        check("tilt: leaning up, along y", Pa::tip_alpha(0, -16, 20, b, lean_up, 0, 0) > 0.9f && Pa::tip_alpha(10, 0, 20, b, lean_up, 0, 0) == 0.f, "");
        check("tilt: upright, round", Pa::tip_alpha(0, 16, 20, b, upright, 0, 0) > 0.9f && Pa::tip_alpha(16, 0, 20, b, upright, 0, 0) > 0.9f, "");
        b.tilt = false;
        check("tilt off: the lean is ignored", Pa::tip_alpha(0, 16, 20, b, lean_right, 0, 0) > 0.9f, "");
    }
    {
        // Strokes: a flat brush at 0 degrees -- thin drawn sideways, wide drawn down.
        Pa::Brush b = hard(40);
        b.roundness = 0.2;
        core::TileSet t;
        t.cols = 3; t.rows = 2; t.on.assign(6, 0);
        double carry = 1e9;
        auto cov = Pa::add_pen_dabs(Pa::new_coverage(600, 400), b, Pa::dabs_along(Pa::Dab{100, 100}, Pa::Dab{300, 100}, b, carry), t);
        const int thin = extent_y(cov, 200, 80);
        carry = 1e9;
        auto cov2 = Pa::add_pen_dabs(Pa::new_coverage(600, 400), b, Pa::dabs_along(Pa::Dab{450, 100}, Pa::Dab{450, 300}, b, carry), t);
        const int wide = extent_x(cov2, 425, 200);
        check("stroke: a flat tip draws thin sideways, wide downwards", thin <= 10 && wide >= 36, num(thin) + " / " + num(wide));
        double c1 = 1e9, c2 = 1e9;
        Pa::Brush r = hard(40);
        check("stroke: a flat tip spaces its dabs closer", Pa::dabs_along(Pa::Dab{0, 0}, Pa::Dab{200, 0}, b, c1).size() >
                                                            Pa::dabs_along(Pa::Dab{0, 0}, Pa::Dab{200, 0}, r, c2).size(), "");
    }
    {
        // A pen stroke through the verb: tilt carried along the path.
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        Pa::Brush b = hard(40);
        b.tilt = true;
        b.colour = P::rgba(0, 0, 1, 1);
        check("stroke_pen with tilt: a step", Pa::stroke_pen(j, d, "L5", {{100, 300, 1.f, 0.9f, 0.f}, {300, 300, 1.f, 0.9f, 0.f}}, b), "");
        const auto& px = *d.get("L5")->pixels;
        check("stroke_pen with tilt: the lean made it thin", px.get(200, 300).c[3] > 0.9f && px.get(200, 310).c[3] == 0.f, "");
    }

    std::cout << "\n-- pen brushes: Smudge (s017 m3) --\n";
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);   // L4: a red square (100, 100) 200 x 150 on transparent
        Pa::Brush b = hard(40);
        b.strength = 0.8;
        const auto before = d.get("L4")->pixels;
        check("smudge: a step", Pa::stroke_smudge(j, d, "L4", {{260, 175, 1.f}, {380, 175, 1.f}}, b), "");
        check("smudge: its label", j.undo_label() == "Smudge", j.undo_label());
        const auto& px = *d.get("L4")->pixels;
        const float near_a = px.get(315, 175).c[3], far_a = px.get(370, 175).c[3];
        check("smudge: red dragged out past the edge", near_a > 0.3f && near(px.get(315, 175).c[0], 1, 0.02) && px.get(315, 175).c[1] < 0.02f,
              num(near_a));
        check("smudge: ... fading along the stroke", far_a < near_a, num(far_a) + " < " + num(near_a));
        check("smudge: off the stroke, nothing", px.get(315, 230) == before->get(315, 230), "");
        j.undo();
        check("smudge: undo", d.get("L4")->pixels == before, "");
        b.strength = 0;
        check("smudge: strength 0 changes nothing (no step)", !Pa::stroke_smudge(j, d, "L4", {{260, 175, 1.f}, {380, 175, 1.f}}, b), "");
        // Into the square: transparency is smudged into the red too.
        b.strength = 0.8;
        Pa::stroke_smudge(j, d, "L4", {{60, 175, 1.f}, {160, 175, 1.f}}, b);
        check("smudge: dragging nothing into colour thins it", d.get("L4")->pixels->get(115, 175).c[3] < 0.9f, num(d.get("L4")->pixels->get(115, 175).c[3]));
        j.undo();
        // Through a selection.
        L::load_selection(d, {300, 100, 30, 150});
        Pa::stroke_smudge(j, d, "L4", {{260, 175, 1.f}, {380, 175, 1.f}}, b);
        check("smudge: only inside the selection", d.get("L4")->pixels->get(315, 175).c[3] > 0.3f && d.get("L4")->pixels->get(345, 175).c[3] == 0.f, "");
    }
}
