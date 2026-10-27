// klimt_selftest -- the pen (s009 m3): pressure drives a dab's size and / or
// its strength; a mouse (pressure 1) paints exactly as before.
#include "core/Document.hpp"
#include "core/Paint.hpp"

#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace Pa = klimt::core::paint;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
struct Fix {
    core::Document d;
    core::DocJournal j{d};
    Fix() {
        d.set_door([this] { return j.open(); });
        core::layers::load_fixture(d);
    }
    float alpha(const std::string& id, int x, int y) const {
        const auto l = d.get(id);
        return l && l->pixels ? l->pixels->get(x, y).c[3] : 0.f;
    }
    // How far up from the stroke's line (y = 200) paint reaches at column x.
    int reach(int x) const {
        int r = 0;
        while (r < 100 && alpha("L5", x, 200 - r) > 0.5f) ++r;
        return r;
    }
};
}  // namespace

void selftest_pen(const Check& check) {
    std::cout << "\n-- pen: pressure (s009) --\n";
    {
        Pa::Brush b;
        b.size = 40;
        check("pen: full pressure is the brush's size", Pa::dab_radius(b, 1.f) == 20, "");
        check("pen: half pressure, half the size", Pa::dab_radius(b, 0.5f) == 10, "");
        check("pen: no pressure still leaves a mark", Pa::dab_radius(b, 0.f) >= 0.5 && Pa::dab_radius(b, 0.f) <= 20 * Pa::kMinPressure + 1e-4, "");
        b.pressure_size = false;
        check("pen: size off -- pressure leaves the size", Pa::dab_radius(b, 0.2f) == 20, "");
        check("pen: opacity off -- full strength", Pa::dab_strength(b, 0.3f) == 1.f, "");
        b.pressure_opacity = true;
        check("pen: opacity on -- the strength is the pressure", Pa::dab_strength(b, 0.3f) == 0.3f, "");
        double carry = 1e9;
        const auto dabs = Pa::dabs_along(Pa::Dab{0, 0, 0.f}, Pa::Dab{100, 0, 1.f}, b, carry);
        check("pen: pressure is interpolated along a segment",
              dabs.size() > 2 && dabs.front().p == 0.f && dabs[dabs.size() / 2].p > 0.3f && dabs[dabs.size() / 2].p < 0.7f, "");
    }
    {
        // A stroke from light (left) to heavy (right): thin to thick.
        Fix t;
        Pa::Brush b;
        b.size = 60;
        b.hardness = 1;
        b.colour = P::rgba(0, 0, 1, 1);
        check("pen: a pressure stroke", Pa::stroke_pen(t.j, t.d, "L5", {{100, 200, 0.2f}, {500, 200, 1.f}}, b), "");
        const int thin = t.reach(120), thick = t.reach(480);
        check("pen: light is thin, heavy is thick", thin < 12 && thick > 24, std::to_string(thin) + " / " + std::to_string(thick));
        check("pen: still ONE step", t.j.size() == 1, std::to_string(t.j.size()));
    }
    {
        // Opacity from pressure, size fixed: a light stroke is faint.
        Fix t;
        Pa::Brush b;
        b.size = 30;
        b.hardness = 1;
        b.pressure_size = false;
        b.pressure_opacity = true;
        b.colour = P::rgba(0, 0, 1, 1);
        Pa::stroke_pen(t.j, t.d, "L5", {{100, 200, 0.25f}, {500, 200, 0.25f}}, b);
        const float a = t.alpha("L5", 300, 200);
        check("pen: a light touch is faint", a > 0.2f && a < 0.3f, std::to_string(a));
    }
    {
        // A mouse (pressure 1 everywhere) paints the same either way.
        Fix a, m;
        Pa::Brush b;
        b.size = 40;
        b.colour = P::rgba(1, 0, 0, 1);
        Pa::stroke(a.j, a.d, "L5", {{50, 50}, {300, 120}}, b);
        b.pressure_opacity = true;
        Pa::stroke_pen(m.j, m.d, "L5", {{50, 50, 1.f}, {300, 120, 1.f}}, b);
        bool same = true;
        for (int y = 0; y < 200 && same; y += 3)
            for (int x = 0; x < 360 && same; x += 3) same = a.d.get("L5")->pixels->get(x, y) == m.d.get("L5")->pixels->get(x, y);
        check("pen: a mouse paints as before", same, "");
    }

    std::cout << "\n-- pen: smoothing and flow (s012) --\n";
    {
        Pa::Smoother sm(10);
        const auto a = sm.push({0, 0, 1.f});
        check("smooth: the first point is the brush's", a && a->x == 0 && a->y == 0, "");
        check("smooth: a wobble inside the string doesn't move it", !sm.push({6, 3, 1.f}) && !sm.push({-4, 5, 1.f}), "");
        const auto b = sm.push({30, 0, 0.5f});
        check("smooth: past the string, it follows 10 behind", b && std::abs(b->x - 20) < 1e-9 && std::abs(b->y) < 1e-9 && b->p == 0.5f,
              b ? std::to_string(b->x) : "");
        const auto c = sm.finish();
        check("smooth: at the end it catches up", c && std::abs(c->x - 30) < 1e-9, "");
        check("smooth: nothing more to catch up", !sm.finish(), "");
        Pa::Smoother none;
        check("smooth: off, every point is the brush's", none.push({0, 0, 1.f}) && none.push({0.5, 0, 1.f}), "");
    }
    {
        // Flow: back and forth over the same line builds up; at 100% it doesn't.
        const auto paint = [](double flow) {
            Fix t;
            Pa::Brush b;
            b.size = 30;
            b.hardness = 1;
            b.flow = flow;
            b.colour = P::rgba(0, 0, 1, 1);
            Pa::stroke_pen(t.j, t.d, "L5", {{100, 200, 1.f}, {300, 200, 1.f}, {100, 200, 1.f}, {300, 200, 1.f}}, b);
            return t.alpha("L5", 200, 200);
        };
        const float full = paint(1.0), low = paint(0.2);
        check("flow: 100% -- one pass is all of it", full > 0.99f, std::to_string(full));
        check("flow: 20% -- passes build up, but not to full yet", low > 0.25f && low < 0.99f, std::to_string(low));
        Fix t;
        Pa::Brush b;
        b.size = 30;
        b.hardness = 1;
        b.flow = 0.2;
        b.opacity = 0.5;
        b.colour = P::rgba(0, 0, 1, 1);
        std::vector<Pa::Dab> zig;
        for (int i = 0; i < 20; ++i) zig.push_back({i % 2 ? 300.0 : 100.0, 200, 1.f});
        Pa::stroke_pen(t.j, t.d, "L5", zig, b);
        const float capped = t.alpha("L5", 200, 200);
        check("flow: build-up stops at the opacity", capped > 0.45f && capped <= 0.5f + 1e-3, std::to_string(capped));
    }
}
