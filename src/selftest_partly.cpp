// klimt_selftest -- PAINTING A LIVE LAYER (s021, WEAKNESSES 38): a look made
// in part (the change and its reach) is the look made whole, and shares the
// tiles the change can't reach.
#include "core/Document.hpp"
#include "core/Live.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
// A busy picture: bands and blobs, some transparent, w x h.
P::TileMap picture(int w, int h, P::Depth depth) {
    P::Format f;
    f.depth = depth;
    P::TileMap m(f, w, h);
    return m.paint({0, 0, w, h}, [](int x, int y, const P::Px&) {
        const float a = ((x / 37 + y / 53) % 5 == 0) ? 0.f : 1.f;
        return P::rgba(0.5f + 0.5f * std::sin(x * 0.05f), 0.5f + 0.5f * std::cos(y * 0.031f), ((x ^ y) & 255) / 255.f, a);
    });
}
// The largest channel difference between two maps (straight values x alpha).
double diff(const P::TileMap& a, const P::TileMap& b) {
    double worst = 0;
    for (int y = 0; y < a.height(); y += 3)
        for (int x = 0; x < a.width(); x += 3) {
            const auto p = a.get(x, y), q = b.get(x, y);
            for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(double(p.c[k] * p.c[3]) - double(q.c[k] * q.c[3])));
            worst = std::max(worst, std::abs(double(p.c[3]) - double(q.c[3])));
        }
    return worst;
}
core::Layer layer_of(std::shared_ptr<const P::TileMap> px) {
    core::Layer l;
    l.kind = core::Kind::Pixel;
    l.name = "L";
    l.pixels = std::move(px);
    return l;
}
double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
}  // namespace

void selftest_partly(const Check& check) {
    std::cout << "\n-- painting a live layer: the look in part (s021) --\n";
    const int W = 2600, H = 1900;
    const auto base = std::make_shared<const P::TileMap>(picture(W, H, P::Depth::U8));
    const auto dab = [&](const P::TileMap& m, int x, int y) {
        return std::make_shared<const P::TileMap>(m.fill({x, y, 40, 30}, P::rgba(1, 0, 0, 1)));
    };
    struct Case { const char* name; std::vector<core::LiveFilter> stack; core::Styles st; bool mask; };
    std::vector<Case> cases;
    {
        Case c{"gaussian 12", {{core::filter::fresh(core::FilterKind::GaussianBlur), true}}, {}, false};
        c.stack[0].f.radius = 12;
        cases.push_back(c);
    }
    {
        Case c{"median 3 + box 5", {{core::filter::fresh(core::FilterKind::Median), true}, {core::filter::fresh(core::FilterKind::BoxBlur), true}}, {}, false};
        c.stack[0].f.radius = 3;
        c.stack[1].f.radius = 5;
        cases.push_back(c);
    }
    {
        Case c{"styles: drop shadow + stroke", {}, {}, false};
        c.st.drop.on = true;
        c.st.stroke.on = true;
        c.st.stroke.size = 6;
        cases.push_back(c);
    }
    {
        Case c{"blur + filter mask + inner shadow", {{core::filter::fresh(core::FilterKind::GaussianBlur), true}}, {}, true};
        c.stack[0].f.radius = 6;
        c.st.inner.on = true;
        cases.push_back(c);
    }
    for (const auto& c : cases) {
        core::live::clear_cache();
        auto l = layer_of(base);
        l.filters = c.stack;
        l.styles = c.st;
        if (c.mask) {
            const P::Format mf{P::Depth::U8, P::Mode::Gray, false};
            l.filter_mask = std::make_shared<const P::TileMap>(P::TileMap(mf, W, H).fill({0, 0, W / 2, H}, P::Px{{1, 0, 0, 0, 0}}));
        }
        const auto t0 = std::chrono::steady_clock::now();
        const auto first = core::live::look(l);
        const double whole_ms = ms_since(t0);
        // A dab in the middle, then another next to it: each a look in part.
        const int before = core::live::partly_count();
        auto l2 = l;
        l2.pixels = dab(*base, 1300, 950);
        const auto t1 = std::chrono::steady_clock::now();
        const auto part = core::live::look(l2);
        const double part_ms = ms_since(t1);
        auto l3 = l2;
        l3.pixels = dab(*l2.pixels, 1330, 960);
        const auto part2 = core::live::look(l3);
        const bool in_part = core::live::partly_count() == before + 2;
        core::live::clear_cache();
        const auto full = core::live::look(l3);
        const double d = diff(*part2, *full);
        check(std::string("in part: ") + c.name + " -- made in part", in_part, "");
        check(std::string("in part: ") + c.name + " -- the same as whole", d <= 1.5 / 255, std::to_string(d * 255) + " /255");
        // Far from the dab, the tiles are the first look's own.
        check(std::string("in part: ") + c.name + " -- far tiles shared", part->same_tile(*first, 0, 0) && part->same_tile(*first, part->cols() - 1, part->rows() - 1), "");
        std::cout << "        whole " << static_cast<int>(whole_ms) << " ms, in part " << static_cast<int>(part_ms) << " ms\n";
    }
    // Add Noise depends on where it is: always whole.
    {
        core::live::clear_cache();
        auto l = layer_of(base);
        l.filters = {{core::filter::fresh(core::FilterKind::AddNoise), true}};
        core::live::look(l);
        const int before = core::live::partly_count();
        auto l2 = l;
        l2.pixels = dab(*base, 100, 100);
        core::live::look(l2);
        check("in part: never with Add Noise", core::live::partly_count() == before, "");
    }
    // A change over most of the layer: whole.
    {
        core::live::clear_cache();
        auto l = layer_of(base);
        l.filters = {{core::filter::fresh(core::FilterKind::BoxBlur), true}};
        core::live::look(l);
        const int before = core::live::partly_count();
        auto l2 = l;
        l2.pixels = std::make_shared<const P::TileMap>(base->fill({0, 0, W, H}, P::rgba(0, 1, 0, 1)));
        core::live::look(l2);
        check("in part: not when most of it changed", core::live::partly_count() == before, "");
    }
    core::live::clear_cache();
}
