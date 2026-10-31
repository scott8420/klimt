// klimt_selftest -- the canvas (s009 m2): reframed maps, the resampler, and
// Crop / Canvas Size / Image Size as steps (the image a record of the store).
#include "core/Canvas.hpp"
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/Selection.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace C = klimt::core::canvas;
namespace S = klimt::core::select;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
struct Fix {
    core::Document d;
    core::DocJournal j{d};
    Fix() {
        d.set_door([this] { return j.open(); });
        core::layers::load_fixture(d);
    }
    P::Px at(const std::string& id, int x, int y) const {
        const auto l = d.get(id);
        return l && l->pixels ? l->pixels->get(x, y) : P::transparent();
    }
};
bool red(const P::Px& p) { return p.c[0] > 0.99f && p.c[1] < 0.01f && p.c[3] > 0.99f; }
bool clear(const P::Px& p) { return p.c[3] <= 0.f; }
std::string sz(const core::Document& d) { return std::to_string(d.image().width) + "x" + std::to_string(d.image().height); }
core::Layer background_layer(std::shared_ptr<const P::TileMap> px) {
    core::Layer l;
    l.name = "Background";
    l.pos = 1;
    l.pixels = std::move(px);
    return l;
}
}  // namespace

void selftest_canvas(const Check& check) {
    std::cout << "\n-- canvas: reframe and resample (s009) --\n";
    {
        const P::Format f{P::Depth::U8, P::Mode::RGB, true};
        P::TileMap m(f, 300, 200);
        m = m.paint({0, 0, 300, 200}, [](int x, int y, const P::Px&) {
            return P::rgba(static_cast<float>(x) / 299.f, static_cast<float>(y) / 199.f, 0.25f, 1);
        });
        const auto r = C::reframed(m, 700, 450, 300, 100, P::transparent());
        bool ok = r.width() == 700 && r.height() == 450;
        for (int y = 0; y < 450 && ok; y += 3)
            for (int x = 0; x < 700 && ok; x += 7) {
                const int sx = x - 300, sy = y - 100;
                const auto want = (sx >= 0 && sy >= 0 && sx < 300 && sy < 200) ? m.get(sx, sy) : P::transparent();
                ok = r.get(x, y) == want;
            }
        check("canvas: reframed puts src(x - ox, y - oy) on the new canvas", ok, "");
        const auto c = C::reframed(m, 100, 50, -120, -70, P::transparent());
        check("canvas: reframed smaller is a crop", c.width() == 100 && c.get(0, 0) == m.get(120, 70) && c.get(99, 49) == m.get(219, 119), "");

        const P::TileMap flat = P::TileMap(f, 400, 300).fill({0, 0, 400, 300}, P::rgba(0.2f, 0.6f, 0.4f, 1));
        const auto half = C::resampled(flat, 200, 150, C::Filter::Smooth);
        const auto dbl = C::resampled(flat, 800, 600, C::Filter::Smooth);
        const auto near = [](const P::Px& a, const P::Px& b) {
            for (int k = 0; k < 4; ++k) if (std::abs(a.c[static_cast<std::size_t>(k)] - b.c[static_cast<std::size_t>(k)]) > 1.5f / 255) return false;
            return true;
        };
        check("canvas: a flat colour stays that colour shrunk", half && near(half->get(0, 0), flat.get(0, 0)) && near(half->get(199, 149), flat.get(0, 0)), "");
        check("canvas: ...and grown", dbl && near(dbl->get(0, 0), flat.get(0, 0)) && near(dbl->get(799, 599), flat.get(0, 0)) &&
                                      dbl->width() == 800 && dbl->height() == 600, "");
        check("canvas: one colour resamples to uniform tiles", dbl && dbl->tile(1, 1) && dbl->tile(1, 1)->is_uniform(), "");
        // A red square on transparency: no dark fringe (premultiplied), the
        // edge between is red with partial alpha.
        const P::TileMap sq = P::TileMap(f, 200, 200).fill({50, 50, 100, 100}, P::rgba(1, 0, 0, 1));
        const auto s2 = C::resampled(sq, 133, 133, C::Filter::Smooth);
        bool fringe = false;
        for (int y = 0; y < 133 && s2; ++y)
            for (int x = 0; x < 133; ++x) {
                const auto p = s2->get(x, y);
                if (p.c[3] > 0.02f && (p.c[1] > 0.02f || p.c[0] < 0.97f)) fringe = true;
            }
        check("canvas: no dark fringe at a transparent edge", s2 && !fringe, "");
        check("canvas: transparency stays transparent", s2 && clear(s2->get(5, 5)) && s2->tile_count() > 0, "");
        // Nearest: a 2x pixel-art grow keeps hard pixels.
        P::TileMap ck(f, 4, 4);
        ck = ck.paint({0, 0, 4, 4}, [](int x, int y, const P::Px&) { return (x + y) % 2 ? P::rgba(0, 0, 0, 1) : P::rgba(1, 1, 1, 1); });
        const auto n2 = C::resampled(ck, 8, 8, C::Filter::Nearest);
        check("canvas: Nearest doubles each pixel", n2 && n2->get(0, 0) == ck.get(0, 0) && n2->get(1, 1) == ck.get(0, 0) &&
                                                    n2->get(2, 0) == ck.get(1, 0) && n2->get(7, 7) == ck.get(3, 3), "");
        bool cancelled = !C::resampled(flat, 900, 700, C::Filter::Smooth, [] { return true; });
        check("canvas: a cancelled resample is nothing", cancelled, "");
        check("canvas: anchors", C::anchor_offset(100, 300, 0) == 0 && C::anchor_offset(100, 300, 1) == 100 &&
                                 C::anchor_offset(100, 300, 2) == 200 && C::anchor_offset(300, 100, 1) == -100, "");
    }

    {
        // s010 m4: the canvas's mips -- a 2 x 2 mean of premultiplied ARGB.
        std::vector<std::uint32_t> in = {0xff000000u, 0xffffffffu, 0x00000000u, 0x80800000u,
                                         0xff000000u, 0xffffffffu, 0x00000000u, 0x80800000u,
                                         0xff102030u, 0xff102030u, 0xff102030u, 0x00000000u};   // 4 x 3
        std::vector<std::uint32_t> out;
        int ow = 0, oh = 0;
        core::argb_half(in, 4, 3, out, ow, oh);
        check("canvas: a mip is half the size, rounded up", ow == 2 && oh == 2 && out.size() == 4, "");
        check("canvas: each mip pixel is its block's mean", out.size() == 4 && out[0] == 0xff808080u && out[1] == 0x40400000u &&
                                                           out[2] == 0xff102030u && (out[3] >> 24) == 0x80u,
              out.size() == 4 ? std::to_string(out[1]) : "");
    }

    std::cout << "\n-- canvas: Crop (s009) --\n";
    {
        Fix t;
        // "A" (L4) holds a red square at (100, 100) 200 x 150; Background is white.
        check("canvas: crop", C::crop(t.j, t.d, {50, 40, 400, 300}, P::rgba(1, 1, 1, 1)), "");
        check("canvas: the image is the crop's size", t.d.image().width == 400 && t.d.image().height == 300, sz(t.d));
        check("canvas: the pixels came with it", red(t.at("L4", 50, 60)) && clear(t.at("L4", 49, 60)) && red(t.at("L4", 249, 209)), "");
        check("canvas: every layer at the new size", t.d.check().empty(), t.d.check());
        check("canvas: one step, Crop", t.j.size() == 1 && t.j.undo_label() == "Crop", t.j.undo_label());
        t.j.undo();
        check("canvas: undo gives the size back", t.d.image().width == 600 && t.d.image().height == 400 && t.d.check().empty(), sz(t.d));
        check("canvas: ...and the pixels", red(t.at("L4", 100, 100)), "");
        t.j.redo();
        check("canvas: redo crops again", t.d.image().width == 400 && red(t.at("L4", 50, 60)) && t.d.check().empty(), sz(t.d));
        check("canvas: nothing written outside the door", t.d.strays() == 0, std::to_string(t.d.strays()));
    }
    {
        Fix t;
        core::layers::load_selection(t.d, {150, 120, 100, 80});
        C::crop(t.j, t.d, {150, 120, 100, 80}, P::rgba(1, 1, 1, 1));
        check("canvas: a crop deselects", t.d.selection() == nullptr && t.d.check().empty(), t.d.check());
        t.j.undo();
        check("canvas: undo brings the selection back", t.d.selection() != nullptr, "");
        check("canvas: cropping to the whole canvas keeps no step", !C::crop(t.j, t.d, {0, 0, 600, 400}, P::rgba(1, 1, 1, 1)), "");
        check("canvas: a 0-wide crop is refused", !C::crop(t.j, t.d, {10, 10, 0, 40}, P::rgba(1, 1, 1, 1)), "");
    }
    {
        Fix t;
        C::crop(t.j, t.d, {-100, 0, 800, 400}, P::rgba(1, 1, 1, 1));
        check("canvas: a crop past the edge adds canvas", t.d.image().width == 800 && clear(t.at("L1", 50, 10)) && t.at("L1", 150, 10).c[0] > 0.99f, sz(t.d));
    }

    {
        const P::Format mf{P::Depth::U8, P::Mode::Gray, false};
        const auto e = S::ellipse_mask(mf, 600, 400, 100.5, 50.5, 300.5, 250.5);
        const auto b = C::bounds(e);
        check("canvas: a selection's exact bounds", b.x == 100 && b.y == 50 && b.w == 201 && b.h == 201,
              std::to_string(b.x) + "," + std::to_string(b.y) + " " + std::to_string(b.w) + "x" + std::to_string(b.h));
        check("canvas: nothing selected, no bounds", C::bounds(P::TileMap(mf, 600, 400)).empty(), "");
        const auto all = S::all_mask(mf, 600, 400);
        check("canvas: all selected, the canvas", C::bounds(all) == P::Rect{0, 0, 600, 400}, "");
    }

    std::cout << "\n-- canvas: Canvas Size (s009) --\n";
    {
        Fix t;
        core::layers::load_selection(t.d, {10, 10, 100, 100});
        check("canvas: canvas size", C::canvas_size(t.j, t.d, 800, 300, 1, 2, P::rgba(1, 1, 1, 1)), "");
        // Centre across: +100; bottom: -100.
        check("canvas: anchored centre / bottom", t.d.image().width == 800 && t.d.image().height == 300 &&
                                                  red(t.at("L4", 200, 0)) && red(t.at("L4", 399, 149)) && clear(t.at("L4", 400, 100)), sz(t.d));
        check("canvas: the new canvas is transparent", clear(t.at("L1", 50, 50)) && t.at("L1", 150, 50).c[0] > 0.99f, "");
        check("canvas: the selection moved with the image", t.d.selection() && S::coverage(*t.d.selection(), 115, 5) > 0.99f &&
                                                           S::coverage(*t.d.selection(), 15, 5) < 0.01f, "");
        check("canvas: the document holds", t.d.check().empty(), t.d.check());
        t.j.undo();
        check("canvas: undo", t.d.image().width == 600 && t.d.check().empty() && red(t.at("L4", 100, 100)), sz(t.d));
        check("canvas: the same size keeps no step", !C::canvas_size(t.j, t.d, 600, 400, 1, 1, P::rgba(1, 1, 1, 1)), "");
    }
    {
        // No alpha: the new canvas is the background colour.
        Fix t;
        const P::Format nf{P::Depth::U8, P::Mode::RGB, false};
        {
            core::Document::Loading load(t.d);
            t.d.replace({.width = 100, .height = 100, .format = nf},
                        {{"L1", background_layer(std::make_shared<const P::TileMap>(P::TileMap(nf, 100, 100).fill({0, 0, 100, 100}, P::rgba(1, 0, 0))))}});
        }
        C::canvas_size(t.j, t.d, 200, 100, 0, 0, P::rgba(0, 0, 1, 1));
        check("canvas: no alpha -- the new canvas is the background colour", t.at("L1", 150, 50).c[2] > 0.99f && t.at("L1", 50, 50).c[0] > 0.99f, "");
    }

    std::cout << "\n-- canvas: Image Size (s009) --\n";
    {
        Fix t;
        core::layers::load_selection(t.d, {0, 0, 300, 400});
        check("canvas: image size", C::image_size(t.j, t.d, 300, 200, 144), "");
        check("canvas: the new size and resolution", t.d.image().width == 300 && t.d.image().height == 200 && t.d.image().dpi == 144, sz(t.d));
        check("canvas: the square scaled with it", red(t.at("L4", 75, 62)) && clear(t.at("L4", 30, 30)) && clear(t.at("L4", 160, 100)), "");
        check("canvas: the selection scaled too", t.d.selection() && S::coverage(*t.d.selection(), 70, 100) > 0.99f &&
                                                 S::coverage(*t.d.selection(), 220, 100) < 0.01f, "");
        check("canvas: the document holds", t.d.check().empty(), t.d.check());
        check("canvas: one step, Image Size", t.j.size() == 1 && t.j.undo_label() == "Image Size", t.j.undo_label());
        t.j.undo();
        check("canvas: undo", t.d.image().width == 600 && t.d.image().dpi == 72 && t.d.check().empty(), sz(t.d));
    }
    {
        Fix t;
        const auto px = t.d.get("L4")->pixels;
        check("canvas: the resolution alone", C::image_size(t.j, t.d, 600, 400, 300) && t.d.image().dpi == 300, "");
        check("canvas: ...touches no pixel", t.d.get("L4")->pixels == px, "");
        check("canvas: nothing changed keeps no step", !C::image_size(t.j, t.d, 600, 400, 300), "");
    }
    {
        // A plan made, then the document moved: the landing is refused.
        Fix t;
        const auto b = C::before(t.d);
        const auto p = C::size_plan(b, 300, 200, 72, C::Filter::Smooth);
        core::layers::fill(t.j, t.d, "L5", {0, 0, 10, 10}, P::rgba(0, 1, 0, 1));
        check("canvas: a stale plan is refused", p && !C::land(t.j, t.d, b, *p, "Image Size") && t.d.image().width == 600, "");
    }
    {
        // s047 (WEAKNESSES 27): the layers resampled on all the cores at once --
        // each exactly what it is resampled alone; progress only climbs, to 1;
        // a cancel gives nothing.
        Fix t;
        const auto b = C::before(t.d);
        std::vector<double> told;
        const auto p = C::size_plan(b, 250, 170, 72, C::Filter::Smooth, {}, [&](double v) { told.push_back(v); });
        bool same = p.has_value() && p->pixels.size() == b.pixels.size();
        for (const auto& [id, src] : b.pixels) {
            if (!same || !src) continue;
            const auto alone = C::resampled_layer(*src, 250, 170, C::Filter::Smooth);
            const auto& got = p->pixels.at(id);
            for (int y = -4; same && alone && got && y < 174; y += 3)
                for (int x = -4; x < 254; x += 5) if (got->get(x, y).c != alone->get(x, y).c) { same = false; break; }
        }
        check("canvas: Image Size over the cores: each layer as if alone", same, "");
        check("canvas: its progress only climbs, to the end", !told.empty() && std::is_sorted(told.begin(), told.end()) && told.back() > 0.999,
              told.empty() ? "" : std::to_string(told.back()));
        int calls = 0;
        check("canvas: cancelled midway, it gives nothing",
              !C::size_plan(b, 250, 170, 72, C::Filter::Smooth, [&] { return ++calls > 3; }).has_value(), "");
    }
}
