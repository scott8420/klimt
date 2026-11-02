// klimt_selftest -- Filter > Liquify (s076): the field's brushes, the
// resample through it, the verb.
#include "core/Document.hpp"
#include "core/Liquify.hpp"
#include "core/Text.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::liquify;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
struct Fix {
    core::Document d;
    core::DocJournal j{d};
    Fix() {
        d.set_door([this] { return j.open(); });
        core::layers::load_fixture(d);
        // "A" (L4): the red square (100, 100) 200 x 150 with a blue stripe down its left 10 px.
        core::Document::Loading load(d);
        auto l = *d.get("L4");
        auto m = l.pixels->fill({100, 100, 10, 150}, P::rgba(0, 0, 1, 1));
        l.pixels = std::make_shared<const P::TileMap>(std::move(m));
        d.put("L4", l);
    }
    P::Px at(const std::string& id, int x, int y) const {
        const auto l = d.get(id);
        return l && l->pixels ? l->pixels->get(x, y) : P::transparent();
    }
};
bool red(const P::Px& p) { return p.c[0] > 0.95f && p.c[1] < 0.05f && p.c[2] < 0.05f && p.c[3] > 0.99f; }
bool blue(const P::Px& p) { return p.c[2] > 0.95f && p.c[0] < 0.05f && p.c[3] > 0.99f; }
bool clear(const P::Px& p) { return p.c[3] <= 0.f; }
}  // namespace

void selftest_liquify(const Check& check) {
    std::cout << "\n-- liquify: the field (s076) --\n";
    {
        L::Field f(600, 400);
        check("liquify: a new field is the identity", f.valid() && f.identity() && f.touched().empty(), "");
        L::Brush b{L::Tool::Forward, 40, 1, 0.5};
        f.dab(b, {200, 200}, {10, 0});
        f.dab(b, {210, 200}, {10, 0});
        const auto d = f.at(208, 200);
        check("liquify: two pushes of 10 make about 20", d.x < -17 && d.x > -21 && std::abs(d.y) < 0.5, std::to_string(d.x));
        check("liquify: beyond the brush nothing moves", std::abs(f.at(300, 200).x) < 1e-6 && std::abs(f.at(208, 260).x) < 1e-6, "");
        const auto t = f.touched();
        check("liquify: what it touched holds the brushes", t.x <= 160 && t.x + t.w >= 250 && t.y <= 160 && t.y + t.h >= 240, "");
        L::Brush rb{L::Tool::Reconstruct, 80, 1, 1};
        for (int i = 0; i < 3; ++i) rb.radius = 80, f.dab(rb, {205, 200});
        check("liquify: Reconstruct brings it back", f.identity(), std::to_string(f.at(208, 200).x));
    }
    {
        L::Field f(600, 400);
        f.dab({L::Tool::Twirl, 60, 1, 1}, {300, 200});
        const auto d = f.at(330, 200);   // to the right of the middle: it turns -- moves up or down, not in or out
        check("liquify: Twirl turns", std::abs(d.y) > 0.5 && std::abs(d.y) > 4 * std::abs(d.x), std::to_string(d.x) + "," + std::to_string(d.y));
        L::Field g(600, 400);
        g.dab({L::Tool::Twirl, 60, 1, 1}, {300, 200}, {0, 0}, true);
        check("liquify: Alt turns the other way", g.at(330, 200).y * d.y < 0, "");
        L::Field pu(600, 400), bl(600, 400), alt(600, 400);
        pu.dab({L::Tool::Pucker, 60, 1, 1}, {300, 200});
        bl.dab({L::Tool::Bloat, 60, 1, 1}, {300, 200});
        alt.dab({L::Tool::Pucker, 60, 1, 1}, {300, 200}, {0, 0}, true);
        check("liquify: Pucker looks outward (the picture draws in)", pu.at(320, 200).x > 0.1, std::to_string(pu.at(320, 200).x));
        check("liquify: Bloat looks inward (the picture swells)", bl.at(320, 200).x < -0.1, "");
        check("liquify: Alt with Pucker bloats", std::abs(alt.at(320, 200).x - bl.at(320, 200).x) < 1e-6, "");
        check("liquify: the middle stays put", std::abs(pu.at(300, 200).x) < 1e-6 && std::abs(pu.at(300, 200).y) < 1e-6, "");
    }
    std::cout << "\n-- liquify: the pixels (s076) --\n";
    {
        // Push the blue stripe 20 px right at the square's middle height.
        Fix t;
        L::Field f(600, 400);
        f.dab({L::Tool::Forward, 40, 1, 0.5}, {105, 175}, {20, 0});
        check("liquify: the verb", L::liquify(t.j, t.d, "L4", f), "");
        check("liquify: one step, named", t.j.size() == 1 && t.j.undo_label() == "Liquify", t.j.undo_label());
        check("liquify: the stripe pushed right", blue(t.at("L4", 120, 175)) && !blue(t.at("L4", 103, 175)), "");
        check("liquify: beyond the brush, as it was", blue(t.at("L4", 105, 120)) && red(t.at("L4", 200, 175)) && clear(t.at("L4", 50, 50)), "");
        check("liquify: the document holds", t.d.check().empty(), t.d.check());
        t.j.undo();
        check("liquify: undo", blue(t.at("L4", 103, 175)) && red(t.at("L4", 120, 175)), "");
    }
    {
        // Half-transparent edges stay half: nothing is laid over itself.
        Fix t;
        {
            core::Document::Loading load(t.d);
            auto l = *t.d.get("L4");
            auto m = l.pixels->fill({400, 300, 40, 40}, P::rgba(0, 1, 0, 0.5f));
            l.pixels = std::make_shared<const P::TileMap>(std::move(m));
            t.d.put("L4", l);
        }
        L::Field f(600, 400);
        f.dab({L::Tool::Forward, 20, 1, 0.5}, {200, 175}, {5, 0});   // somewhere else
        f.dab({L::Tool::Forward, 30, 1, 0.5}, {380, 320}, {1, 0});   // its edge barely moves
        L::liquify(t.j, t.d, "L4", f);
        const auto p = t.at("L4", 430, 330);
        check("liquify: half-transparent stays half", std::abs(p.c[3] - 0.5f) < 0.02f, std::to_string(p.c[3]));
    }
    {
        // An opaque Background pushed in from its edge stays opaque: the edge stretches.
        Fix t;
        L::Field f(600, 400);
        f.dab({L::Tool::Forward, 40, 1, 0.5}, {0, 200}, {30, 0});
        check("liquify: the Background", L::liquify(t.j, t.d, "L1", f), "");
        check("liquify: its edge never empties", t.at("L1", 1, 200).c[3] > 0.99f && t.at("L1", 15, 200).c[3] > 0.99f, "");
    }
    {
        Fix t;
        L::Field f(600, 400);
        check("liquify: nothing brushed, no step", !L::liquify(t.j, t.d, "L4", f) && t.j.size() == 0, "");
        klimt::text::Spec sp;
        sp.text = "Klimt"; sp.x = 60; sp.y = 120; sp.size = 40;
        const auto id = core::layers::add_text(t.j, t.d, sp, "L4");
        check("liquify: type refused", !L::refusal(t.d, id).empty(), L::refusal(t.d, id));
        core::layers::set_locks(t.j, t.d, "L4", core::lock::Pixels);
        check("liquify: a Pixels lock refused", !L::refusal(t.d, "L4").empty(), "");
        check("liquify: an empty layer refused", !L::refusal(t.d, "L5").empty(), "");
    }
}
