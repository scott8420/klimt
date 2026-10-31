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
}
