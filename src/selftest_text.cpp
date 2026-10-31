// klimt_selftest -- TEXT LAYERS (s046). docs/text.md. Rules, not looks (fonts
// differ between machines, so no test counts lines or measures a glyph):
// point type sits on its baseline at its anchor; box type stays inside its
// box; alignment puts the ink on the right side of the anchor; the caret and
// the hit-test agree; a layer's pixels are its Spec set in its colour; the
// verbs keep the pixels in step; the canvas verbs carry the type and keep it
// live; .klimt keeps it; painting refuses it; rendering is thread-safe.
#include "core/Canvas.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Live.hpp"
#include "core/Move.hpp"
#include "core/Transform.hpp"
#include "core/Paint.hpp"
#include "core/Text.hpp"
#include "core/Transform.hpp"
#include "text/Hyphen.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include <unistd.h>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace T = klimt::text;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
std::string num(double v) { return std::to_string(v); }

struct Ink { int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1; bool any() const { return x1 >= 0; } };
Ink ink_of(const T::Coverage& c) {
    Ink k;
    for (int y = 0; y < c.at.h; ++y)
        for (int x = 0; x < c.at.w; ++x)
            if (c.a[static_cast<std::size_t>(y * c.at.w + x)] > 32) {
                k.x0 = std::min(k.x0, c.at.x + x); k.x1 = std::max(k.x1, c.at.x + x);
                k.y0 = std::min(k.y0, c.at.y + y); k.y1 = std::max(k.y1, c.at.y + y);
            }
    return k;
}
Ink ink_of(const P::TileMap& m) {
    Ink k;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x)
            if (m.get(x, y).c[3] > 0.13f) {
                k.x0 = std::min(k.x0, x); k.x1 = std::max(k.x1, x);
                k.y0 = std::min(k.y0, y); k.y1 = std::max(k.y1, y);
            }
    return k;
}
core::Document doc(int w = 480, int h = 360, P::Mode mode = P::Mode::RGB) {
    core::Document d;
    core::Document::Loading load(d);
    core::Image im;
    im.width = w;
    im.height = h;
    im.format = {P::Depth::U8, mode, true};
    d.replace(im, {});
    return d;
}
}  // namespace

void selftest_text(const Check& check) {
    std::cout << "\n-- text layers: libs/text, Kind::Text, the verbs, .klimt (s046 m1) --\n";

    // ── setting: where the ink lands ────────────────────────────────────────
    {
        T::Spec s;
        s.text = "HIH";   // no descenders, no overhang
        s.size = 60;
        s.x = 100;
        s.y = 150;
        const auto c = T::render(s, 480, 360);
        const Ink k = ink_of(c);
        check("text: point type sets ink", k.any(), "");
        check("text: point type sits ON its baseline (ink ends at y, within 2 px)", k.any() && k.y1 <= 152 && k.y1 >= 140, "bottom " + num(k.y1));
        check("text: and rises above it, less than 1.2 em", k.any() && k.y0 < 140 && k.y0 > 150 - 72, "top " + num(k.y0));
        check("text: left-aligned point type starts at its anchor", k.any() && k.x0 >= 100 && k.x0 < 100 + 20, "left " + num(k.x0));
        s.align = T::Align::Right;
        const Ink r = ink_of(T::render(s, 480, 360));
        check("text: right-aligned point type ends at its anchor", r.any() && r.x1 <= 102 && r.x1 > 80, "right " + num(r.x1));
        s.align = T::Align::Center;
        const Ink m = ink_of(T::render(s, 480, 360));
        check("text: centred point type straddles its anchor", m.any() && m.x0 < 100 && m.x1 > 100 && std::abs((m.x0 + m.x1) / 2 - 100) < 6,
              num(m.x0) + ".." + num(m.x1));
        s.text.clear();
        check("text: empty text sets nothing", T::render(s, 480, 360).at.empty(), "");
        s.text = "HIH";
        s.x = 2000;
        check("text: type off the image sets nothing", T::render(s, 480, 360).at.empty(), "");
    }
    {
        T::Spec s;
        s.text = "The quick brown fox jumps over the lazy dog and keeps on running";
        s.size = 24;
        s.x = 50;
        s.y = 40;
        s.box_w = 150;
        const Ink k = ink_of(T::render(s, 480, 360));
        check("text: box type stays inside its box's width", k.any() && k.x0 >= 48 && k.x1 <= 50 + 150 + 2, num(k.x0) + ".." + num(k.x1));
        check("text: and wraps to more than one line", k.any() && k.y1 - k.y0 > 24 * 1.5, "height " + num(k.y1 - k.y0));
        check("text: its first line starts at the box's top", k.any() && k.y0 >= 40 && k.y0 < 40 + 24, "top " + num(k.y0));
        s.box_h = 40;
        const Ink c = ink_of(T::render(s, 480, 360));
        check("text: a box's height clips what overflows it", c.any() && c.y1 <= 40 + 40 + 1, "bottom " + num(c.y1));
        check("text: and says so", T::overflows(s), "");
        s.box_h = 24 * 0.5;   // not even one line fits whole: nothing is set
        check("text: a line that doesn't fit whole isn't set", ink_of(T::render(s, 480, 360)).y1 < 0 || ink_of(T::render(s, 480, 360)).y1 <= 40 + 12, "");
        s.box_h = 40;
        const auto fr = T::frame(s);
        check("text: the frame of box type is the box", std::abs(fr[0].x - 50) < 1e-6 && std::abs(fr[2].x - 200) < 1e-6 && std::abs(fr[2].y - 80) < 1e-6, "");
    }
    {   // the matrix: a quarter turn about the origin, then into the image
        T::Spec s;
        s.text = "HIH";
        s.size = 40;
        s.x = 100;
        s.y = 100;
        const Ink a = ink_of(T::render(s, 480, 360));
        s.m = {0, 1, -1, 0, 360, 0};   // (x, y) -> (360 - y, x): Rotate 90 clockwise on a 360-tall image
        const Ink b = ink_of(T::render(s, 480, 360));
        check("text: the matrix turns the type (wide becomes tall)", a.any() && b.any() && (a.x1 - a.x0) > (a.y1 - a.y0) && (b.y1 - b.y0) > (b.x1 - b.x0),
              num(b.x1 - b.x0) + " x " + num(b.y1 - b.y0));
        check("text: and puts it where the map says", b.any() && std::abs(b.x0 - (360 - a.y1)) <= 3 && std::abs(b.y0 - a.x0) <= 3,
              num(b.x0) + "," + num(b.y0));
    }

    // ── editing geometry ────────────────────────────────────────────────────
    {
        T::Spec s;
        s.text = "Gold leaf\nand oil";
        s.size = 32;
        s.x = 40;
        s.y = 80;
        bool round = true;
        std::string where;
        for (int i = 0; i <= static_cast<int>(s.text.size()); i = T::next_char(s.text, i)) {
            const auto c = T::caret(s, i);
            const int back = T::index_at(s, c.x0 + 0.01, (c.y0 + c.y1) / 2);
            if (back != i) { round = false; where += num(i) + "->" + num(back) + " "; }
            if (i == static_cast<int>(s.text.size())) break;
        }
        check("text: the caret and the hit-test agree at every boundary", round, where);
        check("text: right steps one character", T::step(s, 0, 1) == 1 && T::step(s, 1, -1) == 0, "");
        check("text: left at the start stays", T::step(s, 0, -1) == 0, "");
        check("text: right at the end stays", T::step(s, static_cast<int>(s.text.size()), 1) == static_cast<int>(s.text.size()), "");
        const int down = T::line_step(s, 2, 1);
        check("text: down goes to the next line, near the same x", down >= 10 && down <= 13, num(down));
        check("text: up from the first line goes to the start", T::line_step(s, 3, -1) == 0, "");
        check("text: down from the last line goes to the end", T::line_step(s, 12, 1) == static_cast<int>(s.text.size()), "");
        check("text: Home / End find the line's ends", T::line_home(s, 13) == 10 && T::line_end(s, 2) == 9, num(T::line_home(s, 13)) + " " + num(T::line_end(s, 2)));
        check("text: a word step lands on a word's edge", T::word_step(s, 0, 1) == 4 && T::word_step(s, 7, -1) == 5,
              num(T::word_step(s, 0, 1)) + " " + num(T::word_step(s, 7, -1)));
        check("text: a selection across the break is one quad a line", T::range(s, 5, 13).size() == 2, num(static_cast<double>(T::range(s, 5, 13).size())));
        const auto c1 = T::caret(s, 10);
        check("text: the second line's caret is below the first's", c1.y0 > T::caret(s, 0).y0 + 20, "");
        std::string u = "é—x";
        check("text: UTF-8 steps keep whole characters", T::next_char(u, 0) == 2 && T::next_char(u, 2) == 5 && T::prev_char(u, 5) == 2, "");
    }
    // ── s047 m1: Composix's leading -- lines at the pitch, each centred in it ─
    {
        std::cout << "\n-- text: leading, as Composix sets it (s047 m1) --\n";
        T::Spec s;
        s.text = "Gold leaf\nand oil\nand ash";
        s.size = 40;
        s.x = 30;
        s.y = 90;
        const double c0 = T::caret(s, 0).y0, c1 = T::caret(s, 10).y0, c2 = T::caret(s, 18).y0;
        check("text: auto leading is 1.2 x the size, line to line", std::abs(c1 - c0 - 48) < 0.01 && std::abs(c2 - c1 - 48) < 0.01,
              num(c1 - c0) + " " + num(c2 - c1));
        s.leading = 70;
        const double d0 = T::caret(s, 0).y0, d1 = T::caret(s, 10).y0;
        check("text: a set leading is the pitch", std::abs(d1 - d0 - 70) < 0.01, num(d1 - d0));
        check("text: point type's first baseline stays at its anchor whatever the leading",
              std::abs(T::caret(s, 0).y0 - c0) < 0.01, num(T::caret(s, 0).y0) + " vs " + num(c0));
        // box type: the first line's logical box centred in its leading, from the box's top
        T::Spec b = s;
        b.box_w = 400;
        b.leading = 80;
        b.y = 20;
        const auto k = T::caret(b, 0);
        const double h = k.y1 - k.y0;
        check("text: a line's box is centred in its leading", std::abs((k.y0 - 20) - (80 - h) / 2) < 0.01,
              num(k.y0 - 20) + " vs " + num((80 - h) / 2));
        const auto fr = T::frame(b);   // no height: the lines' bands
        check("text: box type with no height is as tall as its lines' leading", std::abs(fr[2].y - (20 + 3 * 80)) < 0.01, num(fr[2].y));
        b.box_h = 160;
        check("text: lines whose leading fits the box are set; the rest overflow", T::overflows(b), "");
        b.box_h = 240;
        check("text: three leadings fit three lines", !T::overflows(b), "");
        b.box_h = 239;
        check("text: one px short and the third line is out", T::overflows(b), "");
        b.box_h = 0;
        const auto q = T::range(b, 0, 4);
        check("text: a selection fills its line's leading", q.size() == 1 && std::abs(q[0][0].y - 20) < 0.01 && std::abs(q[0][2].y - 100) < 0.01,
              q.empty() ? "" : num(q[0][0].y) + ".." + num(q[0][2].y));
        check("text: a click in the leading below a line's glyphs is still that line",
              T::index_at(b, 31, 20 + 80 - 1) <= 9 && T::index_at(b, 31, 20 + 80 + 1) >= 10,
              num(T::index_at(b, 31, 99)) + " " + num(T::index_at(b, 31, 101)));
        const auto ink = ink_of(T::render(b, 480, 360));
        check("text: the set lines' ink lies in their bands", ink.any() && ink.y0 >= 20 && ink.y1 <= 20 + 3 * 80, num(ink.y0) + ".." + num(ink.y1));
    }
    {
        T::Spec s;
        check("text: a fresh Spec is in range", T::refusal(s).empty(), T::refusal(s));
        s.size = 0;
        check("text: size 0 is refused", !T::refusal(s).empty(), "");
        s.m = {0, 0, 0, 0, 0, 0};
        s.text = std::string("bad \xff utf8");
        const auto n = T::normalised(s);
        check("text: normalised puts it all in range", T::refusal(n).empty() && n.size >= 1 && n.m[0] == 1, T::refusal(n));
        T::Spec k;
        k.size = 40; k.x = 10; k.y = 20; k.box_w = 100;
        const auto h = T::mapped(k, {0.5, 0, 0, 0.5, 4, 6});
        check("text: a uniform scale folds into the size, the box and the place", std::abs(h.size - 20) < 1e-9 && std::abs(h.box_w - 50) < 1e-9 &&
              std::abs(h.x - 5) < 1e-9 && h.m[0] == 1 && h.m[3] == 1 && h.m[4] == 4 && h.m[5] == 6, num(h.size));
        check("text: the name is the first line, at most 30 characters", core::text_name("  Hello\nworld") == "Hello" && core::text_name("") == "Text" &&
              core::text_name(std::string(40, 'a')) == std::string(30, 'a') + "…", core::text_name("  Hello\nworld"));
    }

    // ── the layer: pixels in step with the Spec ─────────────────────────────
    {
        auto d = doc();
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        T::Spec s;
        s.text = "IIII";
        s.size = 80;
        s.x = 50;
        s.y = 150;
        s.colour = {0.2f, 0.4f, 0.8f, 1};
        const auto id = L::add_text(j, d, s);
        const auto l = d.get(id);
        check("text: add_text makes a text layer named by its text", l && l->kind == core::Kind::Text && l->name == "IIII" && l->text && l->pixels, "");
        check("text: and its pixels are its Spec set", l && l->pixels && ink_of(*l->pixels).any(), "");
        bool colour = false;
        if (l && l->pixels)
            for (int y = 80; y < 150 && !colour; ++y)
                for (int x = 50; x < 150 && !colour; ++x) {
                    const auto p = l->pixels->get(x, y);
                    if (p.c[3] > 0.99f) colour = std::abs(p.c[0] - 0.2f) < 0.01f && std::abs(p.c[1] - 0.4f) < 0.01f && std::abs(p.c[2] - 0.8f) < 0.01f;
                }
        check("text: in its colour, where the ink is solid", colour, "");
        check("text: the tree holds", d.check().empty(), d.check());
        T::Spec s2 = s;
        s2.text = "I";
        L::set_text(j, d, id, s2);
        const auto l2 = d.get(id);
        check("text: an edit sets the pixels again (fewer letters, less ink)", l2 && l2->pixels && ink_of(*l2->pixels).x1 < ink_of(*l->pixels).x1, "");
        check("text: and the name follows the text", l2 && l2->name == "I", l2 ? l2->name : "");
        L::rename(j, d, id, "Title");
        s2.text = "II";
        L::set_text(j, d, id, s2);
        check("text: a renamed layer keeps its name", d.get(id)->name == "Title", d.get(id)->name);
        check("text: the same Spec again is no step", !L::set_text(j, d, id, s2), "");
        // Painting refuses type; the reason names Rasterize.
        const auto why = L::not_pixels(d, id);
        check("text: not_pixels says it is type, and how to paint", why.find("Rasterize") != std::string::npos, why);
        klimt::core::paint::Brush b;
        check("text: the brush refuses a text layer", !klimt::core::paint::stroke(j, d, id, {{100, 100}, {120, 110}}, b), "");
        const auto before = d.get(id)->pixels;
        check("text: Rasterize makes it a pixel layer of what it showed", L::rasterize_text(j, d, id) && d.get(id)->kind == core::Kind::Pixel &&
              !d.get(id)->text && d.get(id)->pixels == before, "");
        check("text: Rasterize again is refused", !L::rasterize_text(j, d, id), "");
        j.undo();
        check("text: undo brings the type back", d.get(id)->kind == core::Kind::Text && d.get(id)->text, "");
    }
    {   // a visit that made a layer and left it empty leaves nothing
        auto d = doc();
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        const auto n = d.count();
        L::text_begin(j, d, {});
        T::Spec s;
        s.x = 10; s.y = 50;
        const auto id = L::add_text(j, d, s);
        check("text: a layer made inside the visit exists while it lasts", !id.empty() && d.get(id), "");
        check("text: an empty visit keeps no step", !L::text_end(j, d, id, true) && j.size() == 0 && d.count() == n, num(static_cast<double>(j.size())));
    }
    {   // Gray documents
        auto d = doc(300, 200, P::Mode::Gray);
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        T::Spec s;
        s.text = "Gray";
        s.x = 20; s.y = 100; s.size = 40;
        const auto id = L::add_text(j, d, s);
        check("text: type sets in a Gray document", !id.empty() && d.get(id)->pixels && d.check().empty(), d.check());
    }

    // ── the canvas carries it, live ─────────────────────────────────────────
    {
        auto d = doc();
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        T::Spec s;
        s.text = "HIH";
        s.size = 40; s.x = 100; s.y = 100;
        const auto id = L::add_text(j, d, s);
        const Ink a = ink_of(*d.get(id)->pixels);
        core::canvas::crop(j, d, {30, 20, 300, 200}, P::rgba(1, 1, 1, 1));
        const auto c = d.get(id);
        const Ink b = c->pixels ? ink_of(*c->pixels) : Ink{};
        check("text: Crop keeps it type", c->kind == core::Kind::Text && c->text, "");
        check("text: and moves it with the picture", b.any() && std::abs(b.x0 - (a.x0 - 30)) <= 1 && std::abs(b.y0 - (a.y0 - 20)) <= 1, num(b.x0) + "," + num(b.y0));
        core::canvas::image_size(j, d, 150, 100, 72);
        const auto h = d.get(id);
        check("text: Image Size at half halves the size field", h->text && std::abs(h->text->size - 20) < 1e-6, h->text ? num(h->text->size) : "");
        check("text: and the pixels are set at the new size", h->pixels && h->pixels->width() == 150 && d.check().empty(), d.check());
        klimt::core::xform::turn(j, d, klimt::core::xform::Turn::Clockwise);
        const auto t = d.get(id);
        check("text: a canvas turn keeps it type, turned", t->kind == core::Kind::Text && t->text && std::abs(t->text->m[0]) < 1e-9 && t->pixels, "");
        check("text: the tree holds after all three", d.check().empty(), d.check());
        j.undo(); j.undo(); j.undo();
        const Ink u = ink_of(*d.get(id)->pixels);
        check("text: undo all three: where it was", u.x0 == a.x0 && u.y0 == a.y0 && *d.get(id)->text == s, "");
    }

    // ── the Move tool slides it as type ─────────────────────────────────────
    {
        auto d = doc();
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        T::Spec s;
        s.text = "HIH";
        s.size = 40; s.x = 100; s.y = 100;
        const auto id = L::add_text(j, d, s);
        const Ink a = ink_of(*d.get(id)->pixels);
        klimt::core::move::by(j, d, {id}, -400, 0, P::rgba(1, 1, 1, 1));
        check("text: moved wholly off the canvas, it is still type (no pixels)", d.get(id)->kind == core::Kind::Text && !d.get(id)->pixels &&
              std::abs(d.get(id)->text->m[4] + 400) < 1e-9, "");
        klimt::core::move::by(j, d, {id}, 410, 5, P::rgba(1, 1, 1, 1));
        const Ink b = d.get(id)->pixels ? ink_of(*d.get(id)->pixels) : Ink{};
        check("text: and comes back whole -- nothing lost off the edge", b.any() && b.x0 == a.x0 + 10 && b.y0 == a.y0 + 5 && b.x1 == a.x1 + 10,
              num(b.x0) + "," + num(b.y0));
        check("text: the tree holds", d.check().empty(), d.check());
        namespace X = klimt::core::xform;
        const auto before = *d.get(id)->text;
        X::apply(j, d, {id}, X::Affine::about({120, 90}, X::Affine::scale(2, 2)), "Free Transform", P::rgba(1, 1, 1, 1));
        const auto t = d.get(id);
        check("text: Free Transform keeps it type", t->kind == core::Kind::Text && t->text, "");
        check("text: a uniform scale lands in the size field", t->text && std::abs(t->text->size - 2 * before.size) < 1e-6, t->text ? num(t->text->size) : "");
        X::apply(j, d, {id}, X::Affine::about({120, 90}, X::Affine::rotate(0.5)), "Free Transform", P::rgba(1, 1, 1, 1));
        check("text: a turn stays in the matrix, still type", d.get(id)->text && std::abs(d.get(id)->text->m[1] - std::sin(0.5)) < 1e-9 && d.get(id)->pixels, "");
    }

    // ── .klimt keeps it ─────────────────────────────────────────────────────
    {
        namespace fs = std::filesystem;
        auto d = doc();
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        T::Spec s;
        s.text = "Stoclet\nFrieze";
        s.family = "Serif"; s.weight = 700; s.italic = true; s.size = 36; s.colour = {0.7f, 0.5f, 0.1f, 0.9f};
        s.align = T::Align::Center; s.tracking = 50; s.leading = 44; s.x = 60; s.y = 30; s.box_w = 200; s.box_h = 150;
        s.m = {1, 0, 0.2, 1, 3, 4};
        s.hyphenate = true; s.lang = "de";
        {   // s047: runs ride along
            T::Look k; k.weight = 400; k.colour = std::array<float, 3>{0.1f, 0.2f, 0.3f}; k.kern = 40; k.underline = true;
            T::Look k2; k2.size = 20; k2.shift = 6; k2.strike = true; k2.family = "Monospace"; k2.tracking = -30; k2.caps = 2; k2.position = 1; k2.ligatures = false; k2.figures = 2; k2.fig_space = 2; k2.dlig = true; k2.frac = true; k2.font_kern = false;
            s = T::styled(T::styled(s, 0, 4, k), 8, 14, k2);
        }
        const auto id = L::add_text(j, d, s);
        core::Styles st; st.drop.on = true;
        core::layers::set_styles(j, d, id, st);
        const auto path = (fs::temp_directory_path() / ("klimt_s046_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image();
        c.layers = d.snapshot();
        c.next_id = d.next_id();
        std::string err;
        const bool saved = core::file::save_klimt(path, c, err);
        const auto back = saved ? core::file::open(path, err) : std::nullopt;
        fs::remove(path);
        const bool ok = back && back->layers.count(id) && back->layers.at(id).kind == core::Kind::Text && back->layers.at(id).text;
        check("text: .klimt keeps a text layer", ok, err);
        if (ok) {
            const auto& t = *back->layers.at(id).text;
            check("text: with every setting", t.text == s.text && t.family == s.family && t.weight == 700 && t.italic && t.size == 36 &&
                  std::abs(t.colour[3] - 0.9f) < 1e-6 && t.align == T::Align::Center && t.tracking == 50 && t.leading == 44 &&
                  t.box_w == 200 && t.box_h == 150 && t.m[2] == 0.2, "");
            check("text: and its runs, every field", t.runs == s.runs && s.runs.size() == 2, num(static_cast<double>(t.runs.size())));
            check("text: and its hyphenation and language (s048)", t.hyphenate && t.lang == "de", t.lang);
            check("text: and its pixels as they were saved", back->layers.at(id).pixels && ink_of(*back->layers.at(id).pixels).x0 == ink_of(*d.get(id)->pixels).x0, "");
            check("text: and its layer style", back->layers.at(id).styles.drop.on, "");
        }
    }

    // ── many threads at once ────────────────────────────────────────────────
    {
        T::Spec s;
        s.text = "Thread safe";
        s.size = 30; s.x = 10; s.y = 60;
        const auto ref = T::render(s, 400, 100);
        bool same = true;
        std::vector<std::thread> ts;
        std::vector<char> ok(6, 1);
        for (std::size_t k = 0; k < ok.size(); ++k)
            ts.emplace_back([&, k] { for (int r = 0; r < 8; ++r) { const auto c = T::render(s, 400, 100); if (c.a != ref.a) ok[k] = 0; } });
        for (auto& t : ts) t.join();
        for (char o : ok) same = same && o;
        check("text: setting on six threads at once gives the same pixels", same, "");
    }
    check("text: the font map lists families", !T::families().empty(), "");

    // ── s047 m2: runs ───────────────────────────────────────────────────────
    std::cout << "\n-- text: runs -- a word bold, coloured, tracked inside a line (s047 m2) --\n";
    {
        T::Spec s;
        s.text = "gold leaf and oil";   // gold 0-4, leaf 5-9, and 10-13, oil 14-17
        s.size = 40; s.x = 20; s.y = 100;
        T::Look bold; bold.weight = 700;
        const auto b = T::styled(s, 5, 9, bold);
        check("runs: styling a word makes one run over it", b.runs.size() == 1 && b.runs[0].start == 5 && b.runs[0].end == 9 && b.runs[0].look.weight == 700, "");
        check("runs: the letters in it take the look; those outside keep the layer's",
              *T::look_at(b, 6).weight == 700 && *T::look_at(b, 2).weight == 400 && *T::look_at(b, 10).weight == 400, "");
        T::Look it; it.italic = true;
        const auto c = T::styled(b, 7, 12, it);
        check("runs: a look over part of a run splits it", c.runs.size() == 3 && c.runs[1].start == 7 && c.runs[1].end == 9 &&
              c.runs[1].look.weight == 700 && c.runs[1].look.italic == true && c.runs[2].start == 9 && !c.runs[2].look.weight,
              num(static_cast<double>(c.runs.size())));
        T::Look reg; reg.weight = 400;
        check("runs: setting the layer's own look again leaves no run", T::styled(b, 5, 9, reg).runs.empty(), "");
        check("runs: neighbours with one look join", T::styled(b, 9, 13, bold).runs.size() == 1 && T::styled(b, 9, 13, bold).runs[0].end == 13, "");
        check("runs: a whole-layer change clears the field from every run", T::without(c, T::Field::Weight).runs.size() == 1 &&
              !T::without(c, T::Field::Weight).runs[0].look.weight, "");
        // edits carry them
        const auto e1 = T::edited(b, 7, 7, "XY");   // inside "leaf"
        check("runs: typing inside a run joins it", e1.text == "gold leXYaf and oil" && e1.runs.size() == 1 && e1.runs[0].start == 5 && e1.runs[0].end == 11, "");
        const auto e2 = T::edited(b, 9, 9, "s");    // at its end: the letter before is bold
        check("runs: typing at a run's end continues its look", e2.runs.size() == 1 && e2.runs[0].end == 10, "");
        const auto e3 = T::edited(b, 0, 2, "");     // before it
        check("runs: a cut before a run moves it", e3.runs.size() == 1 && e3.runs[0].start == 3 && e3.runs[0].end == 7, "");
        const auto e4 = T::edited(b, 3, 7, "");     // into it
        check("runs: a cut into a run shrinks it", e4.runs.size() == 1 && e4.runs[0].start == 3 && e4.runs[0].end == 5 && e4.text == "golaf and oil", "");
        const auto e5 = T::edited(T::styled(s, 0, 4, bold), 0, 0, "A");
        check("runs: typing at the very start takes the first letter's look", e5.runs.size() == 1 && e5.runs[0].start == 0 && e5.runs[0].end == 5, "");
        T::Look kern; kern.kern = 300;
        const auto e6 = T::edited(T::styled(s, 3, 4, kern), 4, 4, "Z");
        check("runs: new letters don't take a kern", e6.runs.size() == 1 && e6.runs[0].end == 4, "");
        const auto e7 = T::edited(b, 4, 10, "");    // the run cut whole
        check("runs: a cut over a whole run removes it", e7.runs.empty() && e7.text == "goldand oil", e7.text);
        check("runs: UTF-8 -- a run never splits a character", T::styled([] { T::Spec u; u.text = "é—x"; return u; }(), 1, 4, bold).runs[0].start == 0, "");
        T::Spec bad = b;
        bad.runs.push_back({2, 3, bold});
        check("runs: out of order is refused", !T::refusal(bad).empty() && T::refusal(T::normalised(bad)).empty(), T::refusal(bad));
    }
    {
        // The look shows: a bigger run is taller; it gets its own leading
        T::Spec s;
        s.text = "HH HH\nHH";
        s.size = 40; s.x = 20; s.y = 60;
        T::Look big; big.size = 80;
        const auto b = T::styled(s, 3, 5, big);
        const Ink a0 = ink_of(T::render(s, 480, 360)), a1 = ink_of(T::render(b, 480, 360));
        check("runs: a run's size sets its letters bigger", a0.any() && a1.any() && a1.y0 < a0.y0 - 20, num(a0.y0) + " -> " + num(a1.y0));
        const auto band = [](const T::Spec& x, int i) { const auto q = T::range(x, i, T::next_char(x.text, i)); return q.empty() ? -1.0 : q[0][2].y - q[0][0].y; };
        check("runs: auto leading is 1.2 x the line's LARGEST size", std::abs(band(b, 0) - 96) < 0.01 && std::abs(band(b, 6) - 48) < 0.01 &&
              std::abs(band(s, 0) - 48) < 0.01, num(band(b, 0)) + " " + num(band(b, 6)));
        T::Spec fixed = b;
        fixed.leading = 50;
        check("runs: a set leading stays the pitch whatever the sizes", std::abs(band(fixed, 0) - 50) < 0.01 && std::abs(band(fixed, 6) - 50) < 0.01,
              num(band(fixed, 0)) + " " + num(band(fixed, 6)));
        // baseline shift: the letters rise; the line stays where it was
        T::Look up; up.shift = 15;
        const auto r = T::styled(s, 0, 2, up);
        const Ink i0 = ink_of(T::render(s, 480, 360)), i1 = ink_of(T::render(r, 480, 360));
        check("runs: a baseline shift raises its letters", i1.any() && std::abs((i0.y0 - i1.y0) - 15) <= 2, num(i0.y0 - i1.y0));
        check("runs: and doesn't move its line", std::abs(T::caret(r, 6).y0 - T::caret(s, 6).y0) < 0.01, "");
        // kern: the space after a letter -- what follows moves by it, the letter stays
        T::Spec k = s;
        k.text = "HHHH";
        T::Look kn; kn.kern = 500;
        const auto kk = T::styled(k, 1, 2, kn);
        const double dx = T::caret(kk, 2).x0 - T::caret(k, 2).x0, dx3 = T::caret(kk, 3).x0 - T::caret(k, 3).x0;
        check("runs: a kern of 500 moves what follows by half an em", std::abs(dx - 20) < 0.6 && std::abs(dx3 - 20) < 0.6, num(dx) + " " + num(dx3));
        check("runs: and the kerned letter stays", std::abs(ink_of(T::render(kk, 480, 360)).x0 - ink_of(T::render(k, 480, 360)).x0) <= 1, "");
        T::Look tr; tr.tracking = 500;
        const auto tt = T::styled(k, 0, 4, tr);
        check("runs: a run's tracking spreads its letters", ink_of(T::render(tt, 480, 360)).x1 > ink_of(T::render(k, 480, 360)).x1 + 40, "");
        // folded: Image Size scales the runs too
        const auto f = T::folded([&] { T::Spec h = T::styled(r, 3, 5, big); h.m = {0.5, 0, 0, 0.5, 0, 0}; return h; }());
        check("runs: a uniform scale folds into the runs' sizes and shifts",
              std::abs(f.size - 20) < 1e-9 && f.runs.size() == 2 && f.runs[0].look.shift && std::abs(*f.runs[0].look.shift - 7.5) < 1e-9 &&
              f.runs[1].look.size && std::abs(*f.runs[1].look.size - 40) < 1e-9, "");
    }
    {
        // A coloured word: its pixels in its colour, the rest in the layer's
        auto d = doc();
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        T::Spec s;
        s.text = "IIII IIII";
        s.size = 80; s.x = 20; s.y = 150; s.weight = 700;
        s.colour = {0, 0, 1, 1};
        T::Look red; red.colour = std::array<float, 3>{1, 0, 0};
        const auto id = L::add_text(j, d, s);
        check("runs: styling letters is one step", L::set_text(j, d, id, T::styled(s, 5, 9, red), "Text Colour") && j.size() == 2, "");
        const auto& px = *d.get(id)->pixels;
        const Ink all = ink_of(px);
        const int mid = (all.x0 + all.x1) / 2;
        bool left_blue = false, right_red = false, wrong = false;
        for (int y = all.y0; y <= all.y1; ++y)
            for (int x = all.x0; x <= all.x1; ++x) {
                const auto p = px.get(x, y);
                if (p.c[3] < 0.999f) continue;
                if (x < mid) { left_blue = left_blue || (p.c[2] > 0.99f && p.c[0] < 0.01f); wrong = wrong || p.c[0] > 0.01f; }
                else { right_red = right_red || (p.c[0] > 0.99f && p.c[2] < 0.01f); wrong = wrong || p.c[2] > 0.01f; }
            }
        check("runs: a coloured word is set in its colour; the rest in the layer's", left_blue && right_red && !wrong, "");
        j.undo();
        check("runs: undo takes the run back", d.get(id)->text->runs.empty(), "");
    }

    // ── s047 m3: the font's real faces; the box by its handles ──────────────
    std::cout << "\n-- text: faces, stretch, the box in text space (s047 m3) --\n";
    {
        const auto fams = T::families();
        bool listed = false, ordered = true, found = true;
        std::string where;
        for (std::size_t k = 0; k < fams.size() && k < 12; ++k) {   // any machine's fonts: rules, not names
            const auto fs = T::faces(fams[k]);
            listed = listed || !fs.empty();
            for (std::size_t i = 1; i < fs.size(); ++i) {
                const auto& a = fs[i - 1];
                const auto& b = fs[i];
                if (a.stretch > b.stretch || (a.stretch == b.stretch && (a.weight > b.weight || (a.weight == b.weight && a.italic > b.italic)))) {
                    ordered = false; where = fams[k];
                }
            }
            for (std::size_t i = 0; i < fs.size(); ++i)
                if (T::nearest_face(fs, fs[i].weight, fs[i].italic, fs[i].stretch) != static_cast<int>(i)) { found = false; where = fams[k] + " " + fs[i].name; }
        }
        check("faces: an installed family lists its faces", listed, "");
        check("faces: narrow to wide, light to heavy, upright first", ordered, where);
        check("faces: each face is its own nearest", found, where);
        check("faces: a family not installed has none", T::faces("No Such Family Klimt").empty(), "");
        T::Spec s;
        s.text = "wide";
        s.stretch = 9;
        check("faces: stretch out of range is refused", !T::refusal(s).empty() && T::normalised(s).stretch == 8, "");
        T::Spec c;
        c.text = "abc";
        T::Look narrow; narrow.stretch = 2;
        check("faces: a run keeps its stretch; the layer's own is dropped", T::styled(c, 0, 2, narrow).runs.size() == 1 &&
              [&] { T::Look n4; n4.stretch = 4; return T::styled(c, 0, 2, n4).runs.empty(); }(), "");
        T::Spec m;
        m.text = "x"; m.x = 10; m.y = 20; m.box_w = 100; m.box_h = 50;
        m.m = {0, 1, -1, 0, 300, 0};   // turned a quarter
        const auto f = T::frame(m);
        const auto t0 = T::to_text(m, f[0].x, f[0].y), t2 = T::to_text(m, f[2].x, f[2].y);
        check("box: to_text undoes the matrix (the frame's corners are the box's)",
              std::abs(t0.x - 10) < 1e-9 && std::abs(t0.y - 20) < 1e-9 && std::abs(t2.x - 110) < 1e-9 && std::abs(t2.y - 70) < 1e-9,
              num(t0.x) + "," + num(t0.y) + " " + num(t2.x) + "," + num(t2.y));
    }

    // ── s048 m1: hyphenation -- Composix's Liang patterns in box type ────────
    std::cout << "\n-- text: hyphenation (s048 m1) --\n";
    {
        check("hyphen: the patterns are in (hy-phen-ation)", T::hyphenated("hyphenation") == "hy-phen-ation", T::hyphenated("hyphenation"));
        check("hyphen: in German too", !T::hyphen_points("Silbentrennung", -1, -1, "de").empty(), "");
                const auto width = [](const std::string& t) { T::Spec p; p.text = t; p.size = 40; const auto f = T::frame(p); return f[1].x - f[0].x; };
        T::Spec s;
        s.text = "representation";
        s.size = 40; s.x = 10; s.y = 10;
        // Wide enough for "represent-": a character wrap would break after the
        // t (9); the patterns have no point there -- rep-re-sen-ta-tion -- so 8.
        s.box_w = std::max(width("represent-"), width("represent‐")) + 1;
        s.hyphenate = true;
        const int len = static_cast<int>(s.text.size());
        check("hyphen: a long word breaks at a hyphenation point", T::line_home(s, len) == 8, num(T::line_home(s, len)));
        check("hyphen: not hyphenating, Pango breaks it where the letters run out",
              [&] { T::Spec o = s; o.hyphenate = false; return T::line_home(o, len) != 8; }(), "");
        const Ink one = ink_of(T::render(s, 600, 400));
        T::Spec bare; bare.text = "represen"; bare.size = 40; bare.x = 10; bare.y = 10 + (T::caret(s, 0).y1 - T::caret(bare, 0).y1);
        check("hyphen: and the hyphen is drawn at the break", one.any() && one.x1 > ink_of(T::render(bare, 600, 400)).x1 + 3, "");
        bool round = true;
        for (int i = 0; i <= len; ++i) {
            const auto c = T::caret(s, i);
            if (T::index_at(s, c.x0 + 0.01, (c.y0 + c.y1) / 2) != i && !(i == 8 && T::index_at(s, c.x0 + 0.01, (c.y0 + c.y1) / 2) == 8)) round = false;
        }
        check("hyphen: caret and hit-test agree through the soft hyphens", round, "");
        int steps = 0;
        for (int i = 0; i < len && steps < 100; i = T::step(s, i, 1)) ++steps;
        check("hyphen: right steps one letter at a time -- no stop at a soft hyphen", steps == len, num(steps));
        steps = 0;
        for (int i = len; i > 0 && steps < 100; i = T::step(s, i, -1)) ++steps;
        check("hyphen: and left too", steps == len, num(steps));
        check("hyphen: a word step takes the whole word", T::word_step(s, 0, 1) == len, num(T::word_step(s, 0, 1)));
        check("hyphen: a selection over the word is one quad a line", T::range(s, 0, len).size() == 2, "");
        T::Spec off = s;
        off.hyphenate = false;
        check("hyphen: the text itself never holds the hyphens", T::edited(s, len, len, "s").text == "representations", "");
        check("hyphen: point type doesn't hyphenate", [&] { T::Spec q = s; q.box_w = 0; return T::line_home(q, len) == 0; }(), "");
        T::Spec shortw = s;
        shortw.text = "abcde abcde";
        check("hyphen: words under six letters stay whole", T::line_home(shortw, 11) == 0 || T::line_home(shortw, 11) == 6, num(T::line_home(shortw, 11)));
        T::Look red; red.colour = std::array<float, 3>{1, 0, 0};
        const auto r = T::styled(s, 8, len, red);
        check("hyphen: a run after a break point is set on its letters", r.runs.size() == 1 && r.runs[0].start == 8 && ink_of(T::render(r, 600, 400)).any(), "");
    }

    // ── s048 m2: caps, super- and subscript, OpenType features ───────────────
    std::cout << "\n-- text: caps, scripts, OpenType features (s048 m2) --\n";
    {
        const auto ink = [](const T::Spec& x) { return ink_of(T::render(x, 600, 400)); };
        T::Spec low; low.text = "ace"; low.size = 60; low.x = 20; low.y = 200;
        T::Spec up = low; up.text = "ACE";
        T::Look caps; caps.caps = 1;
        const Ink l = ink(low), u = ink(up), c = ink(T::styled(low, 0, 3, caps));
        check("caps: ALL CAPS sets the capitals (as tall as typed ones)", c.any() && std::abs(c.y0 - u.y0) <= 1 && c.y0 < l.y0 - 5,
              num(c.y0) + " vs " + num(u.y0) + " (lower " + num(l.y0) + ")");
        check("caps: the text keeps what was typed", T::styled(low, 0, 3, caps).text == "ace", "");
        T::Look sc; sc.caps = 2;
        check("caps: small caps changes the letters", T::render(T::styled(low, 0, 3, sc), 600, 400).a != T::render(low, 600, 400).a, "");
        T::Spec h; h.text = "H"; h.size = 60; h.x = 20; h.y = 200;
        const Ink plain = ink(h);
        T::Look sup; sup.position = 1;
        T::Look sub; sub.position = 2;
        const Ink sp = ink(T::styled(h, 0, 1, sup)), sb = ink(T::styled(h, 0, 1, sub));
        const double ph = plain.y1 - plain.y0;
        check("scripts: superscript rises a third of the size", sp.any() && std::abs((plain.y1 - sp.y1) - 20) <= 2, num(plain.y1 - sp.y1));
        check("scripts: at 58.3% of it", std::abs((sp.y1 - sp.y0) - ph * T::kScriptSize) <= 2, num(sp.y1 - sp.y0) + " of " + num(ph));
        check("scripts: subscript drops a third", sb.any() && std::abs((sb.y1 - plain.y1) - 20) <= 2, num(sb.y1 - plain.y1));
        T::Spec two = h; two.text = "Hx\nH";
        check("scripts: a raised letter doesn't move its line",
              std::abs(T::caret(T::styled(two, 1, 2, sup), 3).y0 - T::caret(two, 3).y0) < 0.01 &&
              std::abs(T::caret(T::styled(two, 1, 2, sup), 0).y0 - T::caret(two, 0).y0) < 0.01, "");
        T::Look ot; ot.ligatures = false; ot.figures = 2; ot.fig_space = 2; ot.frac = true; ot.font_kern = false;
        check("features: as HarfBuzz settings", T::ot_settings(ot) == "liga=0,clig=0,kern=0,lnum=0,onum=1,pnum=0,tnum=1,frac=1", T::ot_settings(ot));
        T::Look dflt; dflt.ligatures = true; dflt.font_kern = true; dflt.caps = 0; dflt.position = 0; dflt.figures = 0;
        check("features: the defaults are no setting (no run)", T::styled(h, 0, 1, dflt).runs.empty() && T::ot_settings(dflt).empty(), "");
        T::Spec f; f.text = "1/2 office"; f.size = 40; f.x = 10; f.y = 100;
        T::Look fr; fr.frac = true; fr.ligatures = false;
        check("features: a run with features sets", ink(T::styled(f, 0, 10, fr)).any(), "");
        check("features: look_at fills them in", *T::look_at(T::styled(f, 0, 3, fr), 1).frac && !*T::look_at(f, 1).frac && *T::look_at(f, 1).ligatures, "");
    }

    // ── s048 m3: point type's handles scale it ───────────────────────────────
    std::cout << "\n-- text: point type scaled by its handles (s048 m3) --\n";
    {
        T::Spec s; s.text = "Adele"; s.size = 40; s.x = 100; s.y = 120;
        const auto f = T::frame(s);
        const auto pv = T::to_text(s, f[2].x, f[2].y);   // the bottom-right corner stays
        const auto u = T::scaled(s, 2, 2, pv.x, pv.y);
        const auto g = T::frame(u);
        check("scale: a corner's uniform scale lands in the size", std::abs(u.size - 80) < 1e-9 && u.m[0] == 1 && u.m[3] == 1, num(u.size));
        check("scale: about the opposite corner, which stays", std::abs(g[2].x - f[2].x) < 0.01 && std::abs(g[2].y - f[2].y) < 0.01,
              num(g[2].x) + "," + num(g[2].y) + " vs " + num(f[2].x) + "," + num(f[2].y));
        check("scale: and the frame doubles", std::abs((g[1].x - g[0].x) - 2 * (f[1].x - f[0].x)) < 0.5, "");
        const auto w = T::scaled(s, 1.5, 1, pv.x, pv.y);
        check("scale: a side stretches it -- live, in the matrix", w.size == 40 && std::abs(w.m[0] - 1.5) < 1e-9 && w.m[3] == 1, "");
        check("scale: nothing for a zero scale", T::scaled(s, 0, 1, 0, 0) == s, "");
    }
}
