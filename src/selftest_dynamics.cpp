// klimt_selftest -- BRUSH DYNAMICS (s028): Shape Dynamics, Scattering and
// Count (m1); Color Dynamics (m2). Rules, not pictures: the dice are seeded,
// but every check holds for any draw.
#include "core/Document.hpp"
#include "core/Paint.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <set>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace Pa = klimt::core::paint;
using Check = std::function<void(const std::string&, bool, const std::string&)>;
namespace { bool near(double a, double b, double e) { return std::abs(a - b) <= e; } }

namespace {
std::string num(double v) { return std::to_string(v); }
Pa::Brush hard(double size) { Pa::Brush b; b.size = size; b.hardness = 1; return b; }
std::vector<Pa::Dab> line(const Pa::Brush& b, double x0, double y0, double x1, double y1) {
    double carry = 1e9;
    return Pa::dabs_along(Pa::Dab{x0, y0}, Pa::Dab{x1, y1}, b, carry);
}
core::TileSet grid_for(int w, int h) {
    core::TileSet t;
    t.cols = (w + P::kTile - 1) / P::kTile;
    t.rows = (h + P::kTile - 1) / P::kTile;
    t.on.assign(static_cast<std::size_t>(t.cols) * static_cast<std::size_t>(t.rows), 0);
    return t;
}
}  // namespace

void selftest_dynamics(const Check& check) {
    std::cout << "\n-- brush dynamics: shape, scatter, count (s028 m1) --\n";
    {
        // No dynamics: the dabs come back untouched.
        Pa::Brush b = hard(20);
        const auto in = line(b, 0, 0, 100, 0);
        Pa::Jitter jit;
        const auto out = jit.apply(in, b);
        bool same = out.size() == in.size();
        for (std::size_t i = 0; same && i < in.size(); ++i)
            same = out[i].x == in[i].x && out[i].y == in[i].y && out[i].size == 1.f && out[i].turn == 0.f && out[i].round == 1.f && !out[i].tinted;
        check("dynamics off: the dabs untouched", same, num(static_cast<double>(out.size())));
        check("dynamics off: any() is false", !b.dyn.any(), "");
    }
    {
        // The dice: in 0..1, and the same seed gives the same stroke.
        Pa::Jitter a(7), c(7), e(8);
        bool in01 = true, same = true, differ = false;
        for (int i = 0; i < 2000; ++i) {
            const double x = a.next(), y = c.next(), z = e.next();
            in01 = in01 && x >= 0 && x < 1;
            same = same && x == y;
            differ = differ || x != z;
        }
        check("dice: in [0, 1)", in01, "");
        check("dice: the same seed, the same draws", same, "");
        check("dice: another seed, other draws", differ, "");
    }
    {
        // Size jitter: every dab between the floor and the brush's size, and they vary.
        Pa::Brush b = hard(40);
        b.dyn.size_jitter = 1;
        b.dyn.min_size = 0.3;
        Pa::Jitter jit;
        const auto out = jit.apply(line(b, 0, 0, 400, 0), b);
        float lo = 2, hi = -1;
        for (const auto& d : out) { lo = std::min(lo, d.size); hi = std::max(hi, d.size); }
        check("size jitter: never above the brush, never below the floor", lo >= 0.3f - 1e-6f && hi <= 1.f, num(lo) + " .. " + num(hi));
        check("size jitter: the sizes vary", hi - lo > 0.3f, num(hi - lo));
        check("size jitter: the radius follows", std::abs(Pa::dab_radius(b, out[0]) - 20 * out[0].size) < 1e-4 || Pa::dab_radius(b, out[0]) == 0.5, "");
    }
    {
        // Angle and roundness jitter: within their ranges; the tip turns with the dab.
        Pa::Brush b = hard(40);
        b.dyn.angle_jitter = 0.5;
        b.dyn.roundness_jitter = 1;
        b.dyn.min_roundness = 0.4;
        Pa::Jitter jit;
        const auto out = jit.apply(line(b, 0, 0, 400, 0), b);
        bool ok = true;
        float tmin = 1e9f, tmax = -1e9f;
        for (const auto& d : out) {
            ok = ok && std::abs(d.turn) <= 90.f + 1e-4f && d.round >= 0.4f - 1e-6f && d.round <= 1.f;
            tmin = std::min(tmin, d.turn);
            tmax = std::max(tmax, d.turn);
        }
        check("angle jitter 50%: within +-90 degrees; roundness above its floor", ok, "");
        check("angle jitter: both ways", tmin < -10 && tmax > 10, num(tmin) + " .. " + num(tmax));
        // A flat dab turned 90 degrees stands up -- the turn reaches tip_alpha.
        Pa::Brush f = hard(40);
        f.roundness = 0.25;
        Pa::Dab d0{0, 0, 1.f}, d90{0, 0, 1.f};
        d90.turn = 90;
        check("the dab's turn turns the tip", Pa::tip_alpha(16, 0, 20, f, d0, 0, 0) > 0.9f && Pa::tip_alpha(16, 0, 20, f, d90, 0, 0) == 0.f &&
                                                Pa::tip_alpha(0, 16, 20, f, d90, 0, 0) > 0.9f, "");
        Pa::Dab flat{0, 0, 1.f};
        flat.round = 0.25f;
        check("the dab's roundness flattens a round tip", Pa::tip_alpha(0, 16, 20, hard(40), flat, 0, 0) == 0.f &&
                                                          Pa::tip_alpha(16, 0, 20, hard(40), flat, 0, 0) > 0.9f, "");
    }
    {
        // Scatter across a horizontal stroke: x stays on the dab, y spreads up to
        // scatter x the diameter, both sides.
        Pa::Brush b = hard(20);
        b.dyn.scatter = 2;   // 200 %: up to 40 px off the path
        Pa::Jitter jit;
        const auto in = line(b, 0, 100, 400, 100);
        const auto out = jit.apply(in, b);
        bool on_x = out.size() == in.size(), within = true;
        double ymin = 1e9, ymax = -1e9;
        for (std::size_t i = 0; i < out.size() && i < in.size(); ++i) {
            on_x = on_x && std::abs(out[i].x - in[i].x) < 1e-9;
            within = within && std::abs(out[i].y - 100) <= 40 + 1e-9;
            ymin = std::min(ymin, out[i].y);
            ymax = std::max(ymax, out[i].y);
        }
        check("scatter: across the path only (x kept)", on_x, "");
        check("scatter: within scatter x the diameter", within, num(ymin) + " .. " + num(ymax));
        check("scatter: both sides of the path", ymin < 90 && ymax > 110, num(ymin) + " .. " + num(ymax));
        // Down a vertical stroke the spread turns with it: x spreads, y stays.
        Pa::Jitter j2;
        const auto v_in = line(b, 200, 0, 200, 400);
        const auto v = j2.apply(v_in, b);
        bool y_kept = v.size() == v_in.size();
        double xmin = 1e9, xmax = -1e9;
        for (std::size_t i = 0; i < v.size() && i < v_in.size(); ++i) {
            y_kept = y_kept && std::abs(v[i].y - v_in[i].y) < 1e-9;
            xmin = std::min(xmin, v[i].x);
            xmax = std::max(xmax, v[i].x);
        }
        check("scatter: follows the path's direction", y_kept && xmin < 190 && xmax > 210, num(xmin) + " .. " + num(xmax));
        b.dyn.both_axes = true;
        Pa::Jitter j3;
        const auto w = j3.apply(in, b);
        bool moved_x = false;
        for (std::size_t i = 0; i < w.size(); ++i) moved_x = moved_x || std::abs(w[i].x - in[i].x) > 1;
        check("scatter both axes: along the path too", moved_x, "");
    }
    {
        // Count: n dabs at each step; count jitter: between 1 and n.
        Pa::Brush b = hard(20);
        b.dyn.count = 4;
        Pa::Jitter jit;
        const auto in = line(b, 0, 0, 200, 0);
        check("count 4: four dabs a step", jit.apply(in, b).size() == in.size() * 4, "");
        b.dyn.count_jitter = 1;
        Pa::Jitter j2;
        const auto out = j2.apply(in, b);
        check("count jitter: between one and four a step", out.size() >= in.size() && out.size() < in.size() * 4, num(static_cast<double>(out.size())));
    }
    {
        // A scattered stroke paints off the path; a plain one doesn't.
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        Pa::Brush b = hard(10);
        b.colour = P::rgba(0, 0, 1, 1);
        b.dyn.scatter = 3;
        b.dyn.count = 3;
        check("scattered stroke: a step", Pa::stroke_pen(j, d, "L5", {{100, 300, 1.f}, {500, 300, 1.f}}, b), "");
        const auto& px = *d.get("L5")->pixels;
        int off = 0;
        for (int x = 100; x < 500; ++x)
            for (int y = 270; y < 330; ++y)
                if (std::abs(y - 300) > 8 && px.get(x, y).c[3] > 0.5f && px.get(x, y).c[2] > 0.9f) ++off;
        check("scattered stroke: blue lands off the path", off > 50, num(off));
        check("scattered stroke: one undo takes it all", (j.undo(), d.get("L5")->pixels == nullptr || d.get("L5")->pixels->get(300, 300).c[2] < 0.9f), "");
    }
    {
        // Size jitter in a real coverage: a big jitter leaves the stroke thinner in places.
        Pa::Brush b = hard(40);
        b.dyn.size_jitter = 0.9;
        b.dyn.count = 1;
        Pa::Jitter jit;
        auto t = grid_for(600, 200);
        const auto cov = Pa::add_pen_dabs(Pa::new_coverage(600, 200), b, jit.apply(line(b, 50, 100, 550, 100), b), t);
        std::set<int> widths;
        for (int x = 100; x < 500; x += 7) {
            int n = 0;
            for (int y = 0; y < 200; ++y) n += cov.get(x, y).c[0] > 0.5f;
            widths.insert(n);
        }
        check("size jitter in coverage: the stroke's width varies", widths.size() > 3 && *widths.rbegin() <= 41, num(static_cast<double>(widths.size())));
    }

    std::cout << "\n-- brush dynamics: colour (s028 m2) --\n";
    {
        float c[3] = {1, 0, 0};
        Pa::jitter_hsb(c, 0.5, 0, 0);
        check("hsb: half a turn takes red to cyan", c[0] < 1e-4f && c[1] > 0.999f && c[2] > 0.999f, num(c[0]) + "," + num(c[1]) + "," + num(c[2]));
        float g[3] = {1, 0, 0};
        Pa::jitter_hsb(g, 0, -1, 0);
        check("hsb: no saturation, grey", g[0] == g[1] && g[1] == g[2], "");
        float k[3] = {0.3f, 0.6f, 0.9f};
        Pa::jitter_hsb(k, 0, 0, -1);
        check("hsb: no brightness, black", k[0] == 0 && k[1] == 0 && k[2] == 0, "");
        float same[3] = {0.3f, 0.6f, 0.9f};
        Pa::jitter_hsb(same, 0, 0, 0);
        check("hsb: nothing moved, the same colour", std::abs(same[0] - 0.3f) < 1e-5f && std::abs(same[1] - 0.6f) < 1e-5f && std::abs(same[2] - 0.9f) < 1e-5f, "");
    }
    const float red[3] = {1, 0, 0}, blue[3] = {0, 0, 1};
    const P::Format rgb{P::Depth::U8, P::Mode::RGB, true};
    {
        // Foreground / background jitter: every dab on the line from red to blue.
        Pa::Brush b = hard(20);
        b.dyn.fg_bg = 1;
        Pa::Jitter jit;
        jit.colours(rgb, red, blue);
        const auto out = jit.apply(line(b, 0, 0, 400, 0), b);
        bool tinted = true, on_line = true;
        std::set<int> kinds;
        for (const auto& d : out) {
            tinted = tinted && d.tinted;
            on_line = on_line && std::abs(d.colour.c[0] + d.colour.c[2] - 1) < 1e-4f && d.colour.c[1] == 0;
            kinds.insert(static_cast<int>(d.colour.c[0] * 20));
        }
        check("fg/bg jitter: every dab its own colour", tinted, "");
        check("fg/bg jitter: between the foreground and the background", on_line, "");
        check("fg/bg jitter: the colours vary", kinds.size() > 5, num(static_cast<double>(kinds.size())));
        // Per stroke: one colour for all.
        b.dyn.per_tip = false;
        Pa::Jitter j2;
        j2.colours(rgb, red, blue);
        const auto one = j2.apply(line(b, 0, 0, 400, 0), b);
        const auto more = j2.apply(line(b, 400, 0, 800, 0), b);
        bool all_same = true;
        for (const auto& d : one) all_same = all_same && d.colour == one[0].colour;
        for (const auto& d : more) all_same = all_same && d.colour == one[0].colour;
        check("per stroke: one colour for the whole stroke", all_same, "");
        // No colour jitter (shape only): no dab tinted, even with colours given.
        Pa::Brush s = hard(20);
        s.dyn.size_jitter = 0.5;
        Pa::Jitter j3;
        j3.colours(rgb, red, blue);
        bool none = true;
        for (const auto& d : j3.apply(line(s, 0, 0, 200, 0), s)) none = none && !d.tinted;
        check("no colour jitter: no dab tinted", none, "");
    }
    {
        // The tint path is neutral: untinted dabs through a tint paint exactly as without one.
        Pa::Brush b = hard(30);
        b.hardness = 0.5;
        b.flow = 0.4;
        b.colour = P::rgba(0.2f, 0.7f, 0.1f, 1);
        const P::Format f{P::Depth::F32, P::Mode::RGB, true};
        const P::TileMap orig(f, 300, 200);
        auto t1 = grid_for(300, 200), t2 = grid_for(300, 200);
        const auto dabs = line(b, 20, 100, 280, 120);
        const auto cov1 = Pa::add_pen_dabs(Pa::new_coverage(300, 200), b, dabs, t1);
        auto tint = Pa::new_tint(f, 300, 200);
        const auto cov2 = Pa::add_pen_dabs(Pa::new_coverage(300, 200), b, dabs, t2, &tint);
        const auto a = Pa::apply(orig, orig, cov1, nullptr, b, t1);
        const auto c = Pa::apply(orig, orig, cov2, nullptr, b, t2, &tint);
        bool same = true;
        for (int y = 0; y < 200 && same; y += 3)
            for (int x = 0; x < 300 && same; x += 3) {
                const auto p = a.get(x, y), q = c.get(x, y);
                for (int k = 0; k < 4; ++k) same = same && std::abs(p.c[static_cast<std::size_t>(k)] - q.c[static_cast<std::size_t>(k)]) < 1e-5f;
            }
        check("tint: untinted dabs paint exactly as before", same, "");
    }
    {
        // Later dabs paint on top: a blue dab over a red one, both full -- blue.
        Pa::Brush b = hard(30);
        b.colour = P::rgba(1, 0, 0, 1);
        const P::Format f{P::Depth::F32, P::Mode::RGB, true};
        const P::TileMap orig(f, 200, 200);
        auto t = grid_for(200, 200);
        auto tint = Pa::new_tint(f, 200, 200);
        Pa::Dab d1{100, 100}, d2{100, 100};
        d1.tinted = d2.tinted = true;
        d1.colour = P::rgba(1, 0, 0, 1);
        d2.colour = P::rgba(0, 0, 1, 1);
        const auto cov = Pa::add_pen_dabs(Pa::new_coverage(200, 200), b, {d1, d2}, t, &tint);
        const auto out = Pa::apply(orig, orig, cov, nullptr, b, t, &tint);
        const auto p = out.get(100, 100);
        check("tint: a later dab lays its colour on top", p.c[2] > 0.99f && p.c[0] < 0.01f && p.c[3] > 0.99f, num(p.c[0]) + "," + num(p.c[2]));
        // Half a dab over a full one: half-way between.
        auto t2 = grid_for(200, 200);
        auto tint2 = Pa::new_tint(f, 200, 200);
        Pa::Brush h = b;
        h.flow = 0.5;
        const auto cov2 = Pa::add_pen_dabs(Pa::new_coverage(200, 200), b, {d1}, t2, &tint2);
        const auto cov3 = Pa::add_pen_dabs(cov2, h, {d2}, t2, &tint2);
        const auto out2 = Pa::apply(orig, orig, cov3, nullptr, b, t2, &tint2);
        const auto q = out2.get(100, 100);
        check("tint: a half-strength dab over a full one, half-way", std::abs(q.c[0] - 0.5f) < 0.02f && std::abs(q.c[2] - 0.5f) < 0.02f, num(q.c[0]) + "," + num(q.c[2]));
    }
    {
        // Through the verb: a stroke of many colours, ONE step.
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        Pa::Brush b = hard(16);
        b.colour = P::rgba(1, 0, 0, 1);
        b.dyn.fg_bg = 1;
        Pa::Jitter jit(99);
        jit.colours(d.image().format, red, blue);
        check("colour stroke: a step", Pa::stroke_pen(j, d, "L5", {{60, 330, 1.f}, {560, 330, 1.f}}, b, jit), "");
        const auto& px = *d.get("L5")->pixels;
        std::set<int> reds;
        bool on_line = true;
        for (int x = 70; x < 550; x += 5) {
            const auto p = px.get(x, 330);
            if (p.c[3] < 0.99f) continue;
            reds.insert(static_cast<int>(p.c[0] * 10));
            on_line = on_line && std::abs(p.c[0] + p.c[2] - 1) < 0.02f && p.c[1] < 0.02f;
        }
        check("colour stroke: many colours along it", reds.size() >= 4, num(static_cast<double>(reds.size())));
        check("colour stroke: each between red and blue", on_line, "");
        j.undo();
        check("colour stroke: one undo", !d.get("L5")->pixels || d.get("L5")->pixels->get(300, 330).c[3] < 0.5f, "");
        // The Eraser ignores colour dynamics (no tint): it erases.
        Pa::Brush e = hard(16);
        e.erase = true;
        e.dyn.fg_bg = 1;
        e.dyn.hue = 1;
        Pa::Jitter je;
        je.colours(d.image().format, red, blue);
        check("eraser with colour dynamics: still erases", Pa::stroke_pen(j, d, "L4", {{120, 175, 1.f}, {280, 175, 1.f}}, e, je) &&
                                                             d.get("L4")->pixels->get(200, 175).c[3] < 0.01f, "");
    }
    {
        // A Gray document: brightness jitter makes greys, the mode kept.
        Pa::Brush b = hard(20);
        b.dyn.bri = 1;
        const P::Format gray{P::Depth::U8, P::Mode::Gray, true};
        const float mid[3] = {0.5f, 0.5f, 0.5f};
        Pa::Jitter jit;
        jit.colours(gray, mid, mid);
        std::set<int> levels;
        bool in_range = true;
        for (const auto& dd : jit.apply(line(b, 0, 0, 400, 0), b)) {
            levels.insert(static_cast<int>(dd.colour.c[0] * 20));
            in_range = in_range && dd.colour.c[0] >= 0 && dd.colour.c[0] <= 1;
        }
        check("gray: brightness jitter varies the grey", levels.size() > 4 && in_range, num(static_cast<double>(levels.size())));
    }
    std::cout << "\n-- brush texture (s033) --\n";
    {
        Pa::Brush b = hard(40);
        b.texture.on = true;
        b.texture.builtin = 2;   // Checker: 16 px cells, dark 0.15 / light 1
        b.texture.depth = 1;
        Pa::Dab d0{0, 0, 1.f};
        const float light = Pa::tip_alpha(0, 0, 20, b, d0, 4, 4);     // a light cell
        const float dark = Pa::tip_alpha(0, 0, 20, b, d0, 20, 4);     // a dark cell
        check("texture (multiply, full depth): the light cells take it all", near(light, 1, 1e-3), num(light));
        check("texture: the dark cells take little", near(dark, 0.15, 0.02), num(dark));
        b.texture.depth = 0.5;
        check("texture: half depth -- half-way", near(Pa::tip_alpha(0, 0, 20, b, d0, 20, 4), 0.575, 0.02), num(Pa::tip_alpha(0, 0, 20, b, d0, 20, 4)));
        b.texture.subtract = true;
        b.texture.depth = 1;
        check("texture (subtract): the dark cells lose their darkness", near(Pa::tip_alpha(0, 0, 20, b, d0, 20, 4), 0.15, 0.02), "");
        b.texture.subtract = false;
        b.texture.invert = true;
        check("texture inverted: the dark cells take it now", Pa::tip_alpha(0, 0, 20, b, d0, 20, 4) > 0.8f && Pa::tip_alpha(0, 0, 20, b, d0, 4, 4) < 0.05f, "");
        b.texture.invert = false;
        // Yours: half red (opaque), half clear -- clear counts as white.
        const P::Format pf{P::Depth::U8, P::Mode::RGB, true};
        b.texture.image = std::make_shared<const P::TileMap>(P::TileMap(pf, 4, 4).fill({0, 0, 2, 4}, P::rgba(0, 0, 0, 1)));
        check("texture (yours): black takes nothing, clear takes all", Pa::tip_alpha(0, 0, 20, b, d0, 0, 0) < 0.01f && near(Pa::tip_alpha(0, 0, 20, b, d0, 2, 0), 1, 1e-3), "");
        b.texture.on = false;
        check("texture off: the dab as before", near(Pa::tip_alpha(0, 0, 20, b, d0, 0, 0), 1, 1e-6), "");
        // Fixed to the image: two dabs at different places share the grain.
        b.texture.on = true;
        b.texture.image = nullptr;
        check("texture: fixed to the image, not the dab", near(Pa::tip_alpha(3, 3, 20, b, Pa::Dab{17, 1, 1.f}, 20, 4), Pa::tip_alpha(-7, 1, 20, b, Pa::Dab{27, 3, 1.f}, 20, 4), 1e-4), "");
    }
}
