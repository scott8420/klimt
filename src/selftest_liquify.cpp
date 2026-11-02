// klimt_selftest -- Filter > Liquify (s076): the field's brushes, the
// resample through it, the verb.
#include "core/Document.hpp"
#include "core/Liquify.hpp"
#include "core/Filter.hpp"   // s077: Lens Correction
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

    // ── s077: Lens Correction (a Distort filter) ───────────────────────────
    std::cout << "\n-- lens correction (s077) --\n";
    namespace F = klimt::core::filter;
    using klimt::core::FilterKind;
    {
        const auto f = F::fresh(FilterKind::LensCorrection);
        check("lens: under Distort, named", F::group(FilterKind::LensCorrection) == "Distort" && F::label(FilterKind::LensCorrection) == "Lens Correction", "");
        auto big = f; big.amount = 300; big.vignette = -400; big.vignette_mid = 120; big.fringe_rc = 999; big.lens_scale = 10;
        const auto n = F::normalised(big);
        check("lens: normalised -- -100..100, Midpoint 0..100, Scale 50..150",
              n.amount == 100 && n.vignette == -100 && n.vignette_mid == 100 && n.fringe_rc == 100 && n.lens_scale == 50, "");
        auto g = F::fresh(FilterKind::GaussianBlur); g.vignette = 30; g.lens_scale = 120;
        check("lens: its settings go back on another kind", F::normalised(g).vignette == 0 && F::normalised(g).lens_scale == 100, "");
        // The fresh one changes nothing.
        P::TileMap m(P::Format{}, 400, 300);
        m = m.fill({0, 0, 400, 300}, P::rgba(0.5f, 0.5f, 0.5f, 1)).fill({120, 60, 30, 30}, P::rgba(1, 0, 0, 1));
        const auto out = F::filtered(m, f, nullptr);
        bool same = out.has_value();
        for (int y = 0; y < 300 && same; y += 7)
            for (int x = 0; x < 400 && same; x += 7) same = std::abs(out->get(x, y).c[0] - m.get(x, y).c[0]) < 1e-4f;
        check("lens: nothing asked, nothing changed", same, "");
    }
    {
        // A dark vertical line near the right edge; Remove Distortion + (barrel
        // undone) moves the edges out -- more at the corners than at the middle.
        P::TileMap m(P::Format{}, 400, 300);
        m = m.fill({0, 0, 400, 300}, P::rgba(1, 1, 1, 1)).fill({360, 0, 3, 300}, P::rgba(0, 0, 0, 1));
        auto f = F::fresh(FilterKind::LensCorrection);
        f.amount = 60;
        const auto out = F::filtered(m, f, nullptr);
        const auto line_at = [&](int y) { int best = -1; float lo = 2; for (int x = 300; x < 400; ++x) if (out->get(x, y).c[0] < lo) { lo = out->get(x, y).c[0]; best = x; } return best; };
        const int mid = line_at(150), top = line_at(5);
        check("lens: Remove Distortion + moves the edge out", mid > 362, std::to_string(mid));
        check("lens: ...more at the corners (the bow taken out)", top > mid, std::to_string(top) + " vs " + std::to_string(mid));
        f.amount = -60;
        const auto in = F::filtered(m, f, nullptr);
        int lo_x = -1; float lo = 2;
        for (int x = 300; x < 400; ++x) if (in->get(x, 150).c[0] < lo) { lo = in->get(x, 150).c[0]; lo_x = x; }
        check("lens: Remove Distortion - moves it in", lo_x < 361, std::to_string(lo_x));
    }
    {
        // Scale 125: the middle stays, the rest grows from it.
        P::TileMap m(P::Format{}, 400, 300);
        m = m.fill({0, 0, 400, 300}, P::rgba(1, 1, 1, 1)).fill({190, 140, 20, 20}, P::rgba(0, 0, 0, 1)).fill({290, 145, 4, 10}, P::rgba(0, 0, 0, 1));
        auto f = F::fresh(FilterKind::LensCorrection);
        f.lens_scale = 125;
        const auto out = F::filtered(m, f, nullptr);
        check("lens: Scale -- the middle stays", out->get(200, 150).c[0] < 0.05f, "");
        check("lens: Scale -- the rest grows from it", out->get(316, 150).c[0] < 0.3f && out->get(292, 150).c[0] > 0.9f, "");
    }
    {
        // Vignette: - darkens the corners, + lightens them, the middle kept.
        P::TileMap m(P::Format{}, 400, 300);
        m = m.fill({0, 0, 400, 300}, P::rgba(0.5f, 0.5f, 0.5f, 1));
        auto f = F::fresh(FilterKind::LensCorrection);
        f.vignette = -100;
        const auto dk = F::filtered(m, f, nullptr);
        f.vignette = 100;
        const auto lt = F::filtered(m, f, nullptr);
        check("lens: Vignette - darkens the corners", dk->get(1, 1).c[0] < 0.1f && std::abs(dk->get(200, 150).c[0] - 0.5f) < 0.01f, std::to_string(dk->get(1, 1).c[0]));
        check("lens: Vignette + lightens them", lt->get(398, 298).c[0] > 0.9f && std::abs(lt->get(200, 150).c[0] - 0.5f) < 0.01f, "");
        f.vignette = -100; f.vignette_mid = 0;
        const auto wide = F::filtered(m, f, nullptr);
        check("lens: a smaller Midpoint reaches further in", wide->get(100, 75).c[0] < dk->get(100, 75).c[0] - 0.02f, "");
    }
    {
        // A fringe moves one channel only: red off the white line, green kept.
        P::TileMap m(P::Format{}, 400, 300);
        m = m.fill({0, 0, 400, 300}, P::rgba(0, 0, 0, 1)).fill({370, 0, 2, 300}, P::rgba(1, 1, 1, 1));
        auto f = F::fresh(FilterKind::LensCorrection);
        f.fringe_rc = 100;
        const auto out = F::filtered(m, f, nullptr);
        bool split = false, green_kept = true;
        for (int x = 360; x < 380; ++x) {
            const auto p = out->get(x, 150), o = m.get(x, 150);
            if (std::abs(p.c[0] - p.c[1]) > 0.3f) split = true;
            green_kept = green_kept && std::abs(p.c[1] - o.c[1]) < 1e-4f;
        }
        check("lens: Fix Red / Cyan Fringe moves the red only", split && green_kept, "");
    }
}
