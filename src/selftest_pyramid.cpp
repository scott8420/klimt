// klimt_selftest -- THE VIEW PYRAMID (s046). docs/speed.md. A reduced map is
// the premultiplied mean of the blocks it covers (a solid colour stays that
// colour; transparency averages as coverage, never as a darker colour); the
// edge counts only pixels inside the map; every depth agrees; the cache
// hands back the same reduction and lets go of what's gone; a one-layer
// document's reduced composite is the full composite's block mean.
#include "core/Composite.hpp"
#include "core/Document.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
std::string num(double v) { return std::to_string(v); }
P::Format fmt(P::Depth d) { return {d, P::Mode::RGB, true}; }
P::TileMap checker(P::Depth d, int w, int h) {   // red / clear, one pixel each
    P::TileMap m(fmt(d), w, h);
    return m.paint({0, 0, w, h}, [](int x, int y, const P::Px&) { return (x + y) % 2 ? P::rgba(1, 0, 0, 1) : P::transparent(); });
}
}  // namespace

void selftest_pyramid(const Check& check) {
    std::cout << "\n-- the view pyramid: reduced layers, the preview (s046 m3) --\n";
    {
        P::TileMap m(fmt(P::Depth::U8), 600, 500);
        m = m.fill({0, 0, 600, 500}, P::rgba(0.2f, 0.6f, 0.4f, 1));
        const auto r = core::reduced_map(m, 2);
        check("pyramid: 1/4 of 600 x 500 is 150 x 125", r.width() == 150 && r.height() == 125, num(r.width()) + " x " + num(r.height()));
        const auto p = r.get(77, 60);
        check("pyramid: a solid colour stays that colour", std::abs(p.c[0] - 0.2f) < 0.005f && std::abs(p.c[1] - 0.6f) < 0.005f && p.c[3] > 0.995f, "");
    }
    for (const auto d : {P::Depth::U8, P::Depth::U16, P::Depth::F32}) {
        const auto r = core::reduced_map(checker(d, 300, 300), 1);
        const auto p = r.get(40, 40);
        const std::string tag = d == P::Depth::U8 ? "8" : d == P::Depth::U16 ? "16" : "32";
        check("pyramid: red over clear averages to red at half coverage (" + tag + "-bit)",
              std::abs(p.c[0] - 1) < 0.01f && p.c[1] < 0.01f && std::abs(p.c[3] - 0.5f) < 0.01f, num(p.c[0]) + " a " + num(p.c[3]));
    }
    {
        P::TileMap m(fmt(P::Depth::U8), 601, 300);
        m = m.fill({0, 0, 601, 300}, P::rgba(0, 0, 1, 1));
        const auto r = core::reduced_map(m, 2);
        const auto e = r.get(150, 10);
        check("pyramid: an edge column holding one source column is as solid as it", r.width() == 151 && e.c[3] > 0.995f && e.c[2] > 0.995f,
              num(r.width()) + " a " + num(e.c[3]));
        const auto r6 = core::reduced_map(m, 6);
        check("pyramid: deep levels too (1/64)", r6.width() == 10 && r6.height() == 5 && r6.get(9, 4).c[3] > 0.995f, num(r6.width()));
    }
    {
        P::TileMap m(fmt(P::Depth::U8), 700, 400);   // a soft spot on clear: sparse, not uniform
        m = m.paint({100, 100, 300, 200}, [](int x, int y, const P::Px&) {
            const float a = std::clamp(1.f - std::hypot(x - 250.f, y - 200.f) / 150.f, 0.f, 1.f);
            return P::rgba(0.9f, 0.5f, 0.1f, a);
        });
        const auto r = core::reduced_map(m, 2);
        double want_a = 0, got_a = 0;
        for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) want_a += m.get(200 + x, 160 + y).c[3];
        want_a /= 16;
        got_a = r.get(50, 40).c[3];
        check("pyramid: a pixel's coverage is its block's mean", std::abs(want_a - got_a) < 0.01, num(want_a) + " vs " + num(got_a));
        check("pyramid: and nothing appears where the block was clear", r.get(5, 5).c[3] == 0 && !r.tile(2, 1), "");
    }
    {
        core::ReduceCache cache;
        auto a = std::make_shared<const P::TileMap>(checker(P::Depth::U8, 512, 512));
        const auto r1 = cache.get(a, 2), r2 = cache.get(a, 2);
        check("pyramid: the cache hands back the same reduction", r1 && r1 == r2 && cache.size() == 1, "");
        core::Layers keep;
        cache.prune(keep);
        check("pyramid: and lets go of pixels no layer holds", cache.size() == 0, num(static_cast<double>(cache.size())));
    }
    {   // one layer: the reduced composite IS the full composite's block mean
        core::Image im;
        im.width = 640;
        im.height = 520;
        im.format = fmt(P::Depth::U8);
        P::TileMap m(im.format, im.width, im.height);
        m = m.paint({0, 0, im.width, im.height}, [](int x, int y, const P::Px&) {
            return P::rgba(static_cast<float>(x % 97) / 97.f, static_cast<float>(y % 53) / 53.f, 0.3f, (x / 8 + y / 8) % 3 ? 1.f : 0.4f);
        });
        core::Layer l;
        l.name = "L";
        l.pos = 1;
        l.pixels = std::make_shared<const P::TileMap>(m);
        core::Layers ls{{"L1", l}};
        core::ReduceCache cache;
        const auto r = core::reduced(im, ls, 2, &cache);
        std::vector<std::uint32_t> small;
        const bool ok = r && core::composite_reduced_argb(*r, small);
        check("pyramid: the reduced document is the image's size divided", ok && r->image.width == 160 && r->image.height == 130 && small.size() == 160u * 130u, "");
        std::vector<std::uint32_t> full;
        int w = 0, h = 0;
        double worst = 0;
        core::composite_tile_argb(im, ls, 0, 0, full, w, h);
        for (int oy = 0; oy < 60 && ok; oy += 7)
            for (int ox = 0; ox < 60; ox += 5) {
                for (int ch = 0; ch < 4; ++ch) {
                    double mean = 0;
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x)
                            mean += (full[static_cast<std::size_t>((oy * 4 + y) * w + ox * 4 + x)] >> (ch * 8)) & 0xff;
                    mean /= 16;
                    const double got = (small[static_cast<std::size_t>(oy * 160 + ox)] >> (ch * 8)) & 0xff;
                    worst = std::max(worst, std::abs(mean - got));
                }
            }
        check("pyramid: one layer's preview is its full composite's block mean (within 2 / 255)", ok && worst <= 2.0, "worst " + num(worst));
        check("pyramid: the cache now holds that layer", cache.size() == 1, "");
    }
    {
        // s048: the layers reduced on all the cores -- each exactly its own
        // reduction; a map two layers share reduced once; a cancel gives nothing.
        core::Image im;
        im.width = 300; im.height = 200; im.format = fmt(P::Depth::U8);
        core::Layers ls;
        auto shared = std::make_shared<const P::TileMap>(checker(P::Depth::U8, 300, 200));
        for (int k = 0; k < 6; ++k) {
            core::Layer l;
            l.name = "L" + std::to_string(k);
            l.pos = k + 1;
            l.pixels = k == 5 ? shared : std::make_shared<const P::TileMap>(P::TileMap(fmt(P::Depth::U8), 300, 200).paint({0, 0, 300, 200},
                [k](int x, int y, const P::Px&) { return P::rgba(static_cast<float>((x * (k + 2)) % 256) / 255.f, static_cast<float>(y % 64) / 63.f, 0.2f, 1); }));
            if (k == 2) l.mask = shared;
            ls.emplace("L" + std::to_string(k), l);
        }
        core::ReduceCache cache;
        const auto r = core::reduced(im, ls, 2, &cache);
        bool same = r.has_value();
        for (const auto& [id, l] : ls) {
            if (!same) break;
            const auto mine = core::reduced_map(*l.pixels, 2);
            const auto& got = r->layers.at(id).pixels;
            for (int y = 0; same && y < mine.height(); y += 3)
                for (int x = 0; x < mine.width(); x += 4) if (got->get(x, y).c != mine.get(x, y).c) { same = false; break; }
        }
        check("pyramid: on all the cores, each layer is its own reduction", same, "");
        check("pyramid: a map two layers share is reduced once", cache.size() == 6 && r && r->layers.at("L2").mask == r->layers.at("L5").pixels, num(static_cast<double>(cache.size())));
        check("pyramid: cancelled, it gives nothing", !core::reduced(im, ls, 3, nullptr, [] { return true; }).has_value(), "");
    }
}
