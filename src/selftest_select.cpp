// klimt_selftest -- the selection (s008): masks, set arithmetic, feather,
// the ants' outline, pixels through a mask, and the verbs' steps.
#include "core/Document.hpp"
#include "core/Selection.hpp"
#include "core/Lasso.hpp"
#include "core/Composite.hpp"

#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace S = klimt::core::select;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
double area(const P::TileMap& m) {
    double a = 0;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x) a += S::coverage(m, x, y);
    return a;
}
const P::Format kMask{P::Depth::U8, P::Mode::Gray, false};

struct Fix {
    core::Document d;
    core::DocJournal j{d};
    Fix() {
        d.set_door([this] { return j.open(); });
        core::layers::load_fixture(d);
    }
};
}  // namespace

void selftest_select(const Check& check) {
    std::cout << "\n-- selection: shapes (s008) --\n";
    {
        const auto r = S::rect_mask(kMask, 600, 400, {50, 60, 300, 200});
        check("rect: inside 1, outside 0, edges hard", S::coverage(r, 50, 60) == 1.f && S::coverage(r, 349, 259) == 1.f &&
              S::coverage(r, 49, 60) == 0.f && S::coverage(r, 350, 100) == 0.f && area(r) == 300.0 * 200.0, "");
        const auto all = S::all_mask(kMask, 6600, 10200);
        check("all: a 600 dpi spread selected in a few KB (uniform tiles)", all.bytes() < 4096 && S::coverage(all, 6599, 10199) == 1.f,
              std::to_string(all.bytes()) + " bytes");
        const auto e = S::ellipse_mask(kMask, 600, 400, 100, 50, 500, 350);
        const double want = M_PI * 200 * 150, got = area(e);
        check("ellipse: area within 0.5% of pi*a*b", std::abs(got - want) / want < 0.005, std::to_string(got) + " vs " + std::to_string(want));
        int soft = 0;   // partly covered pixels along the rim
        for (int y = 0; y < 400; ++y) for (int x = 0; x < 600; ++x) { const float v = S::coverage(e, x, y); if (v > 0.05f && v < 0.95f) ++soft; }
        check("ellipse: the rim is anti-aliased", soft > 600, std::to_string(soft) + " soft pixels");
        check("ellipse: symmetric", std::abs(S::coverage(e, 120, 140) - S::coverage(e, 479, 140)) < 0.02f &&
              std::abs(S::coverage(e, 300, 52) - S::coverage(e, 299, 347)) < 0.02f, "");
        const auto big = S::ellipse_mask(kMask, 3000, 3000, 0, 0, 3000, 3000);
        check("ellipse: tiles wholly inside are uniform", big.tile(5, 5) && big.tile(5, 5)->is_uniform(), "");
        const auto sq = S::polygon_mask(kMask, 600, 400, {{50, 60}, {350, 60}, {350, 260}, {50, 260}});
        bool same = true;
        for (int y = 0; y < 400; y += 3) for (int x = 0; x < 600; x += 3) if (S::coverage(sq, x, y) != S::coverage(r, x, y)) same = false;
        check("lasso: an axis-aligned square is the rectangle, exactly", same, "");
        const auto tri = S::polygon_mask(kMask, 600, 400, {{10.5, 10.25}, {410.5, 10.25}, {10.5, 310.25}});
        check("lasso: a triangle's area (sub-pixel corners) within 0.2%", std::abs(area(tri) - 60000.0) / 60000.0 < 0.002, std::to_string(area(tri)));
        // A figure of eight: non-zero winding fills both loops.
        const auto eight = S::polygon_mask(kMask, 600, 400, {{0, 0}, {200, 200}, {0, 200}, {200, 0}});
        check("lasso: a crossed figure fills both lobes", S::coverage(eight, 100, 20) == 1.f && S::coverage(eight, 100, 180) == 1.f &&
              S::coverage(eight, 20, 100) == 0.f, "");
        check("lasso: fewer than three points selects nothing", S::empty(S::polygon_mask(kMask, 600, 400, {{0, 0}, {50, 50}})), "");
        const auto off = S::rect_mask(kMask, 600, 400, {700, 10, 50, 50});
        check("rect: wholly outside selects nothing", S::empty(off), "");
    }

    std::cout << "\n-- selection: arithmetic, invert, feather --\n";
    {
        const auto a = S::rect_mask(kMask, 600, 400, {0, 0, 300, 400});
        const auto b = S::rect_mask(kMask, 600, 400, {200, 0, 400, 400});
        const auto add = S::combine(&a, b, S::Op::Add), sub = S::combine(&a, b, S::Op::Subtract), in = S::combine(&a, b, S::Op::Intersect);
        check("add: the union", area(add) == 600.0 * 400.0, "");
        check("subtract: a without b", area(sub) == 200.0 * 400.0 && S::coverage(sub, 250, 10) == 0.f, "");
        check("intersect: both", area(in) == 100.0 * 400.0 && S::coverage(in, 250, 10) == 1.f, "");
        check("replace: just the shape", S::combine(&a, b, S::Op::Replace).tile(0, 0) == b.tile(0, 0), "");
        check("subtract from nothing: nothing", S::empty(S::combine(nullptr, b, S::Op::Subtract)), "");
        check("subtract everything: empty (no tiles held)", S::empty(S::combine(&a, S::all_mask(kMask, 600, 400), S::Op::Subtract)), "");
        const auto e = S::ellipse_mask(kMask, 600, 400, 100, 50, 500, 350);
        const auto ii = S::inverted(&e, kMask, 600, 400);
        const auto back = S::inverted(&ii, kMask, 600, 400);
        bool ok = true;
        for (int y = 0; y < 400; y += 2) for (int x = 0; x < 600; x += 2) if (S::coverage(back, x, y) != S::coverage(e, x, y)) ok = false;
        check("invert twice: the same selection", ok, "");
        check("invert: coverage is 1 - m", std::abs(S::coverage(ii, 101, 200) + S::coverage(e, 101, 200) - 1.f) < 0.005f, "");
        check("invert of nothing: everything", area(S::inverted(nullptr, kMask, 600, 400)) == 240000.0, "");
        const auto sq = S::rect_mask(kMask, 600, 400, {200, 100, 200, 200});
        const auto f = S::feathered(sq, 10);
        check("feather: the edge goes soft", f && S::coverage(*f, 200, 200) > 0.3f && S::coverage(*f, 200, 200) < 0.7f &&
              S::coverage(*f, 300, 200) > 0.99f && S::coverage(*f, 185, 200) < 0.1f, f ? std::to_string(S::coverage(*f, 200, 200)) : "");
        check("feather: the selected amount is kept (within 1%)", f && std::abs(area(*f) - area(sq)) / area(sq) < 0.01,
              f ? std::to_string(area(*f)) : "");
        check("feather: a cancel returns nothing", !S::feathered(sq, 10, [] { return true; }), "");
        const P::Format f16{P::Depth::U16, P::Mode::Gray, false}, f32{P::Depth::F32, P::Mode::Gray, false};
        check("masks at 16-bit and float work the same",
              area(S::ellipse_mask(f16, 600, 400, 100, 50, 500, 350)) - area(e) < 60 &&
              std::abs(area(S::ellipse_mask(f32, 600, 400, 100, 50, 500, 350)) - area(e)) < 60, "");
    }

    std::cout << "\n-- selection: the ants' outline --\n";
    {
        const auto r = S::rect_mask(kMask, 600, 400, {50, 60, 300, 200});
        const auto o = S::outline(r);
        std::set<std::tuple<int, int, int, int>> segs;
        for (const auto& s : o) segs.insert({s.x0, s.y0, s.x1, s.y1});
        check("outline: a rectangle is its four sides", o.size() == 4 && segs.count({50, 60, 350, 60}) && segs.count({50, 260, 350, 260}) &&
              segs.count({50, 60, 50, 260}) && segs.count({350, 60, 350, 260}), std::to_string(o.size()) + " segments");
        const auto all = S::outline(S::all_mask(kMask, 600, 400));
        check("outline: Select All is the image's border", all.size() == 4, std::to_string(all.size()));
        // Crossing tile borders: a rectangle over four tiles is still 4 runs.
        check("outline: runs merge across tile borders", S::outline(S::rect_mask(kMask, 600, 400, {100, 100, 300, 250})).size() == 4, "");
        // Closed: every corner of an ellipse's outline meets an even number of segment ends.
        std::map<std::pair<int, int>, int> ends;
        for (const auto& s : S::outline(S::ellipse_mask(kMask, 600, 400, 100, 50, 500, 350))) { ++ends[{s.x0, s.y0}]; ++ends[{s.x1, s.y1}]; }
        bool closed = !ends.empty();
        for (const auto& [p, n] : ends) if (n % 2) closed = false;
        check("outline: an ellipse's outline is closed", closed, std::to_string(ends.size()) + " corners");
    }

    std::cout << "\n-- selection: the verbs --\n";
    {
        Fix f;
        check("verbs: nothing selected at first", !f.d.selection(), "");
        check("verbs: Deselect with nothing selected keeps no step", !S::none(f.j, f.d) && f.j.size() == 0, "");
        S::rect(f.j, f.d, {10, 10, 100, 100});
        const auto first = f.d.selection_ptr();
        check("verbs: a marquee is a step, labelled", first && f.j.size() == 1 && f.j.undo_label() == "Rectangle Select", f.j.undo_label());
        check("verbs: the same marquee again keeps no step", !S::rect(f.j, f.d, {10, 10, 100, 100}) && f.j.size() == 1, "");
        S::rect(f.j, f.d, {200, 200, 50, 50}, S::Op::Add);
        check("verbs: Shift adds, labelled", f.j.undo_label() == "Add to Selection" && area(*f.d.selection()) == 100.0 * 100 + 50.0 * 50, f.j.undo_label());
        f.j.undo();
        check("verbs: undo puts the very same mask back", f.d.selection_ptr() == first, "");
        S::polygon(f.j, f.d, {{0, 0}, {5, 5}});
        check("verbs: an empty lasso deselects", !f.d.selection() && f.j.undo_label() == "Lasso", f.j.undo_label());
        f.j.undo();
        S::all(f.j, f.d);
        S::invert(f.j, f.d);
        check("verbs: inverting everything leaves nothing (none, not an empty mask)", !f.d.selection() && f.d.check().empty(), f.d.check());
        // A job's landing: refused when the selection moved meanwhile.
        const auto now = f.d.selection_ptr();
        S::rect(f.j, f.d, {0, 0, 10, 10});
        check("set_if: refused when the selection moved", !S::set_if(f.j, f.d, now, S::all_mask(f.d.mask_format(), 600, 400), "x"), "");
        // Opening another document drops the selection (Document::replace).
        core::Document::Loading load(f.d);
        f.d.replace(f.d.image(), f.d.snapshot(), f.d.next_id());
        check("replace: a new document starts with nothing selected", !f.d.selection(), "");
    }
    {
        // The mask follows the document's depth.
        core::Document d;
        core::Document::Loading load(d);
        d.set_image({.width = 300, .height = 200, .format = P::Format{P::Depth::U16, P::Mode::CMYK, true}});
        check("mask format: Gray at the document's depth, no alpha", d.mask_format() == P::Format{P::Depth::U16, P::Mode::Gray, false}, "");
    }

    std::cout << "\n-- selection: pixels through it --\n";
    {
        const P::Format f{P::Depth::U8, P::Mode::RGB, true};
        const P::TileMap layer = P::TileMap(f, 600, 400).fill({0, 0, 600, 400}, P::rgba(1, 0, 0, 1));
        const auto whole = S::filled(layer, nullptr, P::rgba(0, 0, 1, 1), false);
        check("fill, no selection: everything, as uniform tiles", whole.get(599, 399) == P::rgba(0, 0, 1, 1) && whole.bytes() < 64, "");
        const auto m = S::rect_mask(kMask, 600, 400, {0, 0, 300, 400});
        const auto half = S::filled(layer, &m, P::rgba(0, 0, 1, 1), false);
        check("fill through a mask: inside filled, outside shared", half.get(10, 10).c[2] == 1.f && half.get(400, 10).c[0] == 1.f &&
              half.tile(2, 0) == layer.tile(2, 0), "");
        const auto feath = *S::feathered(m, 20);
        const auto soft = S::filled(layer, &feath, P::rgba(0, 0, 1, 1), false);
        const auto p = soft.get(300, 100);
        check("fill through a soft edge: a blend, alpha stays full", p.c[0] > 0.3f && p.c[2] > 0.3f && p.c[3] == 1.f,
              std::to_string(p.c[0]) + "," + std::to_string(p.c[2]));
        // Alpha lock: colour only where there's something.
        const P::TileMap part = P::TileMap(f, 600, 400).fill({0, 0, 100, 100}, P::rgba(1, 0, 0, 0.5f));
        const auto locked = S::filled(part, nullptr, P::rgba(0, 1, 0, 1), true);
        check("fill, alpha locked: colour where there was something, alpha kept, empty stays empty",
              locked.get(10, 10).c[1] == 1.f && std::abs(locked.get(10, 10).c[3] - 0.5f) < 0.01f && locked.get(300, 300) == P::transparent(), "");
        const auto del = S::cleared(layer, &m, P::rgba(1, 1, 1, 1));
        check("delete: transparent inside (no tile), kept outside", del.get(10, 10) == P::transparent() && !del.tile(0, 0) && del.get(400, 10).c[3] == 1.f, "");
        const auto delsoft = S::cleared(layer, &feath, P::rgba(1, 1, 1, 1));
        check("delete through a soft edge: alpha *= 1 - m", std::abs(delsoft.get(300, 100).c[3] - (1 - S::coverage(feath, 300, 100))) < 0.01f, "");
        const P::Format jpeg{P::Depth::U8, P::Mode::RGB, false};
        const P::TileMap bg = P::TileMap(jpeg, 600, 400).fill({0, 0, 600, 400}, P::rgba(1, 0, 0));
        const auto onwhite = S::cleared(bg, &m, P::rgba(1, 1, 1));
        check("delete with no alpha (a JPEG): the background colour instead", onwhite.get(10, 10).c[1] == 1.f && onwhite.get(400, 10).c[1] == 0.f, "");
    }
    {
        Fix f;
        core::layers::load_selection(f.d, {100, 100, 200, 150});
        core::layers::clear_selection(f.j, f.d, "L4", P::rgba(1, 1, 1, 1));   // A: the red square, now cleared
        check("verbs: Delete in a layer, labelled", f.j.undo_label() == "Delete in “A”" && !f.d.get("L4")->pixels, f.j.undo_label());
        f.j.undo();
        auto l = *f.d.get("L4");
        l.locks = core::lock::Alpha;
        { core::Document::Loading load(f.d); f.d.put("L4", l); }
        check("verbs: Delete refused under an Alpha lock", !core::layers::clear_selection(f.j, f.d, "L4", P::rgba(1, 1, 1, 1)), "");
        check("verbs: Fill refused on a group", !core::layers::fill_selection(f.j, f.d, "L2", P::rgba(0, 0, 0, 1)), "");
        const auto px = f.d.get("L3")->pixels;
        core::layers::fill_selection(f.j, f.d, "L3", P::rgba(0, 1, 0, 1));
        check("set_pixels_if: a stale job's landing is refused",
              !core::layers::set_pixels_if(f.j, f.d, "L3", px, f.d.blank(), "stale"), "");
    }

    std::cout << "\n-- selection: the Magic Wand; the Eyedropper (s008 m3) --\n";
    {
        const P::Format f{P::Depth::U8, P::Mode::RGB, true};
        // Two red squares apart on white, and a near-red (within 32) strip
        // touching the first.
        auto m = P::TileMap(f, 600, 400).fill({0, 0, 600, 400}, P::rgba(1, 1, 1, 1));
        m = m.fill({20, 20, 100, 100}, P::rgba(1, 0, 0, 1));
        m = m.fill({400, 300, 50, 50}, P::rgba(1, 0, 0, 1));
        m = m.fill({120, 20, 30, 100}, P::rgba(0.9f, 0.05f, 0.05f, 1));   // 25 / 255 off
        const auto c = S::wand_mask(m, kMask, 50, 50, 32.f / 255, true);
        check("wand, contiguous: the square and its near-red neighbour, not the far square",
              c && area(*c) == 100.0 * 100 + 30.0 * 100 && S::coverage(*c, 420, 320) == 0.f, c ? std::to_string(area(*c)) : "");
        const auto tight = S::wand_mask(m, kMask, 50, 50, 10.f / 255, true);
        check("wand, tolerance 10: the square only", tight && area(*tight) == 100.0 * 100, "");
        const auto any = S::wand_mask(m, kMask, 50, 50, 0, false);
        check("wand, not contiguous: every exact red, far square too", any && area(*any) == 100.0 * 100 + 50.0 * 50, "");
        const auto white = S::wand_mask(m, kMask, 300, 200, 0, true);
        check("wand on the white: everything else (a big region across tiles)", white &&
              area(*white) == 600.0 * 400 - 100.0 * 100 - 50.0 * 50 - 30.0 * 100 && white->tile(2, 1) != nullptr, "");
        const auto empty = S::wand_mask(P::TileMap(f, 600, 400), kMask, 5, 5, 0, true);
        check("wand on an empty layer: all of it (transparent is a colour too)", empty && area(*empty) == 240000.0, "");
        check("wand: a cancel returns nothing", !S::wand_mask(m, kMask, 300, 200, 0, true, [] { return true; }), "");
    }
    {
        core::Document d;
        core::layers::load_fixture(d);   // Background white; A a red square at (100, 100) 200 x 150
        const auto a = core::sample_composite(d.image(), d.snapshot(), 150, 150);
        const auto b = core::sample_composite(d.image(), d.snapshot(), 10, 10);
        check("eyedropper: red where A is, white where it isn't", a.r > 0.99f && a.g < 0.01f && b.g > 0.99f && a.a == 1.f, "");
        const auto edge = core::sample_composite(d.image(), d.snapshot(), 100, 150, 1);   // 3 x 3 over A's left edge
        check("eyedropper 3 x 3: an average across the edge", std::abs(edge.g - 1.f / 3) < 0.01f, std::to_string(edge.g));
        check("eyedropper: outside the image is nothing", core::sample_composite(d.image(), d.snapshot(), -5, 10).a == 0.f, "");
    }
    {
        // from_srgb is the inverse of how the compositor shows each mode.
        bool ok = true;
        std::string bad;
        for (const auto mode : {P::Mode::RGB, P::Mode::CMYK, P::Mode::Lab}) {
            const P::Format f{P::Depth::F32, mode, true};
            for (const auto& rgb : {std::array<float, 3>{0.8f, 0.3f, 0.1f}, std::array<float, 3>{0.1f, 0.6f, 0.9f},
                                    std::array<float, 3>{1, 1, 1}, std::array<float, 3>{0, 0, 0}}) {
                core::Image im{.width = 4, .height = 4, .format = f};
                core::Layers ls;
                core::Layer l;
                l.name = "x";
                l.pos = 1;
                l.pixels = std::make_shared<const P::TileMap>(P::TileMap(f, 4, 4).fill({0, 0, 4, 4}, core::from_srgb(f, rgb[0], rgb[1], rgb[2])));
                ls["L1"] = l;
                const auto s2 = core::sample_composite(im, ls, 1, 1);
                if (std::abs(s2.r - rgb[0]) > 0.005f || std::abs(s2.g - rgb[1]) > 0.005f || std::abs(s2.b - rgb[2]) > 0.005f) {
                    ok = false;
                    bad += std::to_string(static_cast<int>(mode)) + ":" + std::to_string(s2.r) + "," + std::to_string(s2.g) + "," + std::to_string(s2.b) + " ";
                }
            }
        }
        check("from_srgb: RGB, CMYK and Lab come back as the colour asked", ok, bad);
    }
}

// ── s008 m4: the brush ──────────────────────────────────────────────────────
#include "core/Paint.hpp"

void selftest_paint(const Check& check) {
    namespace Pa = klimt::core::paint;
    std::cout << "\n-- paint: the brush (s008 m4) --\n";
    {
        double carry = 10;
        const auto d1 = Pa::dabs_along({0, 0}, {25, 0}, 10, carry);
        check("dabs: from the start, every spacing", d1.size() == 3 && d1[0].x == 0 && d1[2].x == 20 && std::abs(carry - 5) < 1e-9,
              std::to_string(d1.size()) + " carry " + std::to_string(carry));
        const auto d2 = Pa::dabs_along({25, 0}, {32, 0}, 10, carry);
        check("dabs: the leftover carries (next dab at 30)", d2.size() == 1 && std::abs(d2[0].x - 30) < 1e-9, "");
        const auto d3 = Pa::dabs_along({32, 0}, {35, 0}, 10, carry);
        check("dabs: a short move lays none and carries on", d3.empty() && std::abs(carry - 5) < 1e-9, std::to_string(carry));
        Pa::Brush b;
        b.size = 20;
        b.hardness = 1;
        check("dab: hard -- full inside, nothing outside, a 1 px rim", Pa::dab_alpha(5, b) == 1.f && Pa::dab_alpha(11, b) == 0.f &&
              Pa::dab_alpha(10, b) > 0.4f && Pa::dab_alpha(10, b) < 0.6f, std::to_string(Pa::dab_alpha(10, b)));
        b.hardness = 0;
        check("dab: soft -- falls off from the centre", Pa::dab_alpha(0, b) == 1.f && Pa::dab_alpha(5, b) > 0.4f && Pa::dab_alpha(5, b) < 0.6f,
              std::to_string(Pa::dab_alpha(5, b)));
    }
    {
        Fix f;
        Pa::Brush b;
        b.size = 40;
        b.hardness = 1;
        b.opacity = 0.5;
        b.colour = P::rgba(0, 0, 1, 1);
        // Over itself: a stroke never builds past its opacity.
        Pa::stroke(f.j, f.d, "L5", {{100, 100}, {300, 100}, {100, 100}, {300, 100}}, b);
        const auto px = f.d.get("L5")->pixels;
        check("stroke: ONE step, labelled", f.j.size() == 1 && f.j.undo_label() == "Brush Stroke", f.j.undo_label());
        check("stroke: the opacity is a ceiling (crossed four times, still 50%)", px && std::abs(px->get(200, 100).c[3] - 0.5f) < 0.01f,
              px ? std::to_string(px->get(200, 100).c[3]) : "");
        check("stroke: only the tiles it touched were written", px && px->tile_count() == 2 && !px->tile(0, 1), px ? std::to_string(px->tile_count()) : "");
        f.j.undo();
        check("stroke: undo -- the layer as it was", !f.d.get("L5")->pixels, "");
    }
    {
        Fix f;
        klimt::core::layers::load_selection(f.d, {200, 0, 100, 400});
        Pa::Brush b;
        b.size = 60;
        b.colour = P::rgba(0, 1, 0, 1);
        Pa::stroke(f.j, f.d, "L5", {{0, 200}, {600, 200}}, b);
        const auto px = f.d.get("L5")->pixels;
        check("stroke through a selection: only inside it", px && px->get(250, 200).c[3] > 0.99f && px->get(150, 200).c[3] == 0.f &&
              px->get(350, 200).c[3] == 0.f, "");
    }
    {
        Fix f;   // A: a red square (100, 100) 200 x 150
        Pa::Brush e;
        e.size = 30;
        e.hardness = 1;
        e.erase = true;
        Pa::stroke(f.j, f.d, "L4", {{120, 175}, {280, 175}}, e);
        const auto px = f.d.get("L4")->pixels;
        check("eraser: alpha gone along the stroke, kept off it", px && px->get(200, 175).c[3] == 0.f && px->get(200, 110).c[3] == 1.f &&
              f.j.undo_label() == "Eraser", f.j.undo_label());
        auto l = *f.d.get("L4");
        l.locks = klimt::core::lock::Alpha;
        { klimt::core::Document::Loading load(f.d); f.d.put("L4", l); }
        check("eraser: refused under an Alpha lock", !Pa::refusal(f.d, "L4", e).empty(), "");
        Pa::Brush b;
        b.size = 50;
        b.colour = P::rgba(0, 0, 1, 1);
        check("brush on a group: refused in words", !Pa::refusal(f.d, "L2", b).empty() && !Pa::stroke(f.j, f.d, "L2", {{10, 10}}, b), "");
        const auto before = f.j.size();
        Pa::stroke(f.j, f.d, "L4", {{50, 175}, {350, 175}}, b);   // alpha locked: colour only where red was
        const auto q = f.d.get("L4")->pixels;
        check("brush, alpha locked: colour where there was something, nothing new",
              f.j.size() == before + 1 && q->get(150, 120).c[2] == 0.f && q->get(150, 195).c[2] > 0.9f && q->get(50, 175).c[3] == 0.f,
              std::to_string(f.j.size() - before) + " " + std::to_string(q->get(150, 120).c[2]) + " " + std::to_string(q->get(150, 195).c[2]) + " " + std::to_string(q->get(50, 175).c[3]));
    }
    {
        // On a layer with no alpha (a JPEG's Background), the eraser paints the background colour.
        const P::Format jpeg{P::Depth::U8, P::Mode::RGB, false};
        const auto orig = P::TileMap(jpeg, 300, 200).fill({0, 0, 300, 200}, P::rgba(1, 0, 0));
        auto cov = Pa::new_coverage(300, 200);
        auto touched = klimt::core::tiles_of({.width = 300, .height = 200, .format = jpeg});
        Pa::Brush e;
        e.size = 20;
        e.hardness = 1;
        e.erase = true;
        e.background = P::rgba(1, 1, 1);
        cov = Pa::add_dabs(cov, e, {{150, 100}}, touched);
        const auto out = Pa::apply(orig, orig, cov, nullptr, e, touched);
        check("eraser, no alpha: the background colour", out.get(150, 100).c[1] == 1.f && out.get(10, 10).c[1] == 0.f, "");
    }

    std::cout << "\n-- the Lasso: Alt's straight sides (s038) --\n";
    {
        using L = S::LassoTrace;
        const auto same = [](S::Pt a, S::Pt b) { return std::abs(a.x - b.x) < 1e-9 && std::abs(a.y - b.y) < 1e-9; };
        {   // freehand only: the drag's points, closed on release
            L t;
            t.start({0, 0}, false, 4, 1);
            for (int i = 1; i <= 10; ++i) t.move({double(i * 10), double(i)}, false);
            t.release({100, 50}, false);
            check("lasso trace: freehand -- every move a point, done on release", t.done() && t.points().size() == 12 && !t.went_straight(), std::to_string(t.points().size()));
        }
        {   // Alt mid-drag: a corner where it went down; moves don't add points; the band follows
            L t;
            t.start({0, 0}, false, 4, 1);
            t.move({50, 0}, false);
            t.move({60, 0}, true);
            t.move({70, 30}, true);
            t.move({80, 90}, true);
            check("lasso trace: Alt down -- straight, the band only", t.straight() && t.points().size() == 3 && same(t.tip(), {80, 90}), std::to_string(t.points().size()));
            t.move({90, 100}, false);   // Alt up, button down: the side lands, freehand again
            t.move({95, 120}, false);
            check("lasso trace: Alt up, button down -- the side lands, freehand on", !t.straight() && t.points().size() == 5 && same(t.points()[3], {90, 100}), std::to_string(t.points().size()));
            t.release({95, 140}, false);
            check("lasso trace: button up without Alt -- closes", t.done(), "");
        }
        {   // Alt held, button let go: open; clicks lay corners; Alt up closes
            L t;
            t.start({0, 0}, false, 4, 1);
            t.move({10, 0}, false);
            t.move({20, 0}, true);
            t.release({100, 0}, true);
            check("lasso trace: Alt held at release -- open, a corner there", t.open() && same(t.points().back(), {100, 0}), "");
            t.move({100, 50}, true);
            check("lasso trace: open -- the band follows the pointer", same(t.tip(), {100, 50}) && t.points().size() == 4, std::to_string(t.points().size()));
            t.press({100, 100}, true);
            t.release({100, 100}, true);
            check("lasso trace: a click while open -- one corner", t.open() && t.points().size() == 5 && same(t.points().back(), {100, 100}), std::to_string(t.points().size()));
            t.alt_up();
            check("lasso trace: Alt let go, button up -- closes", t.done() && t.points().size() == 5, "");
        }
        {   // Alt gone while the keys were elsewhere: the next move closes it
            L t;
            t.start({0, 0}, false, 4, 1);
            t.move({10, 0}, true);
            t.release({50, 0}, true);
            t.move({50, 40}, false);
            check("lasso trace: a move with Alt up while open -- closes", t.done(), "");
        }
        {   // close by clicking the start; by a double-click; Enter; Esc
            L t;
            t.start({0, 0}, false, 4, 1);
            t.move({1, 0}, false);   // arms Alt
            t.move({2, 0}, true);
            t.release({100, 0}, true);
            t.press({100, 100}, true);
            t.release({100, 100}, true);
            t.press({2, 3}, true);   // within 4 of the start
            check("lasso trace: a click on the start -- closes, no corner added", t.done() && t.points().size() == 5, std::to_string(t.points().size()));
            L u;
            u.start({0, 0}, false, 4, 1);
            u.move({2, 0}, true);
            u.release({100, 0}, true);
            u.press({100, 100}, true);
            u.release({100, 100}, true);
            u.press({101, 101}, true);   // the double-click's second press
            check("lasso trace: a double-click -- closes", u.done() && u.points().size() == 4, std::to_string(u.points().size()));
            L v;
            v.start({0, 0}, false, 4, 1);
            v.move({2, 0}, true);
            v.move({50, 50}, true);
            v.finish();
            check("lasso trace: Enter mid-side -- the band's end is the last corner", v.done() && same(v.points().back(), {50, 50}), "");
            L w;
            w.start({0, 0}, false, 4, 1);
            w.move({2, 0}, true);
            w.release({50, 0}, true);
            w.cancel();
            check("lasso trace: Esc -- cancelled, nothing done", w.state() == L::State::Cancelled && !w.done(), "");
        }
        {   // Alt at the press is the Subtract op: not straight until let go once
            L t;
            t.start({0, 0}, true, 4, 1);
            t.move({10, 0}, true);
            t.move({20, 5}, true);
            check("lasso trace: Alt at the press -- freehand (it's the op)", !t.straight() && t.points().size() == 3, std::to_string(t.points().size()));
            t.move({30, 5}, false);
            t.move({40, 5}, true);
            check("lasso trace: ... Alt again after letting go -- straight", t.straight(), "");
            L u;
            u.start({0, 0}, true, 4, 1);
            u.move({10, 0}, true);
            u.release({20, 0}, true);
            check("lasso trace: Alt at the press, held to release -- closes (not open)", u.done(), "");
        }
        {   // Backspace: the last corner goes; the start stays
            L t;
            t.start({0, 0}, false, 4, 1);
            t.move({2, 0}, true);
            t.release({100, 0}, true);
            t.press({100, 100}, true);
            t.release({100, 100}, true);
            const auto n = t.points().size();
            const bool b1 = t.back();
            check("lasso trace: Backspace -- the last corner goes", b1 && t.points().size() == n - 1 && same(t.points().back(), {100, 0}), std::to_string(t.points().size()));
            t.back();
            t.back();
            t.back();
            check("lasso trace: Backspace to the end -- the start stays", !t.points().empty() && same(t.points().front(), {0, 0}) && t.open(), "");
        }
        {   // the trace's polygon makes the same mask as the shape it draws
            L t;
            t.start({100, 100}, false, 4, 1);
            t.move({101, 100}, false);
            t.move({101, 100}, true);
            t.release({300, 100}, true);
            t.press({300, 300}, true);
            t.release({300, 300}, true);
            t.press({100, 300}, true);
            t.release({100, 300}, true);
            t.alt_up();
            const auto m = S::polygon_mask(kMask, 400, 400, t.points());
            check("lasso trace: three straight sides -- the square's mask", t.done() && std::abs(area(m) - 40000.0) < 2 * 200 + 1, std::to_string(area(m)));
        }
    }
}
