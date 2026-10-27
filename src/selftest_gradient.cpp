// klimt_selftest -- the Gradient tool (s012 m4).
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/Gradient.hpp"
#include "core/Selection.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace G = klimt::core::gradient;
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
bool near(float a, float b, float tol = 0.02f) { return std::abs(a - b) <= tol; }
}  // namespace

void selftest_gradient(const Check& check) {
    std::cout << "\n-- gradient (s012) --\n";
    {
        G::Spec s;
        s.a = {100, 0};
        s.b = {300, 0};
        check("gradient: linear -- 0 before the start, 1 past the end", G::position(s, 10, 50) == 0 && G::position(s, 500, 50) == 1, "");
        check("gradient: linear -- half way is a half", near(static_cast<float>(G::position(s, 199.5, 77)), 0.5f, 1e-6f), "");
        s.shape = G::Shape::Radial;
        check("gradient: radial -- by the distance from the start", near(static_cast<float>(G::position(s, 99.5, 99.5)), 0.5f, 1e-3f), "");
    }
    {
        Fix t;
        G::Spec s;
        s.a = {0, 0};
        s.b = {600, 0};
        s.from = P::rgba(1, 0, 0, 1);
        s.to = P::rgba(0, 0, 1, 1);
        check("gradient: on a layer, one step", G::apply(t.j, t.d, "L5", s) && t.j.undo_label() == "Gradient", t.j.undo_label());
        const auto l = t.at("L5", 2, 50), m = t.at("L5", 300, 50), r = t.at("L5", 597, 50);
        check("gradient: red to blue across", l.c[0] > 0.98f && r.c[2] > 0.98f && near(m.c[0], 0.5f) && near(m.c[2], 0.5f) && m.c[3] > 0.99f,
              std::to_string(m.c[0]));
        // Dithered: neighbours along a row differ by at most a step; no flat bands longer than the step size.
        bool smooth = true;
        for (int x = 1; x < 600 && smooth; ++x) smooth = std::abs(t.at("L5", x, 10).c[0] - t.at("L5", x - 1, 10).c[0]) <= 3.f / 255;
        check("gradient: smooth, no jumps", smooth, "");
    }
    {
        Fix t;
        core::layers::load_selection(t.d, {100, 100, 100, 100});
        G::Spec s;
        s.a = {0, 0};
        s.b = {600, 0};
        s.from = P::rgba(0, 1, 0, 1);
        s.to = P::rgba(0, 1, 0, 0);   // foreground to transparent
        G::apply(t.j, t.d, "L5", s);
        check("gradient: only inside the selection", t.at("L5", 150, 150).c[1] > 0.99f && t.at("L5", 50, 50).c[3] <= 0.f, "");
        check("gradient: to transparent fades out", near(t.at("L5", 150, 150).c[3], 1.f - 150.5f / 600, 0.02f),
              std::to_string(t.at("L5", 150, 150).c[3]));
    }
    {
        Fix t;
        G::Spec s;
        s.a = {0, 0};
        s.b = {0, 400};
        s.from = P::rgba(0, 0, 0, 1);
        s.to = P::rgba(0, 0, 0, 1);
        s.opacity = 0.5;
        G::apply(t.j, t.d, "L1", s);   // over the white Background
        check("gradient: at 50% opacity", near(t.at("L1", 200, 200).c[0], 0.5f), std::to_string(t.at("L1", 200, 200).c[0]));
    }
    {
        // On a mask: hide from nothing (left) to all (right).
        Fix t;
        core::layers::add_mask(t.j, t.d, "L4", core::layers::MaskFrom::RevealAll);
        G::Spec s;
        s.a = {100, 0};
        s.b = {300, 0};
        s.from.c[0] = 0;
        s.to.c[0] = 1;
        check("gradient: on a mask", G::apply(t.j, t.d, "L4", s, true) && t.j.undo_label() == "Gradient on Mask", t.j.undo_label());
        const auto& m = *t.d.get("L4")->mask;
        check("gradient: the mask fades the layer out", S::coverage(m, 100, 150) < 0.02f && near(S::coverage(m, 200, 150), 0.5f) &&
                                                      S::coverage(m, 299, 150) > 0.98f, "");
        const auto seen = core::sample_composite(t.d.image(), t.d.snapshot(), 200, 150, 0);
        check("gradient: ...half the red shows half way", near(seen.g, 0.5f, 0.03f) && near(seen.r, 1.f, 0.01f), std::to_string(seen.g));
    }
    {
        Fix t;
        core::layers::set_locks(t.j, t.d, "L5", core::lock::Pixels);
        G::Spec s;
        s.b = {10, 10};
        check("gradient: the Pixels lock refuses", !G::apply(t.j, t.d, "L5", s), "");
    }
}
