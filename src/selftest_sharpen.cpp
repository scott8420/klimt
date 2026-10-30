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

    std::cout << "\n-- filters: Lens Blur (s037) --\n";
    {
        const auto lb = F::fresh(FilterKind::LensBlur);
        check("lens: Photoshop's defaults (15 px, a hexagon, threshold 255), under Blur", lb.radius == 15 && lb.blades == 6 && lb.threshold == 255 &&
              lb.bright == 0 && lb.curvature == 0 && F::group(FilterKind::LensBlur) == "Blur", "");
        {
            auto t = lb;
            t.blades = 11; t.angle = 400; t.curvature = 130; t.bright = -5; t.radius = 0.2;
            const auto n = F::normalised(t);
            check("lens: normalised -- blades 3..8, rotation 0..360, curvature / brightness 0..100, radius 1..100",
                  n.blades == 8 && n.angle == 360 && n.curvature == 100 && n.bright == 0 && n.radius == 1, "");
            auto g = F::fresh(FilterKind::GaussianBlur);
            g.blades = 3; g.bright = 50;
            check("lens: its settings go back to their defaults on another kind", F::normalised(g).blades == 6 && F::normalised(g).bright == 0, "");
        }
        // The iris's area: a hexagon's (3 sqrt 3 / 2 R^2), a circle's at full curvature.
        const auto area = [](const std::vector<F::IrisRow>& rows) {
            double a = 0;
            for (const auto& r : rows) if (r.hi > r.lo) a += r.hi - r.lo + 1;   // a row's pixels, its two half ends included
            return a;
        };
        const double R = 30;
        const double hex = area(F::iris(R, 6, 0, 0)), want_hex = 3 * std::sqrt(3.0) / 2 * R * R;
        const double circ = area(F::iris(R, 6, 1, 0)), want_circ = 3.14159265 * R * R;
        check("lens: the iris -- a hexagon's area", std::abs(hex - want_hex) / want_hex < 0.04, num(hex) + " vs " + num(want_hex));
        check("lens: the iris -- full curvature, a circle's", std::abs(circ - want_circ) / want_circ < 0.04, num(circ) + " vs " + num(want_circ));
        const auto h0 = F::iris(R, 6, 0, 0), h60 = F::iris(R, 6, 0, 60);
        bool sym = h0.size() == h60.size();
        for (std::size_t i = 0; i < h0.size() && sym; ++i) sym = std::abs(h0[i].lo - h60[i].lo) < 1e-6 && std::abs(h0[i].hi - h60[i].hi) < 1e-6;
        check("lens: a hexagon turned 60 degrees is the same iris", sym, "");
        const auto tri = F::iris(R, 3, 0, 0);
        const auto rw = [&](int dy) { const auto& r = tri[static_cast<std::size_t>(dy + 30)]; return r.hi - r.lo; };
        check("lens: a triangle, corner up -- narrow at the top, wide below", rw(-25) < rw(10), num(rw(-25)) + " vs " + num(rw(10)));
        // A light: one bright pixel on black -> the iris's shape (a corner up, a flat side to the right).
        const P::Format ff{P::Depth::F32, P::Mode::RGB, true};   // a light spread over ~1000 pixels is under one 8-bit level
        auto dark = P::TileMap(ff, 120, 120).fill({0, 0, 120, 120}, P::rgba(0, 0, 0, 1)).fill({60, 60, 1, 1}, P::rgba(1, 1, 1, 1));
        auto f = lb;
        f.radius = 20;
        const auto o = F::filtered(dark, f, nullptr);
        check("lens: one light -> a hexagon -- reaches 19 px toward a corner (up; the corner at 20) ...", o && o->get(60, 41).c[0] > 0.f, o ? num(o->get(60, 41).c[0]) : "");
        check("lens: ... but not 19 px toward a side (right; the side at 17.3)", o && o->get(79, 60).c[0] == 0.f, o ? num(o->get(79, 60).c[0]) : "");
        f.bright = 100; f.threshold = 200;
        const auto lit = F::filtered(dark, f, nullptr);
        check("lens: highlight brightness -- the light blooms brighter", o && lit && lit->get(60, 55).c[0] > 4 * o->get(60, 55).c[0],
              num(o ? o->get(60, 55).c[0] : 0) + " -> " + num(lit ? lit->get(60, 55).c[0] : 0));
        auto f2 = f; f2.threshold = 255;
        const auto unlit = F::filtered(dark, f2, nullptr);
        check("lens: threshold 255 -- nothing is a light", unlit && o && std::abs(unlit->get(60, 55).c[0] - o->get(60, 55).c[0]) < 1e-6f, "");
        // Brute force: the mean over the iris's rows, the ends' part pixels weighted (8-bit storage).
        auto rnd = P::TileMap(P::Format{}, 50, 40);
        std::uint32_t seed = 21;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 50; ++x) {
                seed = seed * 1664525u + 1013904223u;
                const float v = static_cast<float>((seed >> 24) % 256) / 255.f;
                rnd = rnd.fill({x, y, 1, 1}, P::rgba(v, 1 - v, 0.5f, 1));
            }
        auto b = lb;
        b.radius = 4; b.blades = 5; b.curvature = 30; b.angle = 17;
        const auto out = F::filtered(rnd, b, nullptr);
        const auto rows = F::iris(4, 5, 0.3, 17);
        bool ok = out.has_value();
        for (int y = 0; y < 40 && ok; y += 3)
            for (int x = 0; x < 50 && ok; x += 3) {
                double sum = 0, den = 0;
                for (int dy = -4; dy <= 4; ++dy) {
                    const auto& r = rows[static_cast<std::size_t>(dy + 4)];
                    if (!(r.hi > r.lo)) continue;
                    for (int dx = -8; dx <= 8; ++dx) {   // pixel dx covers [dx - .5, dx + .5]; the span [lo - .5, hi + .5]
                        const double w = std::clamp(std::min(dx + 0.5, r.hi + 0.5) - std::max(dx - 0.5, r.lo - 0.5), 0.0, 1.0);
                        if (w <= 0) continue;
                        sum += w * rnd.get(std::clamp(x + dx, 0, 49), std::clamp(y + dy, 0, 39)).c[0];
                        den += w;
                    }
                }
                ok = std::abs(out->get(x, y).c[0] - sum / den) <= 0.6 / 255;
                if (!ok) std::cout << "    at " << x << "," << y << ": " << out->get(x, y).c[0] * 255 << " vs " << sum / den * 255 << "\n";
            }
        check("lens: the brute force agrees (8-bit, a curved pentagon turned 17 degrees, the edges clamped)", ok, "");
        const P::Rect view{30, 30, 50, 40};
        check("lens: the preview's rectangle == the result there", same_in(*F::filtered(dark, f, nullptr), *F::filtered(dark, f, nullptr, false, &view), view), "");
    }

    std::cout << "\n-- filters: Lens Blur's depth map (s039) --\n";
    {
        auto lb = F::fresh(FilterKind::LensBlur);
        check("depth: Photoshop's defaults -- no source, focal distance 0, not inverted",
              lb.depth == core::DepthSource::None && lb.focal == 0 && !lb.invert, "");
        {
            auto t = lb; t.focal = 300.4; t.depth = static_cast<core::DepthSource>(9);
            const auto n = F::normalised(t);
            check("depth: normalised -- focal 0..255, an unknown source is None", n.focal == 255 && n.depth == core::DepthSource::None, "");
            auto g = F::fresh(FilterKind::GaussianBlur);
            g.depth = core::DepthSource::LayerMask; g.focal = 90; g.invert = true;
            const auto gn = F::normalised(g);
            check("depth: its settings go back to their defaults on another kind",
                  gn.depth == core::DepthSource::None && gn.focal == 0 && !gn.invert, "");
        }
        // The blur's share by depth: 0 at the focal distance, 1 at either end.
        auto a = lb;
        check("depth: focal 0 -- black sharp, white the full radius, grey half",
              near(F::depth_amount(a, 0), 0, 1e-9) && near(F::depth_amount(a, 1), 1, 1e-9) && near(F::depth_amount(a, 0.5), 0.5, 1e-9), "");
        a.focal = 100;
        check("depth: focal 100 -- 100 sharp, 1 and 255 (nearly) fully blurred (Photoshop's example)",
              near(F::depth_amount(a, 100 / 255.0), 0, 1e-9) && F::depth_amount(a, 1 / 255.0) > 0.98 && near(F::depth_amount(a, 1), 1, 1e-9),
              num(F::depth_amount(a, 1 / 255.0)));
        a.focal = 0; a.invert = true;
        check("depth: Invert turns the map over", near(F::depth_amount(a, 1), 0, 1e-9) && near(F::depth_amount(a, 0), 1, 1e-9), "");
        for (const char* k : {"none", "transparency", "layer-mask"})
            check(std::string("depth: the key round-trips -- ") + k, F::depth_from(k) && F::depth_key(*F::depth_from(k)) == k, "");

        // A picture with detail everywhere (8-bit), opaque.
        auto rnd = P::TileMap(P::Format{}, 160, 120);
        std::uint32_t seed = 5;
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 160; ++x) {
                seed = seed * 1664525u + 1013904223u;
                const float v = static_cast<float>((seed >> 24) % 256) / 255.f;
                rnd = rnd.fill({x, y, 1, 1}, P::rgba(v, 0.5f * v, 1 - v, 1));
            }
        auto even = lb; even.radius = 8;
        const auto blurred = F::filtered(rnd, even, nullptr);
        const P::Rect all{0, 0, 160, 120};
        auto tr = even; tr.depth = core::DepthSource::Transparency;
        const auto t0 = F::filtered(rnd, tr, nullptr);
        check("depth: Transparency, opaque, focal 0 -- every pixel at the full radius (as None)",
              blurred && t0 && same_in(*blurred, *t0, all, 1.5f / 255), "");
        tr.focal = 255;
        const auto t255 = F::filtered(rnd, tr, nullptr);
        check("depth: Transparency, opaque, focal 255 -- every pixel sharp (nothing changes)", t255 && same_in(rnd, *t255, all), "");

        // A layer mask: the left half hidden (black: depth 0), the right shown (white: 255).
        P::Px hide{}; hide.c[0] = 1.f;
        const P::Format mf{P::Depth::U8, P::Mode::Gray, false};
        const auto mask = P::TileMap(mf, 160, 120).fill({0, 0, 80, 120}, hide);
        auto lm = even; lm.depth = core::DepthSource::LayerMask;
        check("depth: Layer Mask wants the mask", F::wants_mask(lm) && !F::wants_mask(even), "");
        const auto m0 = F::filtered(rnd, lm, nullptr, false, nullptr, {}, {}, &mask);
        check("depth: Layer Mask, focal 0 -- the black half stays sharp", m0 && same_in(rnd, *m0, {0, 0, 70, 120}), "");
        check("depth: ... the white half has the full blur", m0 && blurred && same_in(*blurred, *m0, {90, 0, 70, 120}, 1.5f / 255), "");
        lm.invert = true;
        const auto mi = F::filtered(rnd, lm, nullptr, false, nullptr, {}, {}, &mask);
        check("depth: inverted -- the other way round", mi && blurred && same_in(rnd, *mi, {90, 0, 70, 120}) &&
              same_in(*blurred, *mi, {0, 0, 70, 120}, 1.5f / 255), "");
        lm.invert = false;
        const auto nomask = F::filtered(rnd, lm, nullptr);
        check("depth: Layer Mask with no mask -- the blur is even (as None)", nomask && blurred && same_in(*blurred, *nomask, all), "");
        // Mid grey: half the radius -- as Lens Blur of half the radius, evenly.
        P::Px mid{}; mid.c[0] = 0.5f;
        const auto grey = P::TileMap(mf, 160, 120).fill(all, mid);
        auto big = lm; big.radius = 16;
        const auto half = F::filtered(rnd, big, nullptr, false, nullptr, {}, {}, &grey);
        check("depth: mid grey at radius 16 == an even blur of 8", half && blurred && same_in(*blurred, *half, all, 2.5f / 255), "");
        const P::Rect view{50, 30, 60, 50};
        check("depth: the preview's rectangle == the result there",
              same_in(*F::filtered(rnd, lm, nullptr, false, nullptr, {}, {}, &mask),
                      *F::filtered(rnd, lm, nullptr, false, &view, {}, {}, &mask), view), "");

        // Live: a Lens Blur reading the layer's mask -- the look follows the mask.
        core::live::clear_cache();
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        auto live = F::fresh(FilterKind::LensBlur);
        live.radius = 6; live.depth = core::DepthSource::LayerMask;
        check("depth: live Lens Blur on a layer", L::add_live(j, d, "L4", live), "");
        const auto px = d.get("L4")->pixels;
        const P::Rect box{0, 0, std::min(px->width(), 200), std::min(px->height(), 200)};
        const auto before = core::live::look(*d.get("L4"));
        check("depth: a mask added (Hide All: black, depth 0)", L::add_mask(j, d, "L4", L::MaskFrom::HideAll), "");
        const auto after = core::live::look(*d.get("L4"));
        check("depth: live -- with no mask the blur is even, with a black one (focal 0) the layer is sharp",
              before && after && !same_in(*px, *before, box) && same_in(*px, *after, box), "");
        live.focal = 40; live.invert = true;
        check("depth: the live filter's focal distance and Invert set", L::set_live(j, d, "L4", d.get("L4")->filters.size() - 1, live), "");
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_s039_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image();
        c.layers = d.snapshot();
        c.next_id = d.next_id();
        std::string err;
        check("depth: saved", core::file::save_klimt(path, c, err), err);
        const auto back = core::file::open(path, err);
        check("depth: the source, focal distance and Invert back from the file",
              back && back->layers.at("L4").filters == d.get("L4")->filters &&
              back->layers.at("L4").filters.back().f.depth == core::DepthSource::LayerMask && back->layers.at("L4").filters.back().f.focal == 40, err);
        fs::remove(path);
    }

    std::cout << "\n-- filters: Lens Blur's Noise (s039 m2) --\n";
    {
        auto lb = F::fresh(FilterKind::LensBlur);
        lb.radius = 6;
        {
            auto t = lb; t.amount = 150; t.gaussian = true; t.mono = true; t.seed = 77;
            const auto n = F::normalised(t);
            check("noise: normalised -- 0..100 %, Gaussian / Monochromatic / the seed kept",
                  n.amount == 100 && n.gaussian && n.mono && n.seed == 77 && F::fresh(FilterKind::LensBlur).amount == 0, "");
        }
        const auto flat = P::TileMap(f32(), 300, 300).fill({0, 0, 300, 300}, P::rgba(0.5f, 0.5f, 0.5f, 1));
        const auto plain = F::filtered(flat, lb, nullptr);
        auto nz = lb; nz.amount = 40; nz.seed = 3;
        const auto grain = F::filtered(flat, nz, nullptr);
        check("noise: none at 0 -- a flat grey blurs to itself", plain && spread(*plain, 0) < 1e-6, plain ? num(spread(*plain, 0)) : "");
        check("noise: 40 % -- grain on the blurred grey", grain && spread(*grain, 0) > 0.01, grain ? num(spread(*grain, 0)) : "");
        check("noise: the same seed, the same grain", grain && same_in(*grain, *F::filtered(flat, nz, nullptr), {0, 0, 300, 300}), "");
        auto other = nz; other.seed = 4;
        check("noise: another seed, other grain", grain && !same_in(*grain, *F::filtered(flat, other, nullptr), {0, 0, 300, 300}), "");
        auto mono = nz; mono.mono = true;
        const auto mg = F::filtered(flat, mono, nullptr);
        check("noise: Monochromatic -- grey grain, no colour in it", mg && spread(*mg, -1) < 1e-6 && grain && spread(*grain, -1) > 0.01, "");
        // A depth map: the sharp half keeps its own (no grain), the blurred half gets it.
        P::Px hide{}; hide.c[0] = 1.f;
        const auto mask = P::TileMap(P::Format{P::Depth::U8, P::Mode::Gray, false}, 300, 300).fill({0, 0, 150, 300}, hide);
        auto dm = nz; dm.depth = core::DepthSource::LayerMask;
        const auto half = F::filtered(flat, dm, nullptr, false, nullptr, {}, {}, &mask);
        check("noise: with a depth map -- the sharp part has none", half && same_in(flat, *half, {0, 0, 140, 300}), "");
        double s = 0, s2 = 0; int n = 0;
        for (int y = 20; y < 280; y += 2)
            for (int x = 170; x < 290; x += 2) { const double v = half ? half->get(x, y).c[0] : 0; s += v; s2 += v * v; ++n; }
        const double sd = std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n)));
        check("noise: ... the blurred part has it", sd > 0.01, num(sd));
        const P::Rect view{100, 60, 120, 90};
        check("noise: the preview's rectangle == the result there",
              same_in(*F::filtered(flat, dm, nullptr, false, nullptr, {}, {}, &mask), *F::filtered(flat, dm, nullptr, false, &view, {}, {}, &mask), view), "");
        // The file keeps it.
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        auto lf = nz; lf.gaussian = true; lf.seed = 91;
        check("noise: live Lens Blur with Noise", L::add_live(j, d, "L4", lf), "");
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_s039n_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image();
        c.layers = d.snapshot();
        c.next_id = d.next_id();
        std::string err;
        check("noise: saved", core::file::save_klimt(path, c, err), err);
        const auto back = core::file::open(path, err);
        check("noise: amount, distribution and seed back from the file",
              back && back->layers.at("L4").filters == d.get("L4")->filters && back->layers.at("L4").filters.back().f.seed == 91, err);
        fs::remove(path);
    }

    std::cout << "\n-- filters: Radial Blur (s040) --\n";
    {
        const auto rb = F::fresh(FilterKind::RadialBlur);
        check("radial: Photoshop's defaults (amount 10, Spin, Good, the middle), under Blur",
              rb.amount == 10 && !rb.zoom && rb.quality == 1 && rb.cx == 50 && rb.cy == 50 && F::group(FilterKind::RadialBlur) == "Blur", "");
        {
            auto t = rb; t.amount = 300; t.quality = 7; t.cx = -4; t.cy = 120.06;
            const auto n = F::normalised(t);
            check("radial: normalised -- amount 1..100, quality 0..2, the centre 0..100 %",
                  n.amount == 100 && n.quality == 2 && n.cx == 0 && n.cy == 100, "");
            auto g = F::fresh(FilterKind::GaussianBlur);
            g.zoom = true; g.quality = 2; g.cx = 10;
            const auto gn = F::normalised(g);
            check("radial: its settings go back to their defaults on another kind", !gn.zoom && gn.quality == 1 && gn.cx == 50, "");
        }
        auto q = rb; q.amount = 50;
        check("radial: no passes at the centre (nothing to blur)", F::radial_passes(q, 0) == 0, "");
        check("radial: more passes for a longer streak", F::radial_passes(q, 2000) > F::radial_passes(q, 20), "");
        auto dr = q; dr.quality = 0; auto be = q; be.quality = 2;
        check("radial: Draft < Good < Best", F::radial_passes(dr, 300) < F::radial_passes(q, 300) && F::radial_passes(q, 300) < F::radial_passes(be, 300),
              num(F::radial_passes(dr, 300)) + " " + num(F::radial_passes(q, 300)) + " " + num(F::radial_passes(be, 300)));
        check("radial: 2^passes taps cover the streak (Good: a tap a pixel at least)",
              (1 << F::radial_passes(q, 300)) >= F::radial_length(q, 300), "");
        // Rings (a value by the distance from the middle) and spokes (by the angle).
        const int N = 240;
        auto rings = P::TileMap(f32(), N, N), spokes = P::TileMap(f32(), N, N);
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const double vx = x + 0.5 - N / 2.0, vy = y + 0.5 - N / 2.0;
                const float r = static_cast<float>(0.5 + 0.4 * std::cos(std::sqrt(vx * vx + vy * vy) / 6));
                const float a = static_cast<float>(0.5 + 0.4 * std::cos(6 * std::atan2(vy, vx)));
                rings = rings.fill({x, y, 1, 1}, P::rgba(r, r, r, 1));
                spokes = spokes.fill({x, y, 1, 1}, P::rgba(a, a, a, 1));
            }
        const auto diff = [&](const P::TileMap& a, const P::TileMap& b) {   // the mean difference, 20..100 px out
            double d = 0; int n = 0;
            for (int y = 0; y < N; y += 3)
                for (int x = 0; x < N; x += 3) {
                    const double rr = std::hypot(x + 0.5 - N / 2.0, y + 0.5 - N / 2.0);
                    if (rr < 20 || rr > 100) continue;
                    d += std::abs(a.get(x, y).c[0] - b.get(x, y).c[0]); ++n;
                }
            return d / n;
        };
        auto spin = rb; spin.amount = 40;
        auto zoom = spin; zoom.zoom = true;
        const auto sr = F::filtered(rings, spin, nullptr), ss = F::filtered(spokes, spin, nullptr);
        const auto zr = F::filtered(rings, zoom, nullptr), zs = F::filtered(spokes, zoom, nullptr);
        check("radial: Spin leaves rings as they are", sr && diff(*sr, rings) < 0.01, sr ? num(diff(*sr, rings)) : "");
        check("radial: Spin smears spokes (10x what it does to rings)", ss && sr && diff(*ss, spokes) > 10 * diff(*sr, rings), ss ? num(diff(*ss, spokes)) : "");
        check("radial: Zoom leaves spokes as they are", zs && diff(*zs, spokes) < 0.02, zs ? num(diff(*zs, spokes)) : "");
        check("radial: Zoom smears rings (10x what it does to spokes)", zr && zs && diff(*zr, rings) > 10 * diff(*zs, spokes), zr ? num(diff(*zr, rings)) : "");
        // The centre moved: the spokes' middle no longer still under Zoom.
        auto off = zoom; off.cx = 25; off.cy = 25;
        const auto zo = F::filtered(spokes, off, nullptr);
        check("radial: the centre moved -- Zoom no longer leaves the spokes", zo && zs && diff(*zo, spokes) > 10 * diff(*zs, spokes), zo ? num(diff(*zo, spokes)) : "");
        const P::Rect view{100, 40, 90, 70};
        check("radial: the preview's rectangle == the result there",
              same_in(*F::filtered(spokes, off, nullptr), *F::filtered(spokes, off, nullptr, false, &view), view), "");
        // Live, and the file.
        core::live::clear_cache();
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        auto lv = rb; lv.amount = 30; lv.zoom = true; lv.quality = 2; lv.cx = 30; lv.cy = 70;
        check("radial: live", L::add_live(j, d, "L4", lv), "");
        check("radial: a filter layer", !L::add_filter_layer(j, d, lv, "L4").empty(), "");
        check("radial: the look is made", core::live::look(*d.get("L4")) != nullptr, "");
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_s040_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image();
        c.layers = d.snapshot();
        c.next_id = d.next_id();
        std::string err;
        check("radial: saved", core::file::save_klimt(path, c, err), err);
        const auto back = core::file::open(path, err);
        check("radial: Method, Quality and the centre back from the file",
              back && back->layers.at("L4").filters == d.get("L4")->filters && back->layers.at("L4").filters.back().f.cx == 30, err);
        fs::remove(path);
    }

    std::cout << "\n-- filters: Distort -- Twirl, Pinch, Spherize (s040 m4) --\n";
    {
        const auto tw = F::fresh(FilterKind::Twirl), pi = F::fresh(FilterKind::Pinch), sp = F::fresh(FilterKind::Spherize);
        check("distort: Photoshop's defaults (Twirl 50 deg, Pinch 50 %, Spherize 100 % Normal), under Distort",
              tw.angle == 50 && pi.amount == 50 && sp.amount == 100 && sp.axis == 0 && F::group(FilterKind::Twirl) == "Distort" &&
              F::group(FilterKind::Pinch) == "Distort" && F::group(FilterKind::Spherize) == "Distort", "");
        {
            auto t = tw; t.angle = 5000; auto q = pi; q.amount = -300; auto r = sp; r.axis = 9; r.amount = 140;
            check("distort: normalised -- angle -999..999, amounts -100..100, mode 0..2",
                  F::normalised(t).angle == 999 && F::normalised(q).amount == -100 && F::normalised(r).axis == 2 && F::normalised(r).amount == 100, "");
            auto g = F::fresh(FilterKind::GaussianBlur); g.axis = 2;
            check("distort: Spherize's mode goes back to Normal on another kind", F::normalised(g).axis == 0, "");
        }
        // The maps, on an ellipse at (100, 80), half-axes 100 x 80.
        const double ex = 100, ey = 80, rx = 100, ry = 80;
        double sx = 0, sy = 0;
        check("distort: outside the ellipse -- unchanged", !F::warp_source(tw, ex, ey, rx, ry, 1, 1, sx, sy), "");
        const auto dist = [&](double x, double y) { return std::hypot((x - ex) / rx, (y - ey) / ry); };
        bool keeps = true, rim = true;
        for (double a = 0; a < 6.28; a += 0.4)
            for (const double d : {0.2, 0.5, 0.8}) {
                const double x = ex + d * rx * std::cos(a), y = ey + d * ry * std::sin(a);
                if (F::warp_source(tw, ex, ey, rx, ry, x, y, sx, sy)) keeps = keeps && std::abs(dist(sx, sy) - d) < 1e-9;
                const double x9 = ex + 0.99999 * rx * std::cos(a), y9 = ey + 0.99999 * ry * std::sin(a);   // Spherize 100 % is steep at the rim (1 - sqrt(1 - d^2))
                for (const auto* f : {&tw, &pi, &sp})
                    if (F::warp_source(*f, ex, ey, rx, ry, x9, y9, sx, sy)) rim = rim && std::hypot(sx - x9, sy - y9) < 1.0;
            }
        check("distort: Twirl turns, keeping each pixel's distance from the middle", keeps, "");
        check("distort: at the rim, all three read (nearly) where they are -- no seam", rim, "");
        const auto reads_from = [&](const core::Filter& f, double d) {
            F::warp_source(f, ex, ey, rx, ry, ex + d * rx, ey, sx, sy);
            return dist(sx, sy);
        };
        auto pm = pi; pm.amount = -50; auto sm = sp; sm.amount = -100;
        check("distort: Pinch + reads from further out (the middle squeezed in), - from nearer",
              reads_from(pi, 0.4) > 0.4 && reads_from(pm, 0.4) < 0.4, num(reads_from(pi, 0.4)) + " " + num(reads_from(pm, 0.4)));
        check("distort: Spherize + reads from nearer (a bulge), - from further out (a dent)",
              reads_from(sp, 0.4) < 0.4 && reads_from(sm, 0.4) > 0.4, num(reads_from(sp, 0.4)) + " " + num(reads_from(sm, 0.4)));
        auto hz = sp; hz.axis = 1; auto vt = sp; vt.axis = 2;
        F::warp_source(hz, ex, ey, rx, ry, 150, 100, sx, sy);
        const bool h_ok = sy == 100 && sx != 150;
        F::warp_source(vt, ex, ey, rx, ry, 150, 100, sx, sy);
        check("distort: Spherize Horizontal only moves along the row; Vertical only along the column", h_ok && sx == 150 && sy != 100, "");
        // On a picture: the corners (outside the ellipse) unchanged; Pinch 0 changes nothing.
        auto rnd = P::TileMap(P::Format{}, 200, 160);
        std::uint32_t seed = 9;
        for (int y = 0; y < 160; ++y)
            for (int x = 0; x < 200; ++x) {
                seed = seed * 1664525u + 1013904223u;
                const float v = static_cast<float>((seed >> 24) % 256) / 255.f;
                rnd = rnd.fill({x, y, 1, 1}, P::rgba(v, 1 - v, 0.3f, 1));
            }
        const auto twd = F::filtered(rnd, tw, nullptr);
        check("distort: Twirl -- the corners, outside the ellipse, as they were", twd && same_in(rnd, *twd, {0, 0, 12, 12}) && same_in(rnd, *twd, {188, 148, 12, 12}), "");
        check("distort: Twirl -- the middle moved", twd && !same_in(rnd, *twd, {80, 60, 40, 40}), "");
        auto p0 = pi; p0.amount = 0;
        const auto pz = F::filtered(rnd, p0, nullptr);
        check("distort: Pinch 0 -- nothing changes (within a level)", pz && same_in(rnd, *pz, {0, 0, 200, 160}, 0.6f / 255), "");
        const P::Rect view{60, 50, 70, 50};
        check("distort: the preview's rectangle == the result there (the ellipse doesn't follow the view)",
              same_in(*F::filtered(rnd, sp, nullptr), *F::filtered(rnd, sp, nullptr, false, &view), view), "");
        // Live, and the file.
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        auto lv = sp; lv.axis = 2; lv.amount = -40;
        check("distort: live Spherize", L::add_live(j, d, "L4", lv), "");
        check("distort: a Twirl filter layer", !L::add_filter_layer(j, d, tw, "L4").empty(), "");
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_s040d_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image();
        c.layers = d.snapshot();
        c.next_id = d.next_id();
        std::string err;
        check("distort: saved", core::file::save_klimt(path, c, err), err);
        const auto back = core::file::open(path, err);
        check("distort: Spherize's mode and amount back from the file",
              back && back->layers.at("L4").filters == d.get("L4")->filters && back->layers.at("L4").filters.back().f.axis == 2, err);
        fs::remove(path);
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
        auto lb = F::fresh(FilterKind::LensBlur);   // s037
        lb.blades = 5; lb.curvature = 20; lb.angle = 30; lb.bright = 40; lb.threshold = 210;
        check("s037: live Lens Blur", L::add_live(j, d, "L4", lb), "");
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
