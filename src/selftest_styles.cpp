// klimt_selftest -- LAYER STYLES (s019): the distance transform; Drop Shadow,
// Outer Glow, Stroke (outside / inside / centre), Inner Shadow, Color
// Overlay; fill vs opacity; the verbs, the file, the cache.
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Live.hpp"
#include "core/StylesRender.hpp"
#include "core/Patterns.hpp"   // s028 m4

#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace S = klimt::core::styles;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
bool near(double a, double b, double eps = 0.01) { return std::abs(a - b) <= eps; }
std::string num(double v) { return std::to_string(v); }
P::TileMap square() {   // red, (100, 100) 200 x 150, on transparent, 600 x 400
    P::Format f; f.depth = P::Depth::F32;
    return P::TileMap(f, 600, 400).fill({100, 100, 200, 150}, P::rgba(1, 0, 0, 1));
}
}  // namespace

void selftest_styles(const Check& check) {
    std::cout << "\n-- layer styles: the distance transform (s019) --\n";
    {
        std::vector<char> seed(20 * 20, 0);
        seed[10 * 20 + 10] = 1;
        const auto d = S::distance(seed, 20, 20);
        check("distance: 3-4-5 from the seed", near(d[(10 + 4) * 20 + 13], 5, 1e-4) && d[10 * 20 + 10] == 0.f && near(d[10 * 20 + 19], 9, 1e-4),
              num(d[(10 + 4) * 20 + 13]));
    }
    std::cout << "\n-- layer styles: the effects (s019) --\n";
    const auto src = square();
    {
        core::Styles s;
        s.drop.on = true;
        s.drop.distance = 10;   // angle 120: away from the upper-left light -> down and right
        s.drop.size = 0;
        const auto out = S::drawn(src, s, 100);
        const auto br = out.get(303, 255), tl = out.get(97, 97);
        check("drop shadow: below and right of the square", br.c[3] > 0.7f && br.c[0] < 0.05f, num(br.c[3]));
        check("drop shadow: not above-left", tl.c[3] == 0.f, num(tl.c[3]));
        check("drop shadow: the content stays on top", out.get(200, 175) == P::rgba(1, 0, 0, 1), "");
        check("drop shadow: 75 % strong", near(br.c[3], 0.75, 0.01), num(br.c[3]));
        const auto none = S::drawn(src, s, 0);
        check("fill 0: the content gone, the shadow left", none.get(200, 175).c[3] == 0.f && near(none.get(303, 255).c[3], 0.75, 0.01), "");
        s.drop.size = 10;
        const auto soft = S::drawn(src, s, 100);
        check("drop shadow: size softens it", soft.get(312, 262).c[3] > 0.f && soft.get(312, 262).c[3] < 0.7f, num(soft.get(312, 262).c[3]));
    }
    {
        core::Styles s;
        s.outer.on = true;
        s.outer.size = 10;
        const auto out = S::drawn(src, s, 100);
        const auto g = out.get(96, 175);
        check("outer glow: a pale yellow just outside", g.c[3] > 0.2f && g.c[0] > 0.9f && g.c[2] < 0.85f, num(g.c[3]));
        check("outer glow: fades out", out.get(80, 175).c[3] < g.c[3], num(out.get(80, 175).c[3]));
    }
    {
        core::Styles s;
        s.stroke.on = true;
        s.stroke.size = 4;
        const auto out = S::drawn(src, s, 100);
        check("stroke outside: black, 4 px out", out.get(97, 175) == P::rgba(0, 0, 0, 1) && out.get(96, 175).c[3] > 0.99f, num(out.get(96, 175).c[3]));
        check("stroke outside: none past it", out.get(95, 175).c[3] == 0.f, num(out.get(95, 175).c[3]));
        check("stroke outside: the content untouched", out.get(100, 175) == P::rgba(1, 0, 0, 1), "");
        check("stroke outside: round at the corner", out.get(98, 98).c[3] > 0.9f && out.get(96, 96).c[3] < 0.5f, num(out.get(96, 96).c[3]));
        s.stroke.position = 1;
        const auto in = S::drawn(src, s, 100);
        check("stroke inside: over the content's edge, 4 px in", in.get(103, 175).c[0] < 0.05f && in.get(104, 175).c[0] > 0.95f && in.get(98, 175).c[3] == 0.f, "");
        s.stroke.position = 2;
        const auto mid = S::drawn(src, s, 100);
        check("stroke centred: both sides", mid.get(98, 175).c[3] > 0.9f && mid.get(101, 175).c[0] < 0.05f && mid.get(105, 175).c[0] > 0.95f, "");
    }
    {
        core::Styles s;
        s.overlay.on = true;
        s.overlay.colour = {0, 0, 1};
        const auto out = S::drawn(src, s, 100);
        check("color overlay: blue over the red, alpha kept", out.get(200, 175) == P::rgba(0, 0, 1, 1) && out.get(90, 175).c[3] == 0.f, "");
        s.overlay.opacity = 50;
        const auto half = S::drawn(src, s, 100).get(200, 175);
        check("color overlay at 50 %", near(half.c[0], 0.5, 0.01) && near(half.c[2], 0.5, 0.01), num(half.c[0]));
    }
    {
        core::Styles s;
        s.inner.on = true;
        s.inner.distance = 10;
        s.inner.size = 0;
        const auto out = S::drawn(src, s, 100);
        check("inner shadow: along the top-left inside", out.get(103, 175).c[0] < 0.4f && out.get(200, 106).c[0] < 0.4f, num(out.get(103, 175).c[0]));
        check("inner shadow: not the bottom-right inside", out.get(296, 175).c[0] > 0.95f, num(out.get(296, 175).c[0]));
        check("inner shadow: nothing outside", out.get(90, 175).c[3] == 0.f, "");
    }
    std::cout << "\n-- layer styles: on a layer (s019) --\n";
    {
        core::live::clear_cache();
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);   // white Background, L4 the red square
        const auto seen = [&](int x, int y) { return core::sample_composite(d.image(), d.snapshot(), x, y, 0); };
        core::Styles s;
        s.stroke.on = true;
        s.stroke.size = 4;
        const auto px0 = d.get("L4")->pixels;
        check("set_styles: ONE step", L::set_styles(j, d, "L4", s) && j.size() == 1 && j.undo_label().find("Layer Style") != std::string::npos, j.undo_label());
        check("styles: the pixels never change", d.get("L4")->pixels == px0, "");
        check("styles: the canvas shows the stroke", seen(97, 175).r < 0.05f && seen(97, 175).g < 0.05f, num(seen(97, 175).r));
        s.stroke.size = 6;
        L::set_styles(j, d, "L4", s);
        check("styles: presses fold into one step", j.size() == 1, num(static_cast<double>(j.size())));
        // Fill hides the content, not the effects; opacity both.
        L::set_fill(j, d, std::vector<std::string>{"L4"}, 0);
        check("fill 0: the square's inside shows white, its stroke stays", seen(200, 175).g > 0.99f && seen(97, 175).g < 0.05f, num(seen(200, 175).g));
        L::set_opacity(j, d, std::vector<std::string>{"L4"}, 0);
        check("opacity 0: the stroke goes too", seen(97, 175).g > 0.99f, "");
        j.undo(); j.undo();
        core::Styles t;
        t.drop.on = true;
        const auto before_drag = d.get("L4")->styles;
        L::styles_begin(j, d, "L4");
        for (int k = 5; k <= 25; k += 5) { t.drop.distance = k; L::styles_to(j, d, "L4", t); }
        L::styles_end(j);
        const bool dragged = d.get("L4")->styles.drop.distance == 25;
        j.undo();
        check("a drag: ONE step (one undo takes it all back)", dragged && d.get("L4")->styles == before_drag, "");
        j.redo();
        check("styles: on a group too (s024)", L::set_styles(j, d, "L2", t) && (j.undo(), true), "");
        // File.
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_styles_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image(); c.layers = d.snapshot(); c.next_id = d.next_id();
        std::string err;
        core::file::save_klimt(path, c, err);
        const auto back = core::file::open(path, err);
        check("styles: in the file", back && back->layers.at("L4").styles == d.get("L4")->styles, err);
        fs::remove(path);
        check("styles: the look reaches past the pixels' tiles (a shadow beyond)",
              core::live::look(*d.get("L4"))->get(308, 265).c[3] > 0.f, "");
    }
    {
        core::live::clear_cache();
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        core::Styles s;
        s.stroke.on = true;
        L::set_styles(j, d, "L4", s);
        L::set_fill(j, d, std::vector<std::string>{"L4"}, 40);
        const auto look = core::live::look(*d.get("L4"));
        check("rasterize layer style: a step", L::bake_live(j, d, "L4") && j.undo_label().find("Rasterize Layer Style") != std::string::npos, j.undo_label());
        const auto l = *d.get("L4");
        check("rasterize layer style: the look is the pixels; no styles; fill 100", l.pixels == look && !l.styles.any() && l.fill == 100, "");
        check("paste style: groups take it too (s024), one step",
              L::set_styles_on(j, d, {"L5", "L2"}, s, "Paste Layer Style") && d.get("L5")->styles == s && d.get("L2")->styles == s, "");
    }
    std::cout << "\n-- layer styles: bevel & emboss (s021) --\n";
    {
        const auto src = square();   // red 200 x 150 at (100, 100)
        core::Styles s;
        s.bevel.on = true;
        s.bevel.size = 10;
        const auto out = S::drawn(src, s, 100);
        const auto lum = [&](int x, int y) { const auto p = out.get(x, y); return (p.c[0] + p.c[1] + p.c[2]) / 3; };
        const float flat = lum(200, 175);
        check("bevel: the middle untouched", near(out.get(200, 175).c[0], 1) && near(out.get(200, 175).c[1], 0), num(flat));
        check("bevel up, light upper left: the top-left edge lit", lum(103, 175) > flat + 0.1 && lum(200, 103) > flat + 0.1, num(lum(103, 175)));
        check("bevel up: the bottom-right edge shaded", lum(296, 175) < flat - 0.1 && lum(200, 246) < flat - 0.1, num(lum(296, 175)));
        check("inner bevel: nothing outside", out.get(95, 175).c[3] == 0.f, "");
        s.bevel.up = false;
        const auto down = S::drawn(src, s, 100);
        const auto dl = [&](int x, int y) { const auto p = down.get(x, y); return (p.c[0] + p.c[1] + p.c[2]) / 3; };
        check("bevel down: turned over (top-left shaded)", dl(103, 175) < flat - 0.1 && dl(296, 175) > flat + 0.1, num(dl(103, 175)));
        s.bevel.up = true;
        s.bevel.style = 1;
        const auto emb = S::drawn(src, s, 100);
        check("emboss: either side of the edge (outside too)", emb.get(95, 175).c[3] > 0.05f, num(emb.get(95, 175).c[3]));
        check("bevel: reach counts its size", S::reach(s) >= 10, num(S::reach(s)));
        // In the file.
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        s.bevel.depth = 250; s.bevel.soften = 3; s.bevel.altitude = 45; s.bevel.highlight = {1, 0.9f, 0.5f}; s.bevel.shadow_opacity = 40;
        L::set_styles(j, d, "L4", s);
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_bevel_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image(); c.layers = d.snapshot(); c.next_id = d.next_id();
        std::string err;
        core::file::save_klimt(path, c, err);
        const auto back = core::file::open(path, err);
        check("bevel: in the file", back && back->layers.at("L4").styles.bevel == s.bevel, err);
        fs::remove(path);
    }
    std::cout << "\n-- layer styles: inner glow (s023) --\n";
    {
        const auto src = square();   // red 200 x 150 at (100, 100)
        core::Styles s;
        s.inner_glow.on = true;
        s.inner_glow.size = 10;
        s.inner_glow.colour = {0, 1, 0};
        s.inner_glow.opacity = 100;
        const auto out = S::drawn(src, s, 100);
        check("inner glow (edge): the edge glows", out.get(101, 175).c[1] > 0.5f, num(out.get(101, 175).c[1]));
        check("inner glow (edge): the middle untouched", out.get(200, 175).c[1] < 0.02f && out.get(200, 175).c[0] > 0.98f, num(out.get(200, 175).c[1]));
        check("inner glow: nothing outside", out.get(95, 175).c[3] == 0.f, "");
        s.inner_glow.center = true;
        const auto c = S::drawn(src, s, 100);
        check("inner glow (center): the middle glows, the edge less", c.get(200, 175).c[1] > 0.9f && c.get(101, 175).c[1] < c.get(200, 175).c[1],
              num(c.get(101, 175).c[1]));
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        s.inner_glow.choke = 30;
        L::set_styles(j, d, "L4", s);
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_iglow_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents fc;
        fc.image = d.image(); fc.layers = d.snapshot(); fc.next_id = d.next_id();
        std::string err;
        core::file::save_klimt(path, fc, err);
        const auto back = core::file::open(path, err);
        check("inner glow: in the file", back && back->layers.at("L4").styles.inner_glow == s.inner_glow, err);
        fs::remove(path);
    }
    std::cout << "\n-- layer styles on groups (s024) --\n";
    {
        core::live::clear_cache();
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);   // the red square A (L4) inside the group L2, on white
        const auto seen = [&](int x, int y) { return core::sample_composite(d.image(), d.snapshot(), x, y, 0); };
        core::Styles s;
        s.stroke.on = true;
        s.stroke.size = 6;
        s.stroke.colour = {0, 0, 1};
        check("group style: one step", L::set_styles(j, d, "L2", s) && j.size() == 1, "");
        check("group style: the stroke round the group's content (outside the square)", seen(96, 175).b > 0.9f && seen(96, 175).r < 0.1f,
              std::to_string(seen(96, 175).b));
        check("group style: the content inside untouched", seen(200, 175).r > 0.99f && seen(200, 175).b < 0.01f, "");
        // A layer added inside: the stroke follows the group's new shape.
        const auto nid = L::add_layer(j, d, "In", "L4");
        L::fill(j, d, nid, {400, 300, 50, 50}, P::rgba(0, 1, 0, 1));
        check("group style: follows its contents (a new layer inside gets the stroke too)", seen(396, 325).b > 0.9f, std::to_string(seen(396, 325).b));
        // Fill 0 on the group: only the effects.
        L::set_fill(j, d, std::vector<std::string>{"L2"}, 0);
        check("group fill 0: content gone, stroke stays", seen(200, 175).g > 0.99f && seen(96, 175).b > 0.9f, "");
        j.undo(); j.undo(); j.undo(); j.undo();
        check("group style: undone", !d.get("L2")->styles.any() && seen(96, 175).b > 0.99f && seen(96, 175).r > 0.99f, "");
    }
    std::cout << "\n-- layer styles: blend modes per effect (s025) --\n";
    {
        const auto src = square();   // red (1, 0, 0)
        core::Styles s;
        s.overlay.on = true;
        s.overlay.colour = {0, 1, 0};
        s.overlay.mode = P::Blend::Normal;
        check("overlay Normal: green replaces red", near(S::drawn(src, s, 100).get(200, 175).c[1], 1) && near(S::drawn(src, s, 100).get(200, 175).c[0], 0), "");
        s.overlay.mode = P::Blend::Screen;
        const auto sc = S::drawn(src, s, 100).get(200, 175);
        check("overlay Screen: red + green = yellow", near(sc.c[0], 1) && near(sc.c[1], 1), num(sc.c[0]) + "," + num(sc.c[1]));
        s.overlay.mode = P::Blend::Multiply;
        const auto mu = S::drawn(src, s, 100).get(200, 175);
        check("overlay Multiply: red x green = black", near(mu.c[0], 0) && near(mu.c[1], 0), num(mu.c[0]));
        core::Styles t;
        t.stroke.on = true;
        t.stroke.position = 1;   // inside: over the content
        t.stroke.colour = {0, 0, 1};
        t.stroke.size = 4;
        t.stroke.mode = P::Blend::Lighten;
        const auto li = S::drawn(src, t, 100).get(101, 175);
        check("stroke Lighten inside: max(red, blue) = magenta", near(li.c[0], 1) && near(li.c[2], 1), num(li.c[0]) + "," + num(li.c[2]));
        // Defaults: Photoshop's for new effects.
        core::Styles d;
        check("defaults: shadows Multiply, glows Screen, stroke / overlay Normal, bevel Screen / Multiply",
              d.drop.mode == P::Blend::Multiply && d.inner.mode == P::Blend::Multiply && d.outer.mode == P::Blend::Screen &&
                  d.inner_glow.mode == P::Blend::Screen && d.stroke.mode == P::Blend::Normal && d.overlay.mode == P::Blend::Normal &&
                  d.bevel.highlight_mode == P::Blend::Screen && d.bevel.shadow_mode == P::Blend::Multiply, "");
        // The file keeps them.
        core::Document doc;
        core::DocJournal j{doc};
        doc.set_door([&] { return j.open(); });
        L::load_fixture(doc);
        core::Styles f;
        f.drop.on = true; f.drop.mode = P::Blend::LinearBurn;
        f.outer.on = true; f.outer.mode = P::Blend::ColorDodge;
        f.overlay.on = true; f.overlay.mode = P::Blend::Hue;
        f.bevel.on = true; f.bevel.highlight_mode = P::Blend::Overlay;
        L::set_styles(j, doc, "L4", f);
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_fxmode_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = doc.image(); c.layers = doc.snapshot(); c.next_id = doc.next_id();
        std::string err;
        core::file::save_klimt(path, c, err);
        const auto back = core::file::open(path, err);
        fs::remove(path);
        check("effect modes: in the file", back && back->layers.at("L4").styles == f, err);
    }
    std::cout << "\n-- layer styles: shadows and glows blend with what is below (s025) --\n";
    {
        core::live::clear_cache();
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        {   // a blue Background
            core::Document::Loading load(d);
            auto bg = *d.get("L1");
            bg.pixels = std::make_shared<const P::TileMap>(d.blank().fill({0, 0, 600, 400}, P::rgba(0, 0, 1, 1)));
            d.put("L1", bg);
        }
        const auto seen = [&](int x, int y) { return core::sample_composite(d.image(), d.snapshot(), x, y, 0); };
        core::Styles s;
        s.drop.on = true;
        s.drop.colour = {1, 0, 0};
        s.drop.opacity = 100;
        s.drop.distance = 20;
        s.drop.size = 0;
        s.drop.mode = P::Blend::Normal;
        L::set_styles(j, d, "L4", s);
        const auto normal = seen(305, 175);   // right of the square: the shadow only
        s.drop.mode = P::Blend::Multiply;
        L::set_styles(j, d, "L4", s);
        const auto mult = seen(305, 175);
        check("drop shadow Normal: red over the blue", normal.r > 0.9f && normal.b < 0.1f, num(normal.r));
        check("drop shadow Multiply: red x blue = black (it blends with what is below)", mult.r < 0.05f && mult.b < 0.05f,
              num(mult.r) + "," + num(mult.b));
        check("the square itself unchanged by the split", seen(200, 175).r > 0.99f && seen(200, 175).b < 0.01f, "");
        s.drop.on = false;
        s.outer.on = true;
        s.outer.colour = {1, 1, 0};
        s.outer.opacity = 100;
        s.outer.size = 10;
        s.outer.mode = P::Blend::Screen;
        L::set_styles(j, d, "L4", s);
        const auto gl = seen(97, 175);
        check("outer glow Screen: yellow screened on blue -> toward white", gl.r > 0.5f && gl.g > 0.5f && gl.b > 0.9f,
              num(gl.r) + "," + num(gl.g) + "," + num(gl.b));
        // A clipped layer keeps its effects inside (no split).
        { core::Document::Loading load(d); auto v = *d.get("L4"); v.clipped = true; d.put("L4", v); }
        check("a clipped styled layer: drawn as before (no stray part)", d.check().empty(), d.check());
        core::live::clear_cache();
    }
    std::cout << "\n-- layer styles: gradient overlay (s025) --\n";
    {
        const auto src = square();   // (100, 100) 200 x 150
        core::Styles s;
        s.gradient.on = true;
        s.gradient.from = {0, 0, 0};
        s.gradient.to = {1, 1, 1};
        s.gradient.angle = 0;   // left (from) to right (to)
        const auto lin = S::drawn(src, s, 100);
        check("gradient linear 0°: dark at the left, light at the right", lin.get(101, 175).c[1] < 0.05f && lin.get(298, 175).c[1] > 0.95f &&
                  near(lin.get(200, 175).c[1], 0.5, 0.03), num(lin.get(200, 175).c[1]));
        check("gradient: only on the content", lin.get(95, 175).c[3] == 0.f, "");
        s.gradient.reverse = true;
        check("gradient reversed: light at the left", S::drawn(src, s, 100).get(101, 175).c[1] > 0.95f, "");
        s.gradient.reverse = false;
        s.gradient.style = 1;
        const auto rad = S::drawn(src, s, 100);
        check("gradient radial: dark in the middle, lighter toward the corners", rad.get(200, 175).c[1] < 0.05f && rad.get(102, 102).c[1] > 0.8f,
              num(rad.get(102, 102).c[1]));
        s.gradient.angle = 90;
        s.gradient.style = 0;
        const auto up = S::drawn(src, s, 100);
        check("gradient linear 90°: dark at the bottom, light at the top", up.get(200, 248).c[1] < 0.05f && up.get(200, 101).c[1] > 0.95f, "");
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        s.gradient.scale = 80; s.gradient.opacity = 60; s.gradient.mode = P::Blend::Overlay; s.gradient.from = {0.2f, 0.4f, 0.6f};
        L::set_styles(j, d, "L4", s);
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_grad_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image(); c.layers = d.snapshot(); c.next_id = d.next_id();
        std::string err;
        core::file::save_klimt(path, c, err);
        const auto back = core::file::open(path, err);
        fs::remove(path);
        check("gradient overlay: in the file", back && back->layers.at("L4").styles.gradient == s.gradient, err);
    }
    std::cout << "\n-- layer styles: satin (s026) --\n";
    {
        const auto src = square();   // red 200 x 150 at (100, 100)
        core::Styles s;
        s.satin.on = true;
        s.satin.invert = false;
        s.satin.opacity = 100;
        s.satin.mode = P::Blend::Normal;
        s.satin.angle = 0;
        s.satin.distance = 10;
        s.satin.size = 0;
        const auto out = S::drawn(src, s, 100);
        // Shifted 10 px left and right: they differ within 10 px of the left / right edges; agree in the middle.
        check("satin: dark near the edges along its angle", out.get(105, 175).c[0] < 0.05f && out.get(295, 175).c[0] < 0.05f, num(out.get(105, 175).c[0]));
        check("satin: untouched where the copies agree", out.get(200, 175).c[0] > 0.99f, num(out.get(200, 175).c[0]));
        check("satin: nothing outside the content", out.get(95, 175).c[3] == 0.f, "");
        s.satin.invert = true;
        const auto inv = S::drawn(src, s, 100);
        check("satin inverted: the middle shaded, the edges not", inv.get(200, 175).c[0] < 0.05f && inv.get(105, 175).c[0] > 0.95f, "");
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        s.satin.colour = {0.1f, 0.2f, 0.9f}; s.satin.mode = P::Blend::Overlay; s.satin.size = 9;
        L::set_styles(j, d, "L4", s);
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_satin_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image(); c.layers = d.snapshot(); c.next_id = d.next_id();
        std::string err;
        core::file::save_klimt(path, c, err);
        const auto back = core::file::open(path, err);
        fs::remove(path);
        check("satin: in the file", back && back->layers.at("L4").styles.satin == s.satin, err);
    }
    std::cout << "\n-- layer styles: pattern overlay (s028 m4) --\n";
    {
        namespace Pt = klimt::core::pattern;
        const auto& pv = Pt::builtin();
        check("patterns: eight built in", pv.size() == 8, num(static_cast<double>(pv.size())));
        bool tiles = true, in01 = true, keys = true;
        for (int id = 0; id < static_cast<int>(pv.size()); ++id) {
            const double p = pv[static_cast<std::size_t>(id)].period;
            keys = keys && Pt::index_of(pv[static_cast<std::size_t>(id)].key) == id;
            for (int k = 0; k < 40; ++k) {
                const double x = k * 7 + 3, y = k * 5 + 1;
                const float g = Pt::grey(id, x, y, 1, 0, 0);
                in01 = in01 && g >= 0.f && g <= 1.f;
                tiles = tiles && std::abs(g - Pt::grey(id, x + p, y, 1, 0, 0)) < 1e-4f && std::abs(g - Pt::grey(id, x, y + 3 * p, 1, 0, 0)) < 1e-4f &&
                        std::abs(Pt::grey(id, x, y, 2, 0, 0) - Pt::grey(id, x + 2 * p, y, 2, 0, 0)) < 1e-4f;
            }
        }
        check("patterns: each repeats at its period (and twice it at 200 %)", tiles, "");
        check("patterns: greys in 0..1", in01, "");
        check("patterns: the keys find them; an unknown key doesn't", keys && Pt::index_of("nope") == -1, "");
        // The geometric ones have dark and light; the textures sit round middle grey.
        bool contrast = true, middle = true;
        for (int id = 0; id < static_cast<int>(pv.size()); ++id) {
            double lo = 1, hi = 0, sum = 0;
            int n = 0;
            for (int y = 0; y < 128; ++y)
                for (int x = 0; x < 128; ++x) {
                    const double g = Pt::grey(id, x, y, 1, 0, 0);
                    lo = std::min(lo, g); hi = std::max(hi, g); sum += g; ++n;
                }
            const std::string key = pv[static_cast<std::size_t>(id)].key;
            if (key == "paper" || key == "canvas") middle = middle && std::abs(sum / n - 0.5) < 0.1;
            else contrast = contrast && lo < 0.3 && hi > 0.95;
        }
        check("patterns: the geometric ones dark on white", contrast, "");
        check("patterns: Paper and Canvas round middle grey", middle, "");
        check("patterns: the offset moves it", std::abs(Pt::grey(2, 40, 7, 1, 10, 0) - Pt::grey(2, 30, 7, 1, 0, 0)) < 1e-6f &&
                                               Pt::grey(2, 10, 5, 1, 0, 0) != Pt::grey(2, 10, 5, 1, 16, 0), "");
        check("patterns: small scales are box-filtered (Checker at 10 %, a pixel across two cells: grey between)",
              [&] { const float g = Pt::grey(2, 51, 50, 0.1, 0, 0); return g > 0.2f && g < 0.95f; }(), num(Pt::grey(2, 51, 50, 0.1, 0, 0)));
    }
    {
        const auto src = square();   // red 200 x 150 at (100, 100)
        core::Styles s;
        s.pattern.on = true;
        s.pattern.pattern = 2;   // Checker: 16 px squares, (0..16, 0..16) light
        const auto out = S::drawn(src, s, 100);
        // (104, 104): cell (6, 6) -- light; (120, 104): cell (7, 6) -- dark.
        check("pattern overlay: Normal 100 % -- the pattern replaces the colour", near(out.get(104, 104).c[0], 1, 0.02) && near(out.get(104, 104).c[1], 1, 0.02) &&
                                                                              near(out.get(120, 104).c[0], 0.15, 0.03), num(out.get(120, 104).c[0]));
        check("pattern overlay: the alpha kept; nothing outside", out.get(120, 104).c[3] > 0.99f && out.get(95, 150).c[3] == 0.f, "");
        s.pattern.mode = P::Blend::Multiply;
        const auto mul = S::drawn(src, s, 100);
        check("pattern overlay: Multiply keeps the red, darkens it", near(mul.get(104, 104).c[0], 1, 0.02) && mul.get(104, 104).c[1] < 0.02f &&
                                                                    near(mul.get(120, 104).c[0], 0.15, 0.03), num(mul.get(120, 104).c[0]));
        s.pattern.mode = P::Blend::Normal;
        s.pattern.opacity = 50;
        const auto half = S::drawn(src, s, 100);
        check("pattern overlay: opacity 50 % -- half-way", near(half.get(120, 104).c[0], 0.575, 0.03), num(half.get(120, 104).c[0]));
        s.pattern.opacity = 100;
        s.gradient.on = true;   // Gradient Overlay sits over it
        const auto both = S::drawn(src, s, 100);
        check("pattern overlay: under Gradient Overlay", near(both.get(120, 104).c[0], both.get(104, 104).c[0], 0.05), "");
        s.gradient.on = false;
        // Anchored to the image: the same pixel, the same grey, wherever the content is.
        P::Format f; f.depth = P::Depth::F32;
        const auto other = P::TileMap(f, 600, 400).fill({110, 90, 100, 100}, P::rgba(0, 1, 0, 1));
        check("pattern overlay: fixed to the image, not the content's box", near(S::drawn(other, s, 100).get(120, 104).c[0], out.get(120, 104).c[0], 1e-4), "");
        // The verb, undo, the file.
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        s.pattern.pattern = 0; s.pattern.mode = P::Blend::Overlay; s.pattern.scale = 250; s.pattern.x = -12; s.pattern.y = 30;
        check("pattern overlay: a step", L::set_styles(j, d, "L4", s), "");
        check("pattern overlay: on the layer", d.get("L4")->styles.pattern == s.pattern, "");
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_pattern_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image(); c.layers = d.snapshot(); c.next_id = d.next_id();
        std::string err;
        core::file::save_klimt(path, c, err);
        const auto back = core::file::open(path, err);
        fs::remove(path);
        check("pattern overlay: in the file (by its key)", back && back->layers.at("L4").styles.pattern == s.pattern, err);
        j.undo();
        check("pattern overlay: undo", !d.get("L4")->styles.pattern.on, "");
    }
    std::cout << "\n-- your patterns (s033) --\n";
    {
        namespace Pt = klimt::core::pattern;
        // A 4 x 2 pattern: left half red, right half transparent.
        const P::Format pf{P::Depth::U8, P::Mode::RGB, true};
        const auto pat = std::make_shared<const P::TileMap>(P::TileMap(pf, 4, 2).fill({0, 0, 2, 2}, P::rgba(1, 0, 0, 1)));
        float o[4];
        Pt::sample(*pat, 0, 0, 1, 0, 0, o);
        check("sample: red where it's red", near(o[0], 1) && near(o[3], 1) && o[1] < 0.01f, "");
        Pt::sample(*pat, 2, 0, 1, 0, 0, o);
        check("sample: clear where it's clear", o[3] < 0.01f, "");
        Pt::sample(*pat, 4, 3, 1, 0, 0, o);
        check("sample: it tiles (4, 3) = (0, 1)", near(o[0], 1) && near(o[3], 1), "");
        Pt::sample(*pat, 1, 0, 1, 1, 0, o);
        check("sample: the offset moves it", near(o[3], 1), "");
        Pt::sample(*pat, 0, 0, 0.25, 0, 0, o);
        check("sample: at 25 % a pixel averages a whole tile (half covered)", near(o[3], 0.5, 0.02) && near(o[0], 1, 0.02), num(o[3]));
        // Define: what the eye sees inside the rectangle.
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);   // white Background, a red square (100, 100) 200 x 150
        std::string why;
        const auto def = Pt::define(d.image(), d.snapshot(), {90, 90, 20, 20}, why);
        check("define: the picture inside the rectangle", def && def->width() == 20 && def->height() == 20 &&
                                                          def->get(0, 0) == P::rgba(1, 1, 1, 1) && near(def->get(15, 15).c[0], 1) && def->get(15, 15).c[1] < 0.01f, why);
        const auto clipped = Pt::define(d.image(), d.snapshot(), {500, 0, 2000, 10}, why);
        check("define: clipped to the picture (500 + 100 wide)", clipped && clipped->width() == 100 && clipped->height() == 10, why);
        check("define: an empty rectangle refused", !Pt::define(d.image(), d.snapshot(), {700, 700, 10, 10}, why), why);
        // The library: a folder of PNGs.
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / ("klimt_patterns_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        Pt::set_library_dir(dir.string());
        check("library: empty at first", Pt::library().empty(), "");
        std::string err;
        const auto n1 = Pt::add_to_library(*def, "Bricks/Red", err);
        const auto n2 = Pt::add_to_library(*def, "Bricks_Red", err);
        check("library: added, the name made safe and unique", n1 == "Bricks_Red" && n2 == "Bricks_Red 2" && fs::exists(dir / "Bricks_Red.png"), n1 + " / " + n2);
        Pt::set_library_dir(dir.string());   // read back from the folder
        check("library: read back from the folder, sorted", Pt::library().size() == 2 && Pt::library()[0].name == "Bricks_Red" &&
                                                            Pt::find("Bricks_Red 2") && Pt::find("Bricks_Red")->get(15, 15) == def->get(15, 15), "");
        check("library: remove", Pt::remove_from_library("Bricks_Red 2") && !fs::exists(dir / "Bricks_Red 2.png") && Pt::library().size() == 1, "");
        Pt::set_library_dir("");
        fs::remove_all(dir);
        // Drawn: yours over the content, through its own alpha.
        const auto src = square();
        core::Styles st;
        st.pattern.on = true;
        st.pattern.user = "Half";
        st.pattern.image = std::make_shared<const P::TileMap>(P::TileMap(pf, 4, 2).fill({0, 0, 2, 2}, P::rgba(0, 0, 1, 1)));   // blue | clear
        const auto out = S::drawn(src, st, 100);
        check("drawn: blue where the pattern is", out.get(100, 120).c[2] > 0.99f && out.get(100, 120).c[0] < 0.01f, "");
        check("drawn: the content where the pattern is clear", out.get(102, 120).c[0] > 0.99f && out.get(102, 120).c[2] < 0.01f, "");
        check("drawn: nothing outside the content", out.get(98, 120).c[3] == 0.f, "");
        // The step and the file: yours travels inside the .klimt.
        L::set_styles(j, d, "L4", st);
        const auto path = (fs::temp_directory_path() / ("klimt_userpat_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image(); c.layers = d.snapshot(); c.next_id = d.next_id();
        std::string ferr;
        const bool saved = core::file::save_klimt(path, c, ferr);
        const auto back = core::file::open(path, ferr);
        fs::remove(path);
        const auto& bp = back ? back->layers.at("L4").styles.pattern : core::PatternFx{};
        check("file: your pattern kept -- its name, size, pixels", saved && back && bp.image && bp.user == "Half" && bp.image->width() == 4 &&
                                                                    bp.image->height() == 2 && bp.image->get(1, 1) == st.pattern.image->get(1, 1) &&
                                                                    bp.image->get(3, 0) == st.pattern.image->get(3, 0), ferr);
    }
}
