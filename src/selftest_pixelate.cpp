// klimt_selftest -- PIXELATE (s043). docs/filters.md. Rules, not looks: a
// Mosaic cell is one colour, the mean of its pixels, its cells on the image's
// grid; a Crystallize pixel takes its nearest point's colour; Pointillize's
// dots sit on the background colour; Fragment is four copies averaged; the
// preview's rectangle == the result; the settings survive a .klimt.
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Filter.hpp"
#include "core/Live.hpp"

#include <algorithm>
#include <array>
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

namespace {
std::string num(double v) { return std::to_string(v); }
P::Format f32() { P::Format f; f.depth = P::Depth::F32; return f; }
bool same_in(const P::TileMap& a, const P::TileMap& b, const P::Rect& r, float eps = 0) {
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            for (int c = 0; c < 4; ++c)
                if (std::abs(a.get(x, y).c[static_cast<std::size_t>(c)] - b.get(x, y).c[static_cast<std::size_t>(c)]) > eps) return false;
    return true;
}
// A smooth ramp: red with x, green with y, opaque -- every pixel its own colour.
P::TileMap ramp(int w, int h) {
    P::TileMap m(f32(), w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) m = m.fill({x, y, 1, 1}, P::rgba(static_cast<float>(x) / static_cast<float>(w), static_cast<float>(y) / static_cast<float>(h), 0.5f, 1));
    return m;
}
bool saved_back(core::Document& d, const std::string& tag, std::string& err) {
    namespace fs = std::filesystem;
    const auto path = (fs::temp_directory_path() / ("klimt_s043" + tag + "_" + std::to_string(::getpid()) + ".klimt")).string();
    core::file::Contents c;
    c.image = d.image();
    c.layers = d.snapshot();
    c.next_id = d.next_id();
    if (!core::file::save_klimt(path, c, err)) return false;
    const auto back = core::file::open(path, err);
    fs::remove(path);
    return back && back->layers.at("L4").filters == d.get("L4")->filters;
}
}  // namespace

void selftest_pixelate(const Check& check) {
    std::cout << "\n-- filters: Pixelate -- Mosaic, Crystallize, Pointillize, Fragment (s043 m1) --\n";
    const auto rp = ramp(300, 280);   // more than one tile row (256) -- the cells cross bands
    {
        const auto mo = F::fresh(FilterKind::Mosaic), cr = F::fresh(FilterKind::Crystallize), po = F::fresh(FilterKind::Pointillize),
                   fr = F::fresh(FilterKind::Fragment);
        check("pixelate: under Pixelate; Photoshop's cell sizes (10, 10, 5)",
              F::group(FilterKind::Mosaic) == "Pixelate" && F::group(FilterKind::Crystallize) == "Pixelate" && F::group(FilterKind::Pointillize) == "Pixelate" &&
                  F::group(FilterKind::Fragment) == "Pixelate" && mo.radius == 10 && cr.radius == 10 && po.radius == 5, "");
        auto t = mo; t.radius = 999; auto u = cr; u.radius = 1; u.seed = 7; auto v = po; v.c2 = {2, 0.5f, -1};
        check("pixelate: normalised -- Mosaic 2..200, Crystallize 3..300, its seed kept; Pointillize's colour 0..1",
              F::normalised(t).radius == 200 && F::normalised(u).radius == 3 && F::normalised(u).seed == 7 &&
                  F::normalised(v).c2 == (std::array<float, 3>{1, 0.5f, 0}), "");
        check("pixelate: seeded / coloured / placed", F::seeded(FilterKind::Crystallize) && F::seeded(FilterKind::Pointillize) &&
              !F::seeded(FilterKind::Mosaic) && F::coloured(FilterKind::Pointillize) && !F::coloured(FilterKind::Crystallize) &&
              F::placed(mo) && F::placed(fr), "");
        auto bad = F::fresh(FilterKind::Mosaic); bad.seed = 9;
        check("pixelate: a Mosaic has no seed (back to 1)", F::normalised(bad).seed == 1, "");
        (void)fr;
    }
    {
        // Mosaic: each 10 x 10 cell of the image's grid one colour -- the mean of the ramp there.
        auto mo = F::fresh(FilterKind::Mosaic);
        mo.radius = 10;
        const auto m = F::filtered(rp, mo, nullptr);
        bool flat = m.has_value(), mean = m.has_value();
        for (int cy = 0; cy < 28 && flat; cy += 3)
            for (int cx = 0; cx < 30 && flat; cx += 3) {
                const auto first = m->get(cx * 10, cy * 10);
                for (int y = cy * 10; y < cy * 10 + 10; ++y)
                    for (int x = cx * 10; x < cx * 10 + 10; ++x)
                        if (std::abs(m->get(x, y).c[0] - first.c[0]) > 1e-6f || std::abs(m->get(x, y).c[1] - first.c[1]) > 1e-6f) flat = false;
                const float want_r = (cx * 10 + 4.5f) / 300.f, want_g = (cy * 10 + 4.5f) / 280.f;
                mean = mean && std::abs(first.c[0] - want_r) < 1e-4f && std::abs(first.c[1] - want_g) < 1e-4f;
            }
        check("pixelate: Mosaic -- each cell of the image's grid one colour", flat, "");
        check("pixelate: Mosaic -- that colour the cell's mean", mean, m ? num(m->get(0, 0).c[0]) : "");
        // A cell across the tile rows (y 250..259): one colour on both sides of 256.
        check("pixelate: Mosaic -- a cell across two bands is one cell",
              m && std::abs(m->get(5, 252).c[1] - m->get(5, 258).c[1]) < 1e-6f && std::abs(m->get(5, 249).c[1] - m->get(5, 250).c[1]) > 1e-3f, "");
        const P::Rect view{33, 47, 120, 90};
        check("pixelate: Mosaic -- the preview's rectangle == the result there", same_in(*m, *F::filtered(rp, mo, nullptr, false, &view), view), "");
        // A selection: the cells stay the image's (not the selection's).
        const auto sel = P::TileMap(P::Format{P::Depth::U8, P::Mode::Gray, false}, 300, 280).fill({15, 15, 100, 100}, P::rgba(1, 1, 1, 1));
        const auto ms = F::filtered(rp, mo, &sel);
        check("pixelate: Mosaic through a selection -- inside, the image's cells; outside, untouched",
              ms && std::abs(ms->get(25, 25).c[0] - m->get(25, 25).c[0]) < 1e-6f && same_in(rp, *ms, {150, 150, 50, 50}), "");
    }
    {
        // Crystallize: every pixel is the colour of the point nearest it (of the 3 x 3 cells round).
        auto cr = F::fresh(FilterKind::Crystallize);
        cr.radius = 20; cr.seed = 3;
        const auto c = F::filtered(rp, cr, nullptr);
        bool nearest = c.has_value();
        std::string why;
        int colours = 0;
        float last = -1;
        for (int y = 5; y < 275; y += 13)
            for (int x = 5; x < 295; x += 11) {
                const double X = x + 0.5, Y = y + 0.5;
                const int gi = static_cast<int>(std::floor(X / 20)), gj = static_cast<int>(std::floor(Y / 20));
                double bd = 1e300; F::Point2 bp{};
                for (int j = gj - 1; j <= gj + 1; ++j)
                    for (int i = gi - 1; i <= gi + 1; ++i) {
                        const auto p = F::cell_point(i, j, 20, 3);
                        const double d = (p.x - X) * (p.x - X) + (p.y - Y) * (p.y - Y);
                        if (d < bd) { bd = d; bp = p; }
                    }
                // Its colour: the ramp near that point (the mean of a 11 x 11 square round it, inside the picture).
                const float want_r = std::clamp(static_cast<float>(std::floor(bp.x)) / 300.f, 0.f, 1.f);
                if (std::abs(c->get(x, y).c[0] - want_r) >= 0.03f && nearest) {
                    why = num(x) + "," + num(y) + " got " + num(c->get(x, y).c[0]) + " want " + num(want_r) + " pt " + num(bp.x) + "," + num(bp.y);
                    nearest = false;
                }
                if (c->get(x, y).c[0] != last) { ++colours; last = c->get(x, y).c[0]; }
            }
        check("pixelate: Crystallize -- each pixel the colour at its nearest point", nearest, why);
        check("pixelate: Crystallize -- many cells (not one colour)", colours > 20, num(colours));
        auto other = cr; other.seed = 4;
        const auto c2 = F::filtered(rp, other, nullptr);
        check("pixelate: Crystallize -- another seed, other cells", c2 && !same_in(*c, *c2, {0, 0, 300, 280}, 1e-4f), "");
        const P::Rect view{60, 200, 150, 70};   // across the band boundary at 256
        check("pixelate: Crystallize -- the preview's rectangle == the result there", same_in(*c, *F::filtered(rp, cr, nullptr, false, &view), view), "");
    }
    {
        // Pointillize on a flat colour: the dots that colour (within the variation), the gaps the background.
        auto po = F::fresh(FilterKind::Pointillize);
        po.radius = 16; po.seed = 5; po.c2 = {1, 1, 1};
        const auto flat = P::TileMap(f32(), 200, 200).fill({0, 0, 200, 200}, P::rgba(0.2f, 0.4f, 0.6f, 1));
        const auto p = F::filtered(flat, po, nullptr);
        int dot = 0, gap = 0, other = 0;
        for (int y = 0; y < 200; y += 3)
            for (int x = 0; x < 200; x += 3) {
                const auto q = p->get(x, y);
                const float k = q.c[0] / 0.2f;   // a dot: the colour scaled by its variation (0.92 .. 1.08)
                if (q.c[0] == 1.f && q.c[1] == 1.f && q.c[2] == 1.f) ++gap;
                else if (k > 0.91f && k < 1.09f && std::abs(q.c[1] - 0.4f * k) < 1e-4f && std::abs(q.c[2] - 0.6f * k) < 1e-4f) ++dot;
                else ++other;   // a dot's soft edge
            }
        check("pixelate: Pointillize -- dots of the colour on the background colour (white)", p && dot > 1000 && gap > 100 && other < dot / 4,
              num(dot) + " dots " + num(gap) + " gaps " + num(other) + " edges");
        check("pixelate: Pointillize -- alpha kept", p && p->get(100, 100).c[3] == 1.f, "");
        const P::Rect view{10, 30, 80, 90};
        check("pixelate: Pointillize -- the preview's rectangle == the result there", same_in(*p, *F::filtered(flat, po, nullptr, false, &view), view), "");
    }
    {
        // Fragment: one white dot on black -- four quarter-bright copies 4 px off diagonally.
        const auto dot = P::TileMap(f32(), 60, 60).fill({0, 0, 60, 60}, P::rgba(0, 0, 0, 1)).fill({30, 30, 1, 1}, P::rgba(1, 1, 1, 1));
        const auto fr = F::filtered(dot, F::fresh(FilterKind::Fragment), nullptr);
        bool four = fr.has_value();
        for (const auto& o : {std::array<int, 2>{4, 4}, {-4, 4}, {4, -4}, {-4, -4}})
            four = four && std::abs(fr->get(30 + o[0], 30 + o[1]).c[0] - 0.25f) < 1e-5f;
        check("pixelate: Fragment -- four copies at a quarter, the dot itself gone", four && fr->get(30, 30).c[0] == 0.f && fr->get(34, 30).c[0] == 0.f, "");
    }
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        auto cr = F::fresh(FilterKind::Crystallize); cr.radius = 33; cr.seed = 77;
        auto po = F::fresh(FilterKind::Pointillize); po.radius = 12; po.seed = 78; po.c2 = {0.1f, 0.2f, 0.3f};
        auto mo = F::fresh(FilterKind::Mosaic); mo.radius = 40;
        check("pixelate: live Mosaic, Crystallize, Pointillize, Fragment",
              L::add_live(j, d, "L4", mo) && L::add_live(j, d, "L4", cr) && L::add_live(j, d, "L4", po) &&
                  L::add_live(j, d, "L4", F::fresh(FilterKind::Fragment)), "");
        std::string err;
        check("pixelate: cell sizes, seeds and Pointillize's colour back from the file", saved_back(d, "p", err), err);
        check("pixelate: the look is made (live)", core::live::look(*d.get("L4")) != nullptr, "");
    }

    std::cout << "\n-- filters: Pixelate -- Colour Halftone, Facet, Mezzotint (s043 m2) --\n";
    {
        const auto ch = F::fresh(FilterKind::ColourHalftone), me = F::fresh(FilterKind::Mezzotint);
        check("pixelate2: Photoshop's defaults (Max. Radius 8; screens 108, 162, 90, 45; Fine Dots), under Pixelate",
              ch.radius == 8 && ch.screen == (std::array<double, 4>{108, 162, 90, 45}) && me.axis == 0 &&
                  F::group(FilterKind::ColourHalftone) == "Pixelate" && F::group(FilterKind::Facet) == "Pixelate" && F::group(FilterKind::Mezzotint) == "Pixelate", "");
        auto t = ch; t.radius = 500; t.screen[1] = 999; auto u = me; u.axis = 40; u.seed = 6; auto g = F::fresh(FilterKind::GaussianBlur); g.screen[0] = 3;
        check("pixelate2: normalised -- Max. Radius 4..127, angles -360..360, Mezzotint's 10 types, its seed kept; screens back on another kind",
              F::normalised(t).radius == 127 && F::normalised(t).screen[1] == 360 && F::normalised(u).axis == 9 && F::normalised(u).seed == 6 &&
                  F::normalised(g).screen[0] == 108, "");
        // The dot's radius: none at 0, half a cell at pi/4, the corners at 1; growing.
        bool grows = true;
        for (double v = 0.01; v <= 1.0; v += 0.01) grows = grows && F::halftone_radius(v) > F::halftone_radius(v - 0.01);
        check("pixelate2: halftone dot -- 0 at 0, half a cell at pi/4, the corners at 1, always growing",
              F::halftone_radius(0) == 0 && std::abs(F::halftone_radius(3.14159265358979 / 4) - 0.5) < 1e-6 &&
                  std::abs(F::halftone_radius(1) - std::sqrt(0.5)) < 1e-6 && grows, num(F::halftone_radius(1)));
        // On flat colour the ink's share matches the tone, channel by channel (within the dots' rounding).
        const auto flat = P::TileMap(f32(), 240, 240).fill({0, 0, 240, 240}, P::rgba(0.2f, 0.5f, 0.8f, 1));
        auto h = ch; h.radius = 10;
        const auto ht = F::filtered(flat, h, nullptr);
        std::array<double, 3> share{};
        bool binaryish = ht.has_value();
        int n = 0;
        for (int y = 40; y < 200; ++y)
            for (int x = 40; x < 200; ++x, ++n)
                for (int c = 0; c < 3; ++c) share[static_cast<std::size_t>(c)] += ht->get(x, y).c[static_cast<std::size_t>(c)];
        for (auto& v : share) v /= n;
        check("pixelate2: Colour Halftone -- each channel's ink about its tone (0.2, 0.5, 0.8)",
              binaryish && std::abs(share[0] - 0.2) < 0.05 && std::abs(share[1] - 0.5) < 0.05 && std::abs(share[2] - 0.8) < 0.05,
              num(share[0]) + " " + num(share[1]) + " " + num(share[2]));
        const auto white = P::TileMap(f32(), 100, 100).fill({0, 0, 100, 100}, P::rgba(1, 1, 1, 1));
        const auto black = P::TileMap(f32(), 100, 100).fill({0, 0, 100, 100}, P::rgba(0, 0, 0, 1));
        check("pixelate2: Colour Halftone -- white stays white, black stays black",
              same_in(white, *F::filtered(white, h, nullptr), {0, 0, 100, 100}) && same_in(black, *F::filtered(black, h, nullptr), {0, 0, 100, 100}), "");
        const P::Rect view{20, 200, 150, 30};
        const auto rh = F::filtered(rp, h, nullptr);
        check("pixelate2: Colour Halftone -- the preview's rectangle == the result there", rh && same_in(*rh, *F::filtered(rp, h, nullptr, false, &view), view), "");
    }
    {
        // Facet: a clean step stays clean (the calm side is picked); noise on a flat is flattened.
        const auto st = P::TileMap(f32(), 100, 60).fill({0, 0, 50, 60}, P::rgba(0.2f, 0.2f, 0.2f, 1)).fill({50, 0, 50, 60}, P::rgba(0.8f, 0.8f, 0.8f, 1));
        const auto fs = F::filtered(st, F::fresh(FilterKind::Facet), nullptr);
        check("pixelate2: Facet -- a clean edge stays where it was, sharp", fs && same_in(st, *fs, {0, 0, 100, 60}, 1e-6f), "");
        auto grain = P::TileMap(f32(), 100, 100);
        std::uint32_t sd = 9;
        double in2 = 0, out2 = 0;
        for (int y = 0; y < 100; ++y)
            for (int x = 0; x < 100; ++x) {
                sd = sd * 1664525u + 1013904223u;
                const float v = 0.5f + (static_cast<float>((sd >> 24) % 256) / 255.f - 0.5f) * 0.2f;
                grain = grain.fill({x, y, 1, 1}, P::rgba(v, v, v, 1));
            }
        const auto fg = F::filtered(grain, F::fresh(FilterKind::Facet), nullptr);
        for (int y = 10; y < 90; ++y)
            for (int x = 10; x < 90; ++x) {
                in2 += (grain.get(x, y).c[0] - 0.5) * (grain.get(x, y).c[0] - 0.5);
                out2 += (fg->get(x, y).c[0] - 0.5) * (fg->get(x, y).c[0] - 0.5);
            }
        check("pixelate2: Facet -- grain on a flat is calmed (spread under half)", fg && out2 < in2 * 0.25, num(std::sqrt(out2 / in2)));
    }
    {
        // Mezzotint: every channel 0 or 1; the share of 1 its tone, for each type.
        const auto flat = P::TileMap(f32(), 240, 240).fill({0, 0, 240, 240}, P::rgba(0.25f, 0.5f, 0.75f, 1));
        bool binary = true, shares = true;
        std::string why;
        for (int type = 0; type < 10; ++type) {
            auto me = F::fresh(FilterKind::Mezzotint);
            me.axis = type; me.seed = 3;
            const auto m = F::filtered(flat, me, nullptr);
            std::array<double, 3> share{};
            for (int y = 0; y < 240; ++y)
                for (int x = 0; x < 240; ++x)
                    for (int c = 0; c < 3; ++c) {
                        const float v = m->get(x, y).c[static_cast<std::size_t>(c)];
                        binary = binary && (v == 0.f || v == 1.f);
                        share[static_cast<std::size_t>(c)] += v;
                    }
            for (int c = 0; c < 3; ++c) {
                const double sh = share[static_cast<std::size_t>(c)] / (240.0 * 240.0), want = 0.25 * (c + 1);
                if (std::abs(sh - want) > 0.06) { shares = false; why += "type " + num(type) + " ch " + num(c) + " " + num(sh) + "; "; }
            }
        }
        check("pixelate2: Mezzotint -- every channel inked or not (0 / 1), all ten types", binary, "");
        check("pixelate2: Mezzotint -- each channel's ink about its tone, all ten types", shares, why);
        auto ln = F::fresh(FilterKind::Mezzotint); ln.axis = 6; ln.seed = 3;
        int runs = 0, same = 0;
        for (int y = 0; y < 50; ++y)
            for (int x = 1; x < 200; ++x, ++runs)
                if ((F::mezzotint_at(6, x, y, 0, 3) == F::mezzotint_at(6, x - 1, y, 0, 3))) ++same;
        check("pixelate2: Mezzotint Long Lines -- the pattern runs along the rows", same > runs * 0.9, num(same) + "/" + num(runs));
        const P::Rect view{30, 30, 70, 80};
        check("pixelate2: Mezzotint -- the preview's rectangle == the result there",
              same_in(*F::filtered(flat, ln, nullptr), *F::filtered(flat, ln, nullptr, false, &view), view), "");
    }
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        auto ch = F::fresh(FilterKind::ColourHalftone); ch.radius = 12; ch.screen = {15, 75, 0, 45};
        auto me = F::fresh(FilterKind::Mezzotint); me.axis = 7; me.seed = 91;
        check("pixelate2: live Colour Halftone, Facet, Mezzotint",
              L::add_live(j, d, "L4", ch) && L::add_live(j, d, "L4", F::fresh(FilterKind::Facet)) && L::add_live(j, d, "L4", me), "");
        std::string err;
        check("pixelate2: screens, type and seed back from the file", saved_back(d, "q", err), err);
    }

    std::cout << "\n-- filters: Render -- Fibers (s043 m3) --\n";
    {
        auto fb = F::fresh(FilterKind::Fibers);
        check("fibers: under Render; Variance 16, Strength 4; takes the rail's colours; seeded",
              F::group(FilterKind::Fibers) == "Render" && fb.amount == 16 && fb.ridges == 4 && F::coloured(FilterKind::Fibers) && F::seeded(FilterKind::Fibers), "");
        auto t = fb; t.amount = 99; t.ridges = 0; t.seed = 12;
        check("fibers: normalised -- Variance and Strength 1..64, the seed kept",
              F::normalised(t).amount == 64 && F::normalised(t).ridges == 1 && F::normalised(t).seed == 12, "");
        // Fibres run down the picture: a pixel is far more like the one below it than the one beside it.
        double down = 0, across = 0, s1 = 0, s2 = 0;
        int n = 0;
        bool in01 = true;
        for (int y = 0; y < 300; y += 3)
            for (int x = 0; x < 300; x += 3, ++n) {
                const float v = F::fibers_at(x, y, 16, 4, 7);
                in01 = in01 && v >= 0 && v <= 1;
                down += std::abs(F::fibers_at(x, y + 1, 16, 4, 7) - v);
                across += std::abs(F::fibers_at(x + 1, y, 16, 4, 7) - v);
                s1 += v; s2 += v * v;
            }
        const double sd = std::sqrt(std::max(0.0, s2 / n - (s1 / n) * (s1 / n)));
        check("fibers: values 0..1 with spread (sd > 0.1)", in01 && sd > 0.1, num(sd));
        check("fibers: streaks run down -- a pixel's neighbour below differs < a third of its neighbour beside", down * 3 < across, num(down / n) + " vs " + num(across / n));
        // Higher variance: the colour changes sooner along a fibre.
        double lo = 0, hi = 0;
        for (int y = 0; y < 300; y += 2)
            for (int x = 0; x < 100; x += 5) {
                lo += std::abs(F::fibers_at(x, y + 1, 2, 4, 7) - F::fibers_at(x, y, 2, 4, 7));
                hi += std::abs(F::fibers_at(x, y + 1, 48, 4, 7) - F::fibers_at(x, y, 48, 4, 7));
            }
        check("fibers: higher Variance -- shorter streaks", hi > lo * 2, num(lo) + " vs " + num(hi));
        fb.seed = 7; fb.c1 = {1, 0, 0}; fb.c2 = {0, 0, 1};
        const auto clear = P::TileMap(f32(), 120, 90);
        const auto r = F::filtered(clear, fb, nullptr);
        bool filled = r.has_value();
        for (int y = 0; y < 90 && filled; y += 7)
            for (int x = 0; x < 120 && filled; x += 7) {
                const auto p = r->get(x, y);
                const float v = F::fibers_at(x, y, 16, 4, 7);
                filled = p.c[3] == 1.f && std::abs(p.c[0] - (1 - v)) < 1e-5f && std::abs(p.c[2] - v) < 1e-5f;
            }
        check("fibers: a clear layer filled -- red to blue by the fibres, opaque", filled, "");
        const P::Rect view{10, 20, 60, 50};
        check("fibers: the preview's rectangle == the result there", same_in(*r, *F::filtered(clear, fb, nullptr, false, &view), view), "");
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        auto lv = fb; lv.amount = 30; lv.ridges = 50; lv.seed = 123;
        check("fibers: live", L::add_live(j, d, "L4", lv), "");
        std::string err;
        check("fibers: Variance, Strength, colours and seed back from the file", saved_back(d, "f", err), err);
    }

    std::cout << "\n-- filters: Distort -- Displace (s043 m4) --\n";
    {
        auto dp = F::fresh(FilterKind::Displace);
        check("displace: under Distort; scales 10 %, Stretch to Fit, Repeat Edge; not livable",
              F::group(FilterKind::Displace) == "Distort" && dp.scale_h == 10 && dp.scale_v == 10 && dp.axis == 0 && !dp.wrap &&
                  !F::livable(FilterKind::Displace) && F::livable(FilterKind::Mosaic), "");
        auto t = dp; t.scale_h = 5000; t.scale_v = -5000; t.axis = 7; t.map_layer = "L2";
        auto g = F::fresh(FilterKind::GaussianBlur); g.map_layer = "L2";
        check("displace: normalised -- scales -999..999, the map kept; another kind drops it",
              F::normalised(t).scale_h == 999 && F::normalised(t).scale_v == -999 && F::normalised(t).axis == 1 && F::normalised(t).map_layer == "L2" &&
                  F::normalised(g).map_layer.empty() && F::wants_map(t) && !F::wants_map(dp), "");
        const int W = 200, H = 100;
        P::TileMap rpw(f32(), W, H);   // red with x
        for (int x = 0; x < W; ++x) rpw = rpw.fill({x, 0, 1, H}, P::rgba(static_cast<float>(x) / W, 0.5f, 0.5f, 1));
        const auto grey = P::TileMap(f32(), W, H).fill({0, 0, W, H}, P::rgba(0.5f, 0.5f, 0.5f, 1));
        const auto white = P::TileMap(f32(), W, H).fill({0, 0, W, H}, P::rgba(1, 1, 1, 1));
        dp.map_layer = "map";
        check("displace: no map, or a mid-grey one -- nothing moves",
              same_in(rpw, *F::filtered(rpw, dp, nullptr), {0, 0, W, H}) && same_in(rpw, *F::filtered(rpw, dp, nullptr, false, nullptr, {}, {}, &grey), {0, 0, W, H}, 1e-5f), "");
        const auto moved = F::filtered(rpw, dp, nullptr, false, nullptr, {}, {}, &white);
        // White, 10 %: the picture 12.8 px right -- the pixel at x shows what was at x - 12.8.
        check("displace: a white map at 10 % -- the picture 12.8 px right (and down)",
              moved && std::abs(moved->get(100, 50).c[0] - (100 - 12.8f) / W) < 1e-4f && std::abs(moved->get(60, 50).c[0] - (60 - 12.8f) / W) < 1e-4f,
              moved ? num(moved->get(100, 50).c[0] * W) : "");
        check("displace: Repeat Edge -- the left edge's colour fills in", moved && moved->get(5, 50).c[0] == 0.f, "");
        auto wr = dp; wr.wrap = true;
        const auto wrapped = F::filtered(rpw, wr, nullptr, false, nullptr, {}, {}, &white);
        check("displace: Wrap Around -- the right edge comes round", wrapped && wrapped->get(5, 50).c[0] > 0.9f, wrapped ? num(wrapped->get(5, 50).c[0]) : "");
        auto neg = dp; neg.scale_h = -10;
        const auto back = F::filtered(rpw, neg, nullptr, false, nullptr, {}, {}, &white);
        check("displace: a negative scale -- the other way", back && std::abs(back->get(100, 50).c[0] - (100 + 12.8f) / W) < 1e-4f, "");
        // A map black left, white right, only its middle drawn (the rest clear): Stretch to Fit spreads its content over the picture.
        const auto half = P::TileMap(f32(), W, H).fill({50, 0, 50, H}, P::rgba(0, 0, 0, 1)).fill({100, 0, 50, H}, P::rgba(1, 1, 1, 1));
        const auto st = F::filtered(rpw, dp, nullptr, false, nullptr, {}, {}, &half);
        check("displace: Stretch to Fit -- the map's content over the whole picture (left half left, right half right)",
              st && st->get(40, 50).c[0] > rpw.get(40, 50).c[0] + 0.05f && st->get(160, 50).c[0] < rpw.get(160, 50).c[0] - 0.05f, "");
        auto tl = dp; tl.axis = 1;
        const auto ti = F::filtered(rpw, tl, nullptr, false, nullptr, {}, {}, &half);
        // Tile: the content (100 px wide, from x 50) repeated from the image's corner -- x 0..49 black, 50..99 white, 100..149 black...
        check("displace: Tile -- the content at its size from the corner",
              ti && ti->get(25, 50).c[0] > rpw.get(25, 50).c[0] + 0.05f && ti->get(75, 50).c[0] < rpw.get(75, 50).c[0] - 0.05f &&
                  ti->get(125, 50).c[0] > rpw.get(125, 50).c[0] + 0.05f, "");
        const P::Rect view{20, 10, 90, 60};
        check("displace: the preview's rectangle == the result there",
              same_in(*moved, *F::filtered(rpw, dp, nullptr, false, &view, {}, {}, &white), view), "");
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        check("displace: never live -- add_live and a filter layer refuse it",
              !L::add_live(j, d, "L4", dp) && L::add_filter_layer(j, d, dp, "L4").empty(), "");
    }
}
