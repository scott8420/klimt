// klimt_selftest -- transforms (s011 m1): the affine algebra, lossless flips
// and quarter turns, scaling, the selection-or-layer rule, pixels beyond the
// canvas, one step each.
#include "core/Canvas.hpp"
#include "core/Document.hpp"
#include "core/Selection.hpp"
#include "core/Transform.hpp"
#include "core/Text.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <numbers>
#include <string>
#include <string_view>
using namespace std::literals;

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace X = klimt::core::xform;
namespace S = klimt::core::select;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
const P::Px kWhite = P::rgba(1, 1, 1, 1);
struct Fix {
    core::Document d;
    core::DocJournal j{d};
    Fix() {
        d.set_door([this] { return j.open(); });
        core::layers::load_fixture(d);
        // "A" (L4): the red square (100, 100) 200 x 150, a blue stripe down its
        // left 10 px and a green one along its top 5 px -- so a flip or a turn
        // shows which way it went.
        core::Document::Loading load(d);
        auto l = *d.get("L4");
        auto m = l.pixels->fill({100, 100, 10, 150}, P::rgba(0, 0, 1, 1)).fill({100, 100, 200, 5}, P::rgba(0, 1, 0, 1));
        l.pixels = std::make_shared<const P::TileMap>(std::move(m));
        d.put("L4", l);
    }
    P::Px at(int x, int y) const { return at(std::string_view("L4"), x, y); }
    P::Px at(std::string_view id, int x, int y) const {
        const auto l = d.get(std::string(id));
        return l && l->pixels ? l->pixels->get(x, y) : P::transparent();
    }
};
bool red(const P::Px& p) { return p.c[0] > 0.99f && p.c[1] < 0.01f && p.c[2] < 0.01f && p.c[3] > 0.99f; }
bool blue(const P::Px& p) { return p.c[2] > 0.99f && p.c[0] < 0.01f && p.c[3] > 0.99f; }
bool green(const P::Px& p) { return p.c[1] > 0.99f && p.c[0] < 0.01f && p.c[3] > 0.99f; }
bool clear(const P::Px& p) { return p.c[3] <= 0.f; }
}  // namespace

void selftest_transform(const Check& check) {
    std::cout << "\n-- transform: the algebra (s011) --\n";
    {
        const auto m = X::Affine::about({10, 20}, X::Affine::rotate(std::numbers::pi / 2));
        const auto p = m.apply({11, 20});
        check("transform: a quarter turn about a point", std::abs(p.x - 10) < 1e-9 && std::abs(p.y - 21) < 1e-9,
              std::to_string(p.x) + "," + std::to_string(p.y));
        const auto r = (m * m.inverse()).apply({123.5, -7.25});
        check("transform: m * m^-1 is the identity", std::abs(r.x - 123.5) < 1e-9 && std::abs(r.y + 7.25) < 1e-9, "");
        const auto b = X::mapped({0, 0, 100, 50}, X::Affine::scale(2, 3));
        check("transform: a box's landing", b == P::Rect{0, 0, 200, 150}, "");
    }

    std::cout << "\n-- transform: a whole layer (s011) --\n";
    {
        Fix t;
        check("transform: Flip Horizontal", X::flip(t.j, t.d, {"L4"}, true, kWhite), "");
        check("transform: the stripe went to the right, exactly", blue(t.at(299, 150)) && blue(t.at(290, 150)) &&
                                                                 red(t.at(289, 150)) && red(t.at(100, 150)), "");
        check("transform: about the layer's own box", clear(t.at(99, 150)) && clear(t.at(300, 150)), "");
        check("transform: one step, named", t.j.size() == 1 && t.j.undo_label() == "Flip Horizontal", t.j.undo_label());
        t.j.undo();
        check("transform: undo", blue(t.at(100, 150)) && red(t.at(299, 150)), "");
    }
    {
        Fix t;
        const auto before = *t.d.get("L4")->pixels;
        for (int i = 0; i < 4; ++i) X::rotate(t.j, t.d, {"L4"}, 90, kWhite);
        bool same = true;
        for (int y = 0; y < 400 && same; ++y)
            for (int x = 0; x < 600 && same; ++x) same = t.at(x, y) == before.get(x, y);
        check("transform: four quarter turns are lossless", same, "");
    }
    {
        Fix t;
        X::rotate(t.j, t.d, {"L4"}, 90, kWhite);   // 200 x 150 about (200, 175) -> 150 x 200 at (125, 75)
        check("transform: a quarter turn clockwise", green(t.at(274, 150)) && blue(t.at(150, 76)) && blue(t.at(150, 84)) && red(t.at(150, 86)) &&
                                                     red(t.at(150, 274)) && clear(t.at(150, 275)) && clear(t.at(124, 150)),
              std::to_string(t.at(274, 150).c[1]));
        check("transform: the document holds", t.d.check().empty(), t.d.check());
    }
    {
        Fix t;
        const X::Affine m = X::Affine::about({200, 175}, X::Affine::scale(1.5, 1.5));
        check("transform: scale 150%", X::apply(t.j, t.d, {"L4"}, m, "Free Transform", kWhite), "");
        check("transform: bigger, red in the middle, the stripe wider", red(t.at(200, 175)) && clear(t.at(48, 175)) &&
                                                                       blue(t.at(52, 175)) && blue(t.at(63, 175)) && red(t.at(70, 175)), "");
        const auto r = core::canvas::content_bounds(*t.d.get("L4")->pixels);
        check("transform: the box landed 300 x 225", std::abs(r.w - 300) <= 4 && std::abs(r.h - 225) <= 4,
              std::to_string(r.w) + " x " + std::to_string(r.h));
    }
    {
        Fix t;
        const X::Affine m = X::Affine::about({200, 175}, X::Affine::rotate(std::numbers::pi / 6));
        X::apply(t.j, t.d, {"L4"}, m, "Free Transform", kWhite);
        const auto p = t.at(150, 160);
        check("transform: a 30-degree turn -- solid inside, soft edges",
              red(t.at(200, 175)) && p.c[3] > 0.99f && clear(t.at(110, 105)), "");
    }
    {
        Fix t;
        core::layers::set_locks(t.j, t.d, "L4", core::lock::Position);
        check("transform: the Position lock refuses", !X::refusal(t.d, {"L4"}).empty() && !X::flip(t.j, t.d, {"L4"}, true, kWhite), "");
        check("transform: an empty layer: nothing to transform", !X::plan(t.d, {"L5"}, kWhite).has_value(), "");
    }

    std::cout << "\n-- transform: a selection (s011) --\n";
    {
        Fix t;
        core::layers::load_selection(t.d, {100, 100, 50, 150});   // the square's left quarter, stripe and all
        check("transform: the selection's pixels", X::flip(t.j, t.d, {"L4"}, true, kWhite), "");
        check("transform: flipped within its own box", blue(t.at(149, 150)) && red(t.at(100, 150)) && red(t.at(200, 150)), "");
        check("transform: the selection stays its shape", t.d.selection() && S::coverage(*t.d.selection(), 120, 150) > 0.99f &&
                                                         S::coverage(*t.d.selection(), 160, 150) < 0.01f, "");
        check("transform: pixels and selection, one step", t.j.size() == 1, std::to_string(t.j.size()));
    }
    {
        Fix t;
        core::layers::load_selection(t.d, {100, 100, 50, 150});
        X::rotate(t.j, t.d, {"L4"}, 90, kWhite);   // 50 x 150 about (125, 175) -> 150 x 50 at (50, 150)
        check("transform: the lifted part turned, the hole is clear", red(t.at(60, 175)) && clear(t.at(110, 120)) &&
                                                                      red(t.at(160, 120)), "");
        check("transform: the selection turned with it", t.d.selection() && S::coverage(*t.d.selection(), 60, 175) > 0.99f &&
                                                         S::coverage(*t.d.selection(), 125, 120) < 0.01f, "");
        check("transform: the document holds", t.d.check().empty(), t.d.check());
    }
    {
        // Lifted pixels carried past the edge are kept beyond it (unlike Move's).
        Fix t;
        core::layers::load_selection(t.d, {150, 120, 100, 80});
        X::apply(t.j, t.d, {"L4"}, X::Affine::translate(500, 0), "Free Transform", kWhite);
        check("transform: lifted pixels past the edge are kept", red(t.at(700, 150)) && clear(t.at(200, 150)), "");
        check("transform: the selection keeps only what's on the canvas", t.d.check().empty(), t.d.check());
    }
    {
        Fix t;
        {
            core::Document::Loading load(t.d);
            auto l = *t.d.get("L4");
            l.pixels = std::make_shared<const P::TileMap>(core::canvas::reframed(*l.pixels, 600, 400, -150, 0, P::transparent()));
            t.d.put("L4", l);
        }
        // A whole layer half off the canvas flips as a whole.
        X::flip(t.j, t.d, {"L4"}, true, kWhite);
        check("transform: what's beyond the canvas takes part", blue(t.at(149, 150)) && red(t.at(-50, 150)), "");
    }

    std::cout << "\n-- transform: the whole canvas (s011) --\n";
    {
        Fix t;
        check("transform: canvas 90 clockwise", X::turn(t.j, t.d, X::Turn::Clockwise), "");
        // (x, y) -> (400 - y, x): the square 100..300 x 100..250 -> 150..300 x 100..300, its stripe on top.
        check("transform: the canvas is 400 x 600", t.d.image().width == 400 && t.d.image().height == 600, "");
        check("transform: every pixel where it belongs", blue(t.at(200, 100)) && blue(t.at(200, 109)) && red(t.at(200, 110)) &&
                                                         green(t.at(299, 200)) && red(t.at(150, 299)) && clear(t.at(149, 200)), "");
        check("transform: one step, named", t.j.undo_label() == "Rotate Canvas 90° Clockwise", t.j.undo_label());
        check("transform: the document holds", t.d.check().empty(), t.d.check());
    }
    {
        Fix t;
        core::layers::load_selection(t.d, {10, 10, 100, 50});
        const auto before = *t.d.get("L4")->pixels;
        for (int i = 0; i < 4; ++i) X::turn(t.j, t.d, X::Turn::Clockwise);
        bool same = t.d.image().width == 600;
        for (int y = 0; y < 400 && same; ++y)
            for (int x = 0; x < 600 && same; ++x) same = t.at(x, y) == before.get(x, y);
        check("transform: four canvas turns are lossless", same, "");
        check("transform: the selection turned all the way round", t.d.selection() && S::coverage(*t.d.selection(), 50, 30) > 0.99f &&
                                                                  S::coverage(*t.d.selection(), 50, 70) < 0.01f, "");
    }
    {
        Fix t;
        X::turn(t.j, t.d, X::Turn::FlipHorizontal);
        check("transform: Flip Canvas Horizontal", red(t.at(300, 150)) && blue(t.at(499, 150)) && clear(t.at(501, 150)), "");
    }
    {
        Fix t;
        check("transform: Rotate Canvas 30 degrees", X::rotate_canvas(t.j, t.d, 30, kWhite), "");
        const int w = t.d.image().width, h = t.d.image().height;
        check("transform: the canvas grew to hold it", std::abs(w - 720) <= 1 && std::abs(h - 647) <= 1,
              std::to_string(w) + " x " + std::to_string(h));
        check("transform: the new corners are clear, the middle is the image", t.at("L1"sv, 2, 2).c[3] <= 0.f && t.at("L1"sv, w / 2, h / 2).c[3] > 0.99f, "");
    }
    {
        check("transform: a level line needs no turn", std::abs(X::straighten_angle({0, 0}, {100, 0})) < 1e-9, "");
        check("transform: an upright line needs no turn", std::abs(X::straighten_angle({0, 0}, {0, 100})) < 1e-9, "");
        check("transform: a line 10 in 100 down turns ~5.7 back",
              std::abs(X::straighten_angle({0, 0}, {100, 10}) + 5.7106) < 1e-3, std::to_string(X::straighten_angle({0, 0}, {100, 10})));
        Fix t;
        core::layers::load_selection(t.d, {10, 10, 100, 50});
        check("transform: Straighten", X::straighten(t.j, t.d, {0, 0}, {100, 10}, kWhite), "");
        const int w = t.d.image().width, h = t.d.image().height;
        check("transform: cropped inside the turn, its proportions kept", w < 600 && h < 400 && std::abs(double(w) / h - 1.5) < 0.01,
              std::to_string(w) + " x " + std::to_string(h));
        bool white = true;   // the Background fills the whole new canvas: no clear corner was kept in
        for (int y = 0; y < h && white; y += 7)
            for (int x = 0; x < w && white; x += 7) white = t.at("L1"sv, x, y).c[3] > 0.99f;
        check("transform: no empty corner inside the straightened canvas", white, "");
        check("transform: what was cropped is kept beyond", t.d.get("L1")->pixels->has_outside(), "");
        check("transform: the selection dropped (a crop's way)", t.d.selection() == nullptr, "");
    }

    // ── s073: projective -- Skew, Distort, Perspective ──────────────────────
    std::cout << "\n-- transform: projective (s073) --\n";
    const auto near = [](X::Pt a, X::Pt b, double tol = 1e-6) { return std::abs(a.x - b.x) < tol && std::abs(a.y - b.y) < tol; };
    {
        // The box's corners land on the quad's, and an affine stays affine.
        const X::Quad q{{{10, 20}, {210, 0}, {260, 180}, {0, 160}}};
        const auto m = X::Projective::box_to_quad(P::Rect{100, 100, 200, 150}, q);
        check("projective: box to quad", m.has_value(), "");
        if (m) {
            check("projective: the corners land on the quad's",
                  near(m->apply({100, 100}), q.p[0]) && near(m->apply({300, 100}), q.p[1]) &&
                  near(m->apply({300, 250}), q.p[2]) && near(m->apply({100, 250}), q.p[3]), "");
            check("projective: a trapezoid is not affine", !m->affine(), "");
            const auto r = (*m * m->inverse()).apply({123.5, -7.25});
            check("projective: m * m^-1 is the identity", near(r, {123.5, -7.25}, 1e-6), "");
            const auto b = X::mapped(P::Rect{100, 100, 200, 150}, *m);
            check("projective: where the box lands", b == P::Rect{0, 0, 260, 180}, "");
        }
        const X::Quad par{{{100, 100}, {300, 100}, {340, 250}, {140, 250}}};   // a skew: a parallelogram
        const auto s = X::Projective::box_to_quad(P::Rect{100, 100, 200, 150}, par);
        check("projective: a skew is affine", s && s->affine(), "");
        const X::Affine a = X::Affine::about({5, 5}, X::Affine::rotate(0.7) * X::Affine::scale(2, 0.5));
        const X::Projective pa(a);
        check("projective: an affine carried over exactly", pa.affine() && near(pa.apply({17, -3}), a.apply({17, -3})), "");
        check("projective: the identity", X::Projective{}.identity() && !pa.identity(), "");
    }
    {
        const X::Quad bow{{{0, 0}, {100, 100}, {100, 0}, {0, 100}}};   // crossed: a bow-tie
        const X::Quad fold{{{0, 0}, {100, 0}, {20, 20}, {0, 100}}};    // a corner pushed in: concave
        const X::Quad flat{{{0, 0}, {50, 0}, {100, 0}, {0, 100}}};     // three in a line
        check("projective: a bow-tie is refused", !X::convex(bow) && !X::Projective::box_to_quad(P::Rect{0, 0, 10, 10}, bow), "");
        check("projective: a concave quad is refused", !X::convex(fold) && !X::Projective::box_to_quad(P::Rect{0, 0, 10, 10}, fold), "");
        check("projective: three corners in a line are refused", !X::convex(flat), "");
        const X::Quad ccw{{{0, 0}, {0, 100}, {100, 100}, {100, 0}}};   // mirrored (a flip): still one piece
        check("projective: a mirrored quad is fine", X::convex(ccw), "");
    }
    {
        // Distort the red square into a trapezoid: narrow at the top.
        Fix t;
        const X::Quad q{{{150, 100}, {250, 100}, {300, 250}, {100, 250}}};
        check("projective: Distort", X::distort(t.j, t.d, {"L4"}, q, "Distort", kWhite), "");
        check("projective: one step, named", t.j.size() == 1 && t.j.undo_label() == "Distort", t.j.undo_label());
        check("projective: the top narrowed", clear(t.at(120, 104)) && clear(t.at(280, 104)) && (red(t.at(200, 104)) || green(t.at(200, 104))), "");
        check("projective: the bottom kept its width", blue(t.at(103, 245)) && red(t.at(295, 245)) && red(t.at(120, 245)) && clear(t.at(98, 245)), "");
        check("projective: the middle is solid", red(t.at(200, 175)), "");
        // The stripe along the left edge follows the slanted side.
        check("projective: the blue stripe follows the side", blue(t.at(149, 110)) && blue(t.at(107, 240)) && clear(t.at(144, 110)) && !blue(t.at(155, 110)), "");
        check("projective: the document holds", t.d.check().empty(), t.d.check());
        t.j.undo();
        check("projective: undo", red(t.at(120, 104)) || green(t.at(120, 104)), "");
    }
    {
        // A perspective: the far side (top) shrunk 4x -- the filter widens there
        // (no aliasing: a 2-px checker averages toward grey, not to stripes).
        Fix t;
        {
            core::Document::Loading load(t.d);
            auto l = *t.d.get("L4");
            P::TileMap m = *l.pixels;
            for (int y = 100; y < 250; ++y)
                for (int x = 100; x < 300; x += 2)
                    m = m.fill({x + ((y / 2) % 2), y, 1, 1}, P::rgba(0, 0, 0, 1));
            l.pixels = std::make_shared<const P::TileMap>(std::move(m));
            t.d.put("L4", l);
        }
        const X::Quad q{{{175, 100}, {225, 100}, {300, 250}, {100, 250}}};
        check("projective: Perspective", X::distort(t.j, t.d, {"L4"}, q, "Perspective", kWhite), "");
        double lo = 1, hi = 0;
        for (int x = 190; x < 210; ++x) {
            const auto p = t.at(x, 103);
            lo = std::min<double>(lo, p.c[0]);
            hi = std::max<double>(hi, p.c[0]);
        }
        check("projective: the far side is filtered, not aliased", hi - lo < 0.35, std::to_string(lo) + " .. " + std::to_string(hi));
        // The near side (bottom) is barely shrunk: the checker still shows.
        double nlo = 1, nhi = 0;
        for (int x = 190; x < 210; ++x) {
            const auto p = t.at(x, 246);
            nlo = std::min<double>(nlo, p.c[0]);
            nhi = std::max<double>(nhi, p.c[0]);
        }
        check("projective: the near side keeps its detail", nhi - nlo > 0.5, std::to_string(nlo) + " .. " + std::to_string(nhi));
    }
    {
        // A selection's pixels distorted, the selection with them.
        Fix t;
        core::layers::load_selection(t.d, {150, 120, 100, 100});
        const X::Quad q{{{150, 120}, {250, 140}, {250, 200}, {150, 220}}};
        check("projective: a selection's pixels", X::distort(t.j, t.d, {"L4"}, q, "Distort", kWhite), "");
        check("projective: the selection took the shape", t.d.selection() && S::coverage(*t.d.selection(), 245, 170) > 0.99f &&
                                                          S::coverage(*t.d.selection(), 245, 125) < 0.01f, "");
        check("projective: pixels and selection, one step", t.j.size() == 1, std::to_string(t.j.size()));
        check("projective: the document holds", t.d.check().empty(), t.d.check());
    }
    {
        // A bad quad is no step.
        Fix t;
        const X::Quad bow{{{100, 100}, {300, 250}, {300, 100}, {100, 250}}};
        check("projective: a bow-tie is no step", !X::distort(t.j, t.d, {"L4"}, bow, "Distort", kWhite) && t.j.size() == 0, "");
    }
    {
        // Type stays type: a distort is refused, a skew keeps it live.
        Fix t;
        klimt::text::Spec sp;
        sp.text = "Klimt"; sp.x = 60; sp.y = 120; sp.size = 40;
        const auto id = core::layers::add_text(t.j, t.d, sp, "L4");
        const auto pl = X::plan(t.d, {id}, kWhite);
        check("projective: a type layer refuses a distort", pl && !X::projective_refusal(*pl).empty(), "");
        const auto steps = t.j.size();
        if (pl) {
            const auto& b = pl->box;
            const double x0 = b.x, y0 = b.y, x1 = b.x + b.w, y1 = b.y + b.h;
            const X::Quad trap{{{x0 + 10, y0}, {x1 - 10, y0}, {x1, y1}, {x0, y1}}};
            check("projective: ... and no step", !X::distort(t.j, t.d, {id}, trap, "Distort", kWhite) && t.j.size() == steps, "");
            const X::Quad skew{{{x0 + 20, y0}, {x1 + 20, y0}, {x1, y1}, {x0, y1}}};
            check("projective: a skew of type", X::distort(t.j, t.d, {id}, skew, "Skew", kWhite), "");
            const auto l = t.d.get(id);
            check("projective: ... stays type", l && l->kind == core::Kind::Text && l->text, "");
        }
    }
}

// s011 m4 -- Select > Modify.
void selftest_modify(const Check& check) {
    std::cout << "\n-- select: Modify (s011) --\n";
    const P::Format mf{P::Depth::U8, P::Mode::Gray, false};
    const auto sq = S::rect_mask(mf, 600, 400, {200, 100, 100, 100});
    const auto cov = [](const P::TileMap& m, int x, int y) { return S::coverage(m, x, y); };
    {
        const auto e = S::modified(sq, S::Modify::Expand, 10);
        check("modify: Expand 10 -- 9 px out is in", e && cov(*e, 191, 150) > 0.99f && cov(*e, 250, 150) > 0.99f, "");
        check("modify: ...10 px out is the soft edge, 11 is out", e && std::abs(cov(*e, 190, 150) - 0.5f) < 0.01f && cov(*e, 189, 150) < 0.01f,
              e ? std::to_string(cov(*e, 190, 150)) : "");
        check("modify: ...the corner is round (Euclidean)", e && cov(*e, 191, 91) < 0.01f && cov(*e, 194, 94) > 0.4f, "");
    }
    {
        const auto c = S::modified(sq, S::Modify::Contract, 10);
        check("modify: Contract 10", c && cov(*c, 200, 150) < 0.01f && cov(*c, 209, 150) < 0.01f && cov(*c, 210, 150) > 0.99f &&
                                     cov(*c, 250, 150) > 0.99f && cov(*c, 289, 150) > 0.99f && cov(*c, 290, 150) < 0.01f, "");
    }
    {
        const auto b = S::modified(sq, S::Modify::Border, 10);
        check("modify: Border 10 -- a band across the edge", b && cov(*b, 200, 150) > 0.99f && cov(*b, 198, 150) > 0.99f &&
                                                           cov(*b, 250, 150) < 0.01f && cov(*b, 180, 150) < 0.01f, "");
    }
    {
        // A speck outside and a pinhole inside: Smooth 2 takes both away.
        auto m = sq.fill({240, 140, 1, 1}, P::Px{}).fill({400, 300, 1, 1}, [] { P::Px p; p.c[0] = 1; return p; }());
        const auto s = S::modified(m, S::Modify::Smooth, 2);
        check("modify: Smooth fills a pinhole and drops a speck", s && cov(*s, 240, 140) > 0.99f && cov(*s, 400, 300) < 0.01f &&
                                                                cov(*s, 250, 150) > 0.99f, "");
    }
    {
        // The canvas edge isn't "unselected": a selection along it stays along it.
        const auto edge = S::rect_mask(mf, 600, 400, {0, 0, 100, 400});
        const auto c = S::modified(edge, S::Modify::Contract, 10);
        check("modify: Contract keeps to the canvas edge", c && cov(*c, 0, 200) > 0.99f && cov(*c, 0, 0) > 0.99f &&
                                                          cov(*c, 95, 200) < 0.01f, "");
    }
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        core::layers::load_fixture(d);
        core::layers::load_selection(d, {200, 100, 100, 100});
        check("modify: a step", S::modify(j, d, S::Modify::Expand, 8) && j.undo_label() == "Expand 8 px", j.undo_label());
        check("modify: the document holds", d.check().empty(), d.check());
        j.undo();
        check("modify: undo", d.selection() && S::coverage(*d.selection(), 195, 150) < 0.01f, "");
        check("modify: nothing selected: no step", (S::none(j, d), !S::modify(j, d, S::Modify::Expand, 8)), "");
    }
}
