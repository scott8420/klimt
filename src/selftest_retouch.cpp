// klimt_selftest -- RETOUCHING (s015): the Clone Stamp's source and offset,
// clone strokes (the layer itself, All Layers, opacity, the selection,
// transparency); m3: healing.
#include "core/Document.hpp"
#include "core/Heal.hpp"
#include "core/Paint.hpp"
#include "core/Selection.hpp"

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
struct Fix {
    core::Document d;
    core::DocJournal j{d};
    Fix() {
        d.set_door([this] { return j.open(); });
        L::load_fixture(d);   // white Background L1; L4 a red square (100, 100) 200 x 150; L5 empty
    }
    P::Px px(const char* id, int x, int y) const { const auto l = d.get(id); return l && l->pixels ? l->pixels->get(x, y) : P::transparent(); }
};
Pa::Brush hard(double size) { Pa::Brush b; b.size = size; b.hardness = 1; return b; }
}  // namespace

void selftest_retouch(const Check& check) {
    std::cout << "\n-- retouching: the Clone Stamp (s015) --\n";
    {
        Pa::CloneSource s;
        s.set_point({10, 10});
        s.begin({50, 60});
        check("clone source: the first stroke sets the offset", s.dx == -40 && s.dy == -50, num(s.dx));
        s.begin({100, 100});
        check("clone source: aligned keeps it", s.dx == -40 && s.dy == -50 && s.at({100, 100}).x == 60, "");
        s.set_point({0, 0});
        check("clone source: a new point waits for the next stroke", !s.fixed && s.at({5, 5}).x == 0, "");
        s.begin({20, 30});
        check("clone source: ... which sets it again", s.dx == -20 && s.dy == -30, "");
        s.aligned = false;
        s.begin({200, 100});
        check("clone source: not aligned, each stroke from the point", s.dx == -200 && s.dy == -100, num(s.dx));
    }
    {
        Fix f;
        Pa::Brush b = hard(30);
        std::string why;
        b.source = Pa::clone_source(f.d, "L4", false, why);
        b.dx = -250; b.dy = -125;   // from (150, 175): red
        const auto steps = f.j.size();
        check("clone: a stroke", Pa::stroke(f.j, f.d, "L4", {{400, 300}}, b), "");
        check("clone: ONE step, its label", f.j.size() == steps + 1 && f.j.undo_label() == "Clone Stamp", f.j.undo_label());
        const auto c = f.px("L4", 400, 300);
        check("clone: the source's colour lands", near(c.c[0], 1) && near(c.c[1], 0) && near(c.c[3], 1), num(c.c[3]));
        check("clone: outside the dab nothing", f.px("L4", 430, 300).c[3] == 0.f, "");
        f.j.undo();
        check("clone: undo", f.px("L4", 400, 300).c[3] == 0.f, "");
        // A transparent source over the square: nothing (cloning nothing doesn't erase).
        b.dx = 250; b.dy = 0;
        Pa::stroke(f.j, f.d, "L4", {{200, 175}}, b);
        check("clone: a transparent source erases nothing", f.px("L4", 200, 175) == P::rgba(1, 0, 0, 1), "");
        // Half opacity: half way.
        b.dx = -250; b.dy = -125;
        b.opacity = 0.5;
        Pa::stroke(f.j, f.d, "L4", {{450, 300}}, b);
        check("clone: opacity 50% -> half alpha", near(f.px("L4", 450, 300).c[3], 0.5, 0.01), num(f.px("L4", 450, 300).c[3]));
    }
    {
        Fix f;   // All Layers onto the empty L5: red over white, white
        std::string why;
        Pa::Brush b = hard(30);
        b.source = Pa::clone_source(f.d, "L5", true, why);
        check("clone all layers: a source", b.source && why.empty(), why);
        b.dx = -350; b.dy = -50;   // from (50, 250): the white Background
        Pa::stroke(f.j, f.d, "L5", {{400, 300}}, b);
        const auto w = f.px("L5", 400, 300);
        check("clone all layers: what the eye sees lands on the empty layer", near(w.c[0], 1) && near(w.c[1], 1) && near(w.c[3], 1), num(w.c[1]));
        b.dx = -350; b.dy = 75;    // from (150, 175): the red square
        Pa::stroke(f.j, f.d, "L5", {{500, 100}}, b);
        check("clone all layers: red from the group's layer", near(f.px("L5", 500, 100).c[0], 1) && near(f.px("L5", 500, 100).c[1], 0), num(f.px("L5", 500, 100).c[1]) + " src " + num(b.source->get(150, 175).c[1]));
    }
    {
        Fix f;   // Through a selection.
        L::load_selection(f.d, {380, 0, 220, 400});
        std::string why;
        Pa::Brush b = hard(60);
        b.source = Pa::clone_source(f.d, "L5", true, why);
        b.dx = -300; b.dy = 0;
        Pa::stroke(f.j, f.d, "L5", {{380, 50}}, b);
        check("clone: inside the selection", f.px("L5", 390, 50).c[3] > 0.9f, "");
        check("clone: not outside it", f.px("L5", 370, 50).c[3] == 0.f, "");
    }
    {
        Fix f;   // Gray: All Layers refused (the flattened picture is RGB).
        auto im = f.d.image();
        im.format.mode = P::Mode::Gray;
        { core::Document::Loading load(f.d); f.d.set_image(im); }
        std::string why;
        const auto s = Pa::clone_source(f.d, "L5", true, why);
        check("clone all layers on Gray: refused, saying why", !s && !why.empty(), why);
    }

    std::cout << "\n-- retouching: healing (s015 m3) --\n";
    {
        // A ramp 0.2 .. 0.8 across 600 px on the Background, a dark spot at (300, 200).
        Fix f;
        const auto ramp = [](int x) { return 0.2f + 0.6f * static_cast<float>(x) / 599.f; };
        auto bg = *f.d.get("L1");
        bg.pixels = std::make_shared<const P::TileMap>(bg.pixels->paint({0, 0, 600, 400}, [&](int x, int y, const P::Px&) {
            const bool spot = (x - 300) * (x - 300) + (y - 200) * (y - 200) <= 64;
            const float v = spot ? 0.f : ramp(x);
            return P::rgba(v, v, v, 1);
        }));
        bg.locks = 0;
        { core::Document::Loading load(f.d); f.d.put("L1", bg); }
        const auto base = bg.pixels;
        Pa::Brush b = hard(30);
        check("heal: a step", core::heal::stroke(f.j, f.d, "L1", {{300, 200, 1.f}}, b, false, 100, 0), "");
        check("heal: its label", f.j.undo_label() == "Healing Brush", f.j.undo_label());
        const float h = f.px("L1", 300, 200).c[0];
        check("heal: the spot takes the light from around it (not the source's)", near(h, ramp(300), 0.025), num(h) + " want " + num(ramp(300)));
        const float h2 = f.px("L1", 306, 200).c[0];
        check("heal: across the stroke, the ramp goes on", near(h2, ramp(306), 0.025), num(h2));
        f.j.undo();
        check("heal: undo", f.d.get("L1")->pixels == base, "");
        // A plain clone from the same place keeps the source's light -- the difference healing makes.
        std::string why;
        Pa::Brush c = hard(30);
        c.source = Pa::clone_source(f.d, "L1", false, why);
        c.dx = 100;
        Pa::stroke(f.j, f.d, "L1", {{300, 200}}, c);
        check("heal vs clone: the clone is 0.1 too light", near(f.px("L1", 300, 200).c[0], ramp(400), 0.02), num(f.px("L1", 300, 200).c[0]));
        f.j.undo();
        // Spot: it finds its own source.
        check("spot heal: a step", core::heal::stroke(f.j, f.d, "L1", {{300, 200, 1.f}}, b, true, 0, 0), "");
        check("spot heal: its label", f.j.undo_label() == "Spot Healing", f.j.undo_label());
        check("spot heal: the spot is gone", near(f.px("L1", 300, 200).c[0], ramp(300), 0.03), num(f.px("L1", 300, 200).c[0]));
        f.j.undo();
        // Spot avoids another blemish: a dark block to the left.
        auto bg2 = *f.d.get("L1");
        bg2.pixels = std::make_shared<const P::TileMap>(bg2.pixels->fill({200, 100, 75, 200}, P::rgba(0, 0, 0, 1)));
        { core::Document::Loading load(f.d); f.d.put("L1", bg2); }
        P::TileMap cov = Pa::new_coverage(600, 400);
        core::TileSet t = core::tiles_of(f.d.image());
        cov = Pa::add_pen_dabs(cov, b, {{300, 200, 1.f}}, t);
        const auto o = core::heal::spot_offset(*f.d.get("L1")->pixels, cov, 30);
        check("spot heal: doesn't take from the dark block (left)", o && o->dx >= 0, o ? num(o->dx) : "none");
    }
    {
        // Detail moves: a fine pattern in the source lands on a smooth destination.
        Fix f;
        auto bg = *f.d.get("L1");
        bg.pixels = std::make_shared<const P::TileMap>(bg.pixels->paint({0, 0, 600, 400}, [](int x, int y, const P::Px&) {
            const float v = x >= 400 ? (((x + y) & 1) ? 0.6f : 0.4f) : 0.5f;
            return P::rgba(v, v, v, 1);
        }));
        bg.locks = 0;
        { core::Document::Loading load(f.d); f.d.put("L1", bg); }
        core::heal::stroke(f.j, f.d, "L1", {{200, 200, 1.f}}, hard(30), false, 250, 0);
        const float a = f.px("L1", 200, 200).c[0], c = f.px("L1", 201, 200).c[0];
        check("heal: the source's texture comes along", std::abs(a - c) > 0.15f && near((a + c) / 2, 0.5, 0.03), num(a) + " " + num(c));
    }
    {
        // On an empty layer, sampling All Layers: the healed colours land there.
        Fix f;
        auto bg = *f.d.get("L1");
        bg.pixels = std::make_shared<const P::TileMap>(bg.pixels->fill({280, 180, 40, 40}, P::rgba(0.2f, 0.2f, 0.2f, 1)));
        { core::Document::Loading load(f.d); f.d.put("L1", bg); }
        check("heal on an empty layer (All Layers): a step", core::heal::stroke(f.j, f.d, "L5", {{450, 300, 1.f}}, hard(20), false, 0, 0, true), "");
        check("heal on an empty layer: opaque, the white around", near(f.px("L5", 450, 300).c[3], 1) && near(f.px("L5", 450, 300).c[0], 1, 0.02), "");
    }
}
