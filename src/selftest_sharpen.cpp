// klimt_selftest -- SMART SHARPEN, DUST & SCRATCHES, REDUCE NOISE (s034).
// docs/filters.md. Rules, not looks: an edge gets steeper, a flat stays flat,
// noise below the threshold isn't sharpened, the shadows fade, Dust & Scratches
// at threshold 0 is the Median, Reduce Noise flattens noise and keeps an edge,
// the preview's rectangle == the result, the settings survive a .klimt.
#include "core/Commands.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Filter.hpp"
#include "core/Live.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

#include <unistd.h>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace F = klimt::core::filter;
using Check = std::function<void(const std::string&, bool, const std::string&)>;
using core::FilterKind;
using core::Remove;

namespace {
bool near(double a, double b, double eps = 0.01) { return std::abs(a - b) <= eps; }
std::string num(double v) { return std::to_string(v); }
P::Format f32() { P::Format f; f.depth = P::Depth::F32; return f; }

// Left half `lo`, right half `hi`, opaque (a vertical edge at x = 300).
P::TileMap step(float lo = 0.25f, float hi = 0.75f, P::Format f = f32()) {
    return P::TileMap(f, 600, 400).fill({0, 0, 300, 400}, P::rgba(lo, lo, lo, 1)).fill({300, 0, 300, 400}, P::rgba(hi, hi, hi, 1));
}
// Top half dark, bottom half light (a horizontal edge at y = 200).
P::TileMap hstep() {
    return P::TileMap(f32(), 600, 400).fill({0, 0, 600, 200}, P::rgba(0.25f, 0.25f, 0.25f, 1)).fill({0, 200, 600, 200}, P::rgba(0.75f, 0.75f, 0.75f, 1));
}
bool same_in(const P::TileMap& a, const P::TileMap& b, const P::Rect& r, float eps = 0) {
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            for (int c = 0; c < 4; ++c)
                if (std::abs(a.get(x, y).c[static_cast<std::size_t>(c)] - b.get(x, y).c[static_cast<std::size_t>(c)]) > eps) return false;
    return true;
}
// Grey 0.5 with noise of about `amount` % (Add Noise, Gaussian), mono or not.
P::TileMap noisy(double amount, bool mono) {
    const auto flat = P::TileMap(f32(), 300, 300).fill({0, 0, 300, 300}, P::rgba(0.5f, 0.5f, 0.5f, 1));
    auto n = F::fresh(FilterKind::AddNoise);
    n.amount = amount; n.gaussian = true; n.mono = mono; n.seed = 11;
    return *F::filtered(flat, n, nullptr);
}
// The spread (standard deviation) of channel c, or of the chroma (r - g), over the middle.
double spread(const P::TileMap& m, int c) {
    double s = 0, s2 = 0;
    int n = 0;
    for (int y = 50; y < 250; y += 2)
        for (int x = 50; x < 250; x += 2) {
            const auto p = m.get(x, y);
            const double v = c < 0 ? p.c[0] - p.c[1] : p.c[static_cast<std::size_t>(c)];
            s += v; s2 += v * v; ++n;
        }
    const double mean = s / n;
    return std::sqrt(std::max(0.0, s2 / n - mean * mean));
}
core::Filter smart(Remove r, double amount = 100, double radius = 2) {
    auto f = F::fresh(FilterKind::SmartSharpen);
    f.remove = r; f.amount = amount; f.radius = radius; f.noise = 0;
    return f;
}
}  // namespace

void selftest_sharpen(const Check& check) {
    std::cout << "\n-- filters: Smart Sharpen (s034) --\n";
    {
        const auto d = F::fresh(FilterKind::SmartSharpen);
        check("smart: Photoshop's defaults (200 %, 1 px, noise 10 %, Lens)",
              d.amount == 200 && d.radius == 1 && d.noise == 10 && d.remove == Remove::Lens && d.shadow_tonal == 50, "");
        auto big = d;
        big.radius = 500; big.amount = 9000; big.noise = -4; big.shadow_radius = 0;
        const auto n = F::normalised(big);
        check("smart: normalised clamps (radius 64, amount 500, noise 0, radius 1)",
              n.radius == 64 && n.amount == 500 && n.noise == 0 && n.shadow_radius == 1, num(n.radius));
        auto g = F::fresh(FilterKind::GaussianBlur);
        g.noise = 50; g.remove = Remove::Motion; g.preserve = 3;
        check("smart: other kinds drop its settings (equality stays meaningful)", F::normalised(g) == F::normalised(F::fresh(FilterKind::GaussianBlur)), "");
        auto m = smart(Remove::Gaussian);
        m.angle = 30;
        check("smart: the angle only for Motion", F::normalised(m).angle == 0, "");
        m.remove = Remove::Motion;
        check("smart: Motion keeps its angle", F::normalised(m).angle == 30, "");
        check("smart: Lens reaches its radius", F::reach(smart(Remove::Lens, 100, 5)) == 5, num(F::reach(smart(Remove::Lens, 100, 5))));
        auto t = smart(Remove::Gaussian, 100, 1);
        t.shadow_fade = 50; t.shadow_radius = 30;
        check("smart: a fade reaches its tone radius", F::reach(t) >= 30 * 2, num(F::reach(t)));
        check("smart, dust, reduce: in the menus", F::group(FilterKind::SmartSharpen) == "Sharpen" &&
                                                   F::group(FilterKind::DustScratches) == "Noise" && F::group(FilterKind::ReduceNoise) == "Noise", "");
        check("smart: remove keys round-trip", F::remove_from(F::remove_key(Remove::Lens)) == Remove::Lens &&
                                               F::remove_from(F::remove_key(Remove::Motion)) == Remove::Motion && !F::remove_from("x"), "");
    }
    {
        const auto src = step();
        for (const Remove r : {Remove::Gaussian, Remove::Lens, Remove::Motion}) {
            const std::string w = r == Remove::Gaussian ? "gaussian" : r == Remove::Lens ? "lens" : "motion";
            const auto out = F::filtered(src, smart(r, 100, 3), nullptr);
            check("smart " + w + ": the light side brightens at the edge", out->get(301, 50).c[0] > 0.76f, num(out->get(301, 50).c[0]));
            check("smart " + w + ": the dark side darkens at the edge", out->get(298, 50).c[0] < 0.24f, num(out->get(298, 50).c[0]));
            check("smart " + w + ": flat areas stay", near(out->get(100, 50).c[0], 0.25, 1e-5) && near(out->get(500, 50).c[0], 0.75, 1e-5), "");
            check("smart " + w + ": alpha stays", out->get(301, 50).c[3] == 1.f, "");
            const P::Rect view{270, 30, 60, 50};
            const auto part = F::filtered(src, smart(r, 100, 3), nullptr, false, &view);
            check("smart " + w + ": the preview's rectangle == the result there", part && same_in(*out, *part, view), "");
        }
        auto usm = F::fresh(FilterKind::UnsharpMask);
        usm.amount = 150; usm.radius = 2;
        check("smart Gaussian, no noise reduction == Unsharp Mask",
              same_in(*F::filtered(src, usm, nullptr), *F::filtered(src, smart(Remove::Gaussian, 150, 2), nullptr), {280, 40, 40, 20}, 1e-5f), "");
        // Motion at 0 degrees undoes a horizontal streak: a horizontal edge has none.
        const auto hs = hstep();
        const auto mo = F::filtered(hs, smart(Remove::Motion, 200, 4), nullptr);
        check("smart motion 0 deg: a horizontal edge is left alone", same_in(hs, *mo, {100, 190, 50, 20}, 1e-6f), "");
        auto up = smart(Remove::Motion, 200, 4);
        up.angle = 90;
        const auto mv = F::filtered(hs, up, nullptr);
        check("smart motion 90 deg: sharpens it", mv->get(100, 200).c[0] > 0.76f && mv->get(100, 198).c[0] < 0.24f, num(mv->get(100, 200).c[0]));
        // Transparent pixels stay transparent.
        const auto sq = P::TileMap(f32(), 400, 300).fill({100, 100, 150, 100}, P::rgba(1, 0, 0, 1));
        const auto so = F::filtered(sq, F::fresh(FilterKind::SmartSharpen), nullptr);
        check("smart: transparent stays transparent", so->get(50, 50).c[3] == 0.f && so->get(98, 150).c[3] == 0.f, "");
        // 8-bit too.
        const auto s8 = F::filtered(step(0.25f, 0.75f, P::Format{}), smart(Remove::Lens, 100, 3), nullptr);
        check("smart 8-bit: sharpens, clamped", s8->get(301, 50).c[0] > 0.76f && s8->get(301, 50).c[0] <= 1.f, num(s8->get(301, 50).c[0]));
    }
    {
        // Reduce Noise: fine noise isn't sharpened, the edge still is.
        const auto n = noisy(2, true);
        auto f = smart(Remove::Gaussian, 300, 1);
        const double raw = spread(n, 0);
        const double plain = spread(*F::filtered(n, f, nullptr), 0);
        f.noise = 100;
        const double kept = spread(*F::filtered(n, f, nullptr), 0);
        check("smart: without Reduce Noise the noise grows", plain > raw * 1.8, num(raw) + " -> " + num(plain));
        check("smart: Reduce Noise 100 % leaves fine noise nearly alone", kept < raw * 1.35, num(raw) + " -> " + num(kept));
        const auto edge = F::filtered(step(), f, nullptr);
        check("smart: ... and still sharpens an edge", edge->get(301, 50).c[0] > 0.76f, num(edge->get(301, 50).c[0]));
    }
    {
        // Shadows / Highlights: less sharpening in the darks (the lights).
        const auto dark = step(0.05f, 0.15f), light = step(0.85f, 0.95f);
        auto f = smart(Remove::Gaussian, 200, 2);
        const auto over = [](const P::TileMap& m, float hi) { return m.get(301, 50).c[0] - hi; };
        const float d0 = over(*F::filtered(dark, f, nullptr), 0.15f), l0 = over(*F::filtered(light, f, nullptr), 0.95f);
        f.shadow_fade = 100; f.shadow_tonal = 50;
        const float d1 = over(*F::filtered(dark, f, nullptr), 0.15f), l1 = over(*F::filtered(light, f, nullptr), 0.95f);
        check("smart: the shadows' fade sharpens the darks less", d1 < d0 * 0.6f && d1 >= 0, num(d0) + " -> " + num(d1));
        check("smart: ... and leaves the lights", near(l1, l0, 0.002), num(l0) + " / " + num(l1));
        f.shadow_fade = 0; f.light_fade = 100; f.light_tonal = 50;
        const float l2 = over(*F::filtered(light, f, nullptr), 0.95f), d2 = over(*F::filtered(dark, f, nullptr), 0.15f);
        check("smart: the highlights' fade sharpens the lights less", l2 < l0 * 0.6f, num(l0) + " -> " + num(l2));
        check("smart: ... and leaves the darks", near(d2, d0, 0.002), num(d0) + " / " + num(d2));
        f.light_tonal = 0;
        check("smart: tonal width 0 fades nothing", near(over(*F::filtered(light, f, nullptr), 0.95f), l0, 1e-5), "");
    }

    std::cout << "\n-- filters: Dust & Scratches (s034) --\n";
    {
        auto salty = P::TileMap(P::Format{}, 300, 300).fill({0, 0, 300, 300}, P::rgba(0.4f, 0.4f, 0.4f, 1));
        for (int i = 0; i < 40; ++i) salty = salty.fill({(i * 37) % 290 + 5, (i * 53) % 290 + 5, 1, 1}, P::rgba(1, 1, 1, 1));
        auto ds = F::fresh(FilterKind::DustScratches);
        check("dust: defaults radius 1, threshold 0", ds.radius == 1 && ds.threshold == 0, "");
        check("dust: threshold kept by normalised", [&] { auto t = ds; t.threshold = 40; return F::normalised(t).threshold == 40; }(), "");
        const auto med = F::filtered(salty, F::fresh(FilterKind::Median), nullptr);
        const auto d0 = F::filtered(salty, ds, nullptr);
        check("dust: threshold 0 is the Median", same_in(*med, *d0, {0, 0, 300, 300}), "");
        ds.threshold = 255;
        check("dust: threshold 255 changes nothing", same_in(salty, *F::filtered(salty, ds, nullptr), {0, 0, 300, 300}), "");
        // A fine texture of +-6 levels with specks on it: threshold 30 takes the specks, keeps the texture.
        auto tex = P::TileMap(P::Format{}, 300, 300);
        for (int y = 0; y < 300; y += 2)
            for (int x = 0; x < 300; x += 2) {
                const float v = 0.4f + (((x / 2 + y / 2) % 2) ? 6.f : -6.f) / 255.f;
                tex = tex.fill({x, y, 2, 2}, P::rgba(v, v, v, 1));
            }
        const auto clean_tex = tex;
        for (int i = 0; i < 30; ++i) tex = tex.fill({(i * 37) % 280 + 10, (i * 53) % 280 + 10, 1, 1}, P::rgba(1, 1, 1, 1));
        ds.threshold = 30;
        ds.radius = 2;
        const auto dt = F::filtered(tex, ds, nullptr);
        bool specks = true;
        for (int i = 0; i < 30; ++i) specks = specks && dt->get((i * 37) % 280 + 10, (i * 53) % 280 + 10).c[0] < 0.5f;
        check("dust: the specks go", specks, "");
        check("dust: the texture under the threshold stays", same_in(clean_tex, *dt, {150, 3, 8, 6}), "");
        check("dust: reach is the radius", F::reach(ds) == 2, "");
    }

    std::cout << "\n-- filters: Reduce Noise (s034) --\n";
    {
        const auto rn = F::fresh(FilterKind::ReduceNoise);
        check("reduce: Photoshop's defaults (6, 60 %, 45 %, 25 %)", rn.amount == 6 && rn.preserve == 60 && rn.colour == 45 && rn.sharpen == 25, "");
        const auto n = noisy(8, true);
        auto f = rn;
        f.amount = 10; f.preserve = 0; f.sharpen = 0; f.colour = 0;
        const double before = spread(n, 0), after = spread(*F::filtered(n, f, nullptr), 0);
        check("reduce: strength 10 flattens grey noise to under half", after < before * 0.5, num(before) + " -> " + num(after));
        f.preserve = 100;
        const double kept = spread(*F::filtered(n, f, nullptr), 0);
        check("reduce: Preserve Details takes less", kept > after, num(after) + " / " + num(kept));
        f.preserve = 0;
        const auto e = F::filtered(step(), f, nullptr);
        check("reduce: an edge stays sharp", e->get(298, 50).c[0] < 0.3f && e->get(301, 50).c[0] > 0.7f,
              num(e->get(298, 50).c[0]) + " / " + num(e->get(301, 50).c[0]));
        // Colour noise: chroma calms, the grey stays.
        const auto cn = noisy(8, false);
        auto c = rn;
        c.amount = 0; c.sharpen = 0; c.colour = 100;
        const auto co = F::filtered(cn, c, nullptr);
        check("reduce: colour noise goes", spread(*co, -1) < spread(cn, -1) * 0.5, num(spread(cn, -1)) + " -> " + num(spread(*co, -1)));
        auto zero = rn;
        zero.amount = 0; zero.colour = 0; zero.sharpen = 0;
        check("reduce: all at 0 changes nothing", same_in(cn, *F::filtered(cn, zero, nullptr), {40, 40, 100, 100}, 1e-5f), "");
        const auto sq = P::TileMap(f32(), 400, 300).fill({100, 100, 150, 100}, P::rgba(1, 0, 0, 1));
        const auto so = F::filtered(sq, rn, nullptr);
        check("reduce: transparent stays, alpha stays", so->get(50, 50).c[3] == 0.f && so->get(150, 150).c[3] == 1.f, "");
        const P::Rect view{120, 120, 70, 60};
        check("reduce: the preview's rectangle == the result there",
              same_in(*F::filtered(cn, rn, nullptr), *F::filtered(cn, rn, nullptr, false, &view), view, 1e-4f), "");   // float sums
        auto sh = rn;
        sh.amount = 0; sh.colour = 0; sh.sharpen = 100;
        check("reduce: Sharpen Details sharpens", F::filtered(step(), sh, nullptr)->get(301, 50).c[0] > 0.76f, "");
    }

    {
        // Every filter can be found in the command palette, both ways (s034:
        // the palette's list is by hand and had missed the new three).
        std::string missing;
        for (const auto k : F::kinds())
            for (const std::string a : {"win.filter-" + std::string(F::key(k)), "win.filter-layer-" + std::string(F::key(k))}) {
                bool found = false;
                for (const auto& c : core::commands()) found = found || c.action == a;
                if (!found) missing += a + " ";
            }
        check("s034: every filter in the palette (Filter and New Filter Layer)", missing.empty(), missing);
    }
    std::cout << "\n-- filters: Maximum / Minimum (s034 m3) --\n";
    {
        const auto dot = P::TileMap(f32(), 300, 300).fill({0, 0, 300, 300}, P::rgba(0, 0, 0, 1)).fill({150, 150, 1, 1}, P::rgba(1, 1, 1, 1));
        auto mx = F::fresh(FilterKind::Maximum);
        mx.radius = 2;
        const auto sq = F::filtered(dot, mx, nullptr);
        check("maximum square: a 5 x 5 square", sq->get(152, 152).c[0] == 1.f && sq->get(148, 148).c[0] == 1.f && sq->get(153, 150).c[0] == 0.f, "");
        mx.round = true;
        const auto rd = F::filtered(dot, mx, nullptr);
        check("maximum round: a disc (no corners)", rd->get(152, 152).c[0] == 0.f && rd->get(152, 151).c[0] == 1.f && rd->get(150, 152).c[0] == 1.f, "");
        auto mn = F::fresh(FilterKind::Minimum);
        mn.radius = 3;
        const auto hole = P::TileMap(f32(), 300, 300).fill({0, 0, 300, 300}, P::rgba(1, 1, 1, 1)).fill({150, 150, 1, 1}, P::rgba(0, 0, 0, 1));
        const auto grown = F::filtered(hole, mn, nullptr);
        check("minimum: the dark grows", grown->get(153, 147).c[0] == 0.f && grown->get(154, 150).c[0] == 1.f, "");
        // Brute force on a random 8-bit picture: both shapes, both ways.
        auto rnd = P::TileMap(P::Format{}, 70, 60);
        std::uint32_t seed = 5;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 70; ++x) {
                seed = seed * 1664525u + 1013904223u;
                const float v = static_cast<float>((seed >> 24) & 255) / 255.f;
                rnd = rnd.fill({x, y, 1, 1}, P::rgba(v, 1 - v, 0.5f, 1));
            }
        for (const bool round : {false, true})
            for (const bool up : {true, false}) {
                auto f = F::fresh(up ? FilterKind::Maximum : FilterKind::Minimum);
                f.radius = 3; f.round = round;
                const auto out = F::filtered(rnd, f, nullptr);
                bool ok = true;
                for (int y = 0; y < 60 && ok; y += 3)
                    for (int x = 0; x < 70 && ok; x += 3) {
                        float best = up ? 0.f : 1.f;
                        for (int dy = -3; dy <= 3; ++dy)
                            for (int dx = -3; dx <= 3; ++dx) {
                                if (round && dx * dx + dy * dy > 3.5 * 3.5) continue;
                                const float v = rnd.get(std::clamp(x + dx, 0, 69), std::clamp(y + dy, 0, 59)).c[0];
                                best = up ? std::max(best, v) : std::min(best, v);
                            }
                        ok = near(out->get(x, y).c[0], best, 1e-6);
                    }
                check(std::string(up ? "maximum " : "minimum ") + (round ? "round" : "square") + ": the brute force agrees", ok, "");
            }
        const auto big = step();
        auto f = F::fresh(FilterKind::Maximum);
        f.radius = 7; f.round = true;
        const P::Rect view{280, 40, 50, 30};
        check("maximum: the preview's rectangle == the result there",
              same_in(*F::filtered(big, f, nullptr), *F::filtered(big, f, nullptr, false, &view), view), "");
        check("maximum: the light side spreads by the radius", F::filtered(big, f, nullptr)->get(293, 50).c[0] == 0.75f &&
                                                            F::filtered(big, f, nullptr)->get(292, 50).c[0] == 0.25f, "");
        auto shape = P::TileMap(f32(), 200, 200).fill({80, 80, 40, 40}, P::rgba(0, 0, 1, 1));
        auto g = F::fresh(FilterKind::Maximum);
        g.radius = 5;
        const auto sg = F::filtered(shape, g, nullptr);
        check("maximum on a transparent layer: the shape grows, its colour kept", sg->get(76, 100).c[3] == 1.f && sg->get(76, 100).c[2] == 1.f &&
                                                                                sg->get(74, 100).c[3] == 0.f, "");
        f.radius = 900;
        check("maximum: normalised to 500, round only on these", F::normalised(f).radius == 500 &&
              !F::normalised([] { auto b = F::fresh(FilterKind::BoxBlur); b.round = true; return b; }()).round, "");
        check("maximum / minimum: under Other", F::group(FilterKind::Maximum) == "Other" && F::group(FilterKind::Minimum) == "Other", "");
    }

    std::cout << "\n-- filters: Surface Blur (s034 m4) --\n";
    {
        const auto sb = F::fresh(FilterKind::SurfaceBlur);
        check("surface: Photoshop's defaults (5 px, 15 levels), under Blur", sb.radius == 5 && sb.threshold == 15 && F::group(FilterKind::SurfaceBlur) == "Blur", "");
        check("surface: threshold at least 2", [&] { auto t = sb; t.threshold = 0; return F::normalised(t).threshold == 2; }(), "");
        // Fine noise on grey, 8-bit: smoothed.
        auto n = P::TileMap(P::Format{}, 300, 300);
        std::uint32_t seed = 9;
        for (int y = 0; y < 300; ++y)
            for (int x = 0; x < 300; ++x) {
                seed = seed * 1664525u + 1013904223u;
                const float v = 0.5f + static_cast<float>(static_cast<int>((seed >> 24) % 11) - 5) / 255.f;
                n = n.fill({x, y, 1, 1}, P::rgba(v, v, v, 1));
            }
        const double before = spread(n, 0), after = spread(*F::filtered(n, sb, nullptr), 0);
        check("surface: fine noise smooths to under a third", after < before / 3, num(before) + " -> " + num(after));
        // A strong edge, 8-bit: untouched (127 levels > 2.5 x 15).
        const auto e8 = step(0.25f, 0.75f, P::Format{});
        const auto eo = F::filtered(e8, sb, nullptr);
        check("surface: an edge past the threshold stays exactly", same_in(e8, *eo, {290, 20, 20, 20}), "");
        // Brute force: the weighted mean of the square, 8-bit.
        auto rnd = P::TileMap(P::Format{}, 60, 50);
        for (int y = 0; y < 50; ++y)
            for (int x = 0; x < 60; ++x) {
                seed = seed * 1664525u + 1013904223u;
                const float v = static_cast<float>(100 + ((seed >> 24) % 40)) / 255.f;
                rnd = rnd.fill({x, y, 1, 1}, P::rgba(v, v, v, 1));
            }
        auto f = sb;
        f.radius = 2; f.threshold = 10;
        const auto out = F::filtered(rnd, f, nullptr);
        bool ok = true;
        const double reach = 2.5 * 10;
        for (int y = 0; y < 50 && ok; y += 3)
            for (int x = 0; x < 60 && ok; x += 3) {
                const int v = static_cast<int>(std::lround(rnd.get(x, y).c[0] * 255));
                double sum = 0, den = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int q = static_cast<int>(std::lround(rnd.get(std::clamp(x + dx, 0, 59), std::clamp(y + dy, 0, 49)).c[0] * 255));
                        const double w = std::max(0.0, 1 - std::abs(q - v) / reach);
                        sum += w * q; den += w;
                    }
                ok = std::abs(out->get(x, y).c[0] * 255 - sum / den) <= 0.51;   // 8-bit storage
            }
        check("surface: the brute force agrees (8-bit)", ok, "");
        const P::Rect view{120, 120, 70, 60};
        check("surface: the preview's rectangle == the result there",
              same_in(*F::filtered(n, sb, nullptr), *F::filtered(n, sb, nullptr, false, &view), view), "");
    }

    std::cout << "\n-- s034: live, filter layers, the file --\n";
    {
        core::live::clear_cache();
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        auto s = F::fresh(FilterKind::SmartSharpen);
        s.remove = Remove::Motion; s.angle = 30; s.shadow_fade = 40; s.light_fade = 20; s.light_tonal = 70;
        auto r = F::fresh(FilterKind::ReduceNoise);
        r.preserve = 33;
        auto ds = F::fresh(FilterKind::DustScratches);
        ds.radius = 4; ds.threshold = 12;
        check("s034: live Smart Sharpen", L::add_live(j, d, "L4", s), "");
        check("s034: live Reduce Noise", L::add_live(j, d, "L4", r), "");
        check("s034: live Dust & Scratches", L::add_live(j, d, "L4", ds), "");
        check("s034: a Smart Sharpen filter layer", !L::add_filter_layer(j, d, s, "L4").empty(), "");
        auto mn = F::fresh(FilterKind::Minimum);
        mn.radius = 3; mn.round = true;
        check("s034: live Minimum, round", L::add_live(j, d, "L4", mn), "");
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_s034_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image();
        c.layers = d.snapshot();
        c.next_id = d.next_id();
        std::string err;
        check("s034: saved", core::file::save_klimt(path, c, err), err);
        const auto back = core::file::open(path, err);
        check("s034: every setting back", back && back->layers.at("L4").filters == d.get("L4")->filters, err);
        bool layer_ok = false;
        if (back)
            for (const auto& [id, l] : back->layers)
                if (l.kind == core::Kind::Filter && l.filters.size() == 1 && l.filters[0].f == F::normalised(s)) layer_ok = true;
        check("s034: the filter layer's settings back", layer_ok, "");
        fs::remove(path);
        check("s034: the look is made (live)", core::live::look(*d.get("L4")) != nullptr, "");
    }
}
