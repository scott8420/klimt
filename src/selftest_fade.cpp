// klimt_selftest -- EDIT > FADE (s020): what can be faded, the mix (Normal,
// an erase, Multiply, Screen, Dissolve), one step of its own, shared tiles.
#include "core/Document.hpp"
#include "core/Fade.hpp"
#include "core/Selection.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace F = klimt::core::fade;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
bool near(double a, double b, double eps = 0.02) { return std::abs(a - b) <= eps; }
std::string px(const P::Px& p) {
    return std::to_string(p.c[0]) + "," + std::to_string(p.c[1]) + "," + std::to_string(p.c[2]) + "," + std::to_string(p.c[3]);
}
}  // namespace

void selftest_fade(const Check& check) {
    std::cout << "\n-- edit > fade (s020) --\n";
    core::Document d;
    core::DocJournal j{d};
    d.set_door([&] { return j.open(); });
    L::load_fixture(d);
    const auto at = [&](int x, int y) { return d.get("L4")->pixels->get(x, y); };
    const P::Px red = at(150, 150);
    check("fade: nothing to fade on a fresh document", !F::source(j, d), "");

    // A fill on the square: blue.
    L::fill(j, d, "L4", {120, 120, 60, 60}, P::rgba(0, 0, 1, 1));
    auto src = F::source(j, d);
    check("source: the fill, on L4", src && src->id == "L4" && src->after == d.get("L4")->pixels, src ? src->what : "none");
    const auto steps = j.size();
    check("fade Normal 50 %: ONE step, \"Fade ...\"",
          L::fade(j, d, src->id, src->before, src->after, 0.5, P::Blend::Normal, src->what) && j.size() == steps + 1 &&
              j.undo_label().rfind("Fade ", 0) == 0, j.undo_label());
    check("fade Normal 50 %: half red, half blue", near(at(150, 150).c[0], 0.5 * red.c[0]) && near(at(150, 150).c[2], 0.5) &&
              near(at(150, 150).c[3], 1), px(at(150, 150)));
    check("fade: outside the fill, untouched", at(110, 110) == red, px(at(110, 110)));
    check("source: not a fade of a fade", !F::source(j, d), "");
    j.undo();
    check("undo: the fade off, the fill stays", near(at(150, 150).c[2], 1) && near(at(150, 150).c[0], 0), px(at(150, 150)));
    check("undo: Fade is offered again", F::source(j, d).has_value(), "");

    // Multiply and Screen at 100 %.
    src = F::source(j, d);
    L::fade(j, d, src->id, src->before, src->after, 1.0, P::Blend::Multiply, src->what);
    check("fade Multiply 100 %: blue x red = black", near(at(150, 150).c[0], 0) && near(at(150, 150).c[2], 0), px(at(150, 150)));
    j.undo();
    L::fade(j, d, src->id, src->before, src->after, 1.0, P::Blend::Screen, src->what);
    check("fade Screen 100 %: blue + red = magenta", near(at(150, 150).c[0], red.c[0]) && near(at(150, 150).c[2], 1), px(at(150, 150)));
    j.undo();
    // Dissolve 50 %: each pixel the one or the other.
    {
        auto out = F::faded(src->before, *src->after, 0.5, P::Blend::Dissolve);
        int blue = 0, other = 0, mixed = 0;
        for (int y = 120; y < 180; ++y)
            for (int x = 120; x < 180; ++x) {
                const auto p = out->get(x, y);
                if (near(p.c[2], 1) && near(p.c[0], 0)) ++blue;
                else if (near(p.c[0], red.c[0]) && near(p.c[2], red.c[2])) ++other;
                else ++mixed;
            }
        check("fade Dissolve 50 %: dithered, not mixed", mixed == 0 && blue > 1200 && other > 1200,
              std::to_string(blue) + " / " + std::to_string(other) + " / " + std::to_string(mixed));
    }
    // Only the changed tiles are new.
    {
        auto out = F::faded(src->before, *src->after, 0.3, P::Blend::Normal);
        bool shared = true;
        for (int r = 0; r < out->rows(); ++r)
            for (int c = 0; c < out->cols(); ++c)
                if (src->after->same_tile(*src->before, c, r)) shared = shared && out->same_tile(*src->after, c, r);
        check("faded: untouched tiles stay shared", shared, "");
        int calls = 0;
        check("faded: a stop is a nullopt", !F::faded(src->before, *src->after, 0.3, P::Blend::Normal, [&] { return ++calls > 0; }), "");
    }

    // An erase faded half-way is half erased.
    L::clear(j, d, "L4", {120, 120, 60, 60});
    src = F::source(j, d);
    check("source: the erase", src.has_value(), "");
    L::fade(j, d, src->id, src->before, src->after, 0.5, P::Blend::Normal, src->what);
    check("fade an erase 50 %: half the alpha, the colour kept", near(at(150, 150).c[3], 0.5) && near(at(150, 150).c[2], 1), px(at(150, 150)));

    // Not fadeable: a rename (no pixels), a step on two layers, the layer moved on.
    L::rename(j, d, "L4", "Square");
    check("source: not a rename", !F::source(j, d), "");
    L::set_opacity(j, d, std::vector<std::string>{"L4", "L5"}, 50);
    check("source: not a step on two layers", !F::source(j, d), "");
    check("supports: CMYK Normal only", F::supports({P::Depth::U8, P::Mode::CMYK, true}, P::Blend::Normal) &&
                                         !F::supports({P::Depth::U8, P::Mode::CMYK, true}, P::Blend::Multiply), "");
    check("fade: the tree's invariants, no stray writes", d.check().empty() && d.strays() == 0, d.check());
}
