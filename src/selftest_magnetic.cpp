// klimt_selftest -- the Magnetic Lasso's live wire (s079).
#include "core/Magnetic.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace S = klimt::core::select;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

void selftest_magnetic(const Check& check) {
    std::cout << "\n-- select: the Magnetic Lasso (s079) --\n";
    constexpr double kPi = 3.14159265358979323846;
    // A white disc, radius 60, at (100, 100) on black, 200 x 200.
    const auto disc = [](int x, int y) { return std::hypot(x + 0.5 - 100, y + 0.5 - 100) <= 60 ? 1.f : 0.f; };
    const auto r_of = [](S::Pt p) { return std::hypot(p.x - 100, p.y - 100); };
    S::MagneticTrace t;
    t.start({162, 100}, disc, 200, 200, {}, 4);
    check("magnetic: starts open, on the edge", t.active() && std::abs(r_of(t.fixed().front()) - 60) < 2, std::to_string(r_of(t.fixed().front())));
    check("magnetic: the pointer snaps to the edge within Width", std::abs(r_of(t.snap({155, 100})) - 60) < 2 && !std::isnan(t.snap({155, 100}).x), "");
    check("magnetic: no edge within Width -- the pointer as it is", t.snap({100, 100}).x == 100, "");
    check("magnetic: the edge is strong at the rim, nothing inside", t.edge(160, 100) > 0.4f && t.edge(100, 100) == 0.f, "");
    {
        const auto p = t.path({160.5, 100.5}, {100.5, 40.5});   // a quarter round: the chord's middle is 42 from the middle
        double worst = 0;
        for (const auto& q : p) worst = std::max(worst, std::abs(r_of(q) - 60));
        check("magnetic: the wire follows the circle, not the chord", p.size() > 60 && worst < 2.5, std::to_string(worst));
        check("magnetic: from one end to the other", std::abs(p.front().x - 160.5) < 1e-9 && std::abs(p.back().y - 40.5) < 1e-9, "");
    }
    // Round the disc, the pointer 3 px off the edge outside: anchors are laid as it goes.
    for (int a = 5; a <= 355; a += 5) {
        const double r = a * kPi / 180;
        t.move({100 + 63 * std::cos(r), 100 - 63 * std::sin(r)});
    }
    const auto laid = t.anchors().size();
    check("magnetic: anchors laid as the wire grows (Frequency)", laid >= 5, std::to_string(laid));
    check("magnetic: Backspace takes the last", t.back() && t.anchors().size() == laid - 1, "");
    t.finish();
    check("magnetic: closed", t.done(), "");
    const auto pts = t.points();
    double area = 0, worst = 0;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const auto& p = pts[i];
        const auto& q = pts[(i + 1) % pts.size()];
        area += p.x * q.y - q.x * p.y;
        worst = std::max(worst, std::abs(r_of(p) - 60));
    }
    area = std::abs(area) / 2;
    check("magnetic: the outline hugs the disc", worst < 2.5, std::to_string(worst));
    check("magnetic: ...and holds its area (within 5 %)", std::abs(area / (kPi * 60 * 60) - 1) < 0.05, std::to_string(area));
    {
        S::MagneticTrace c;
        c.start({162, 100}, disc, 200, 200, {}, 4);
        c.click({100, 37});
        c.click({38, 100});
        check("magnetic: clicks lay anchors", c.anchors().size() == 3, std::to_string(c.anchors().size()));
        c.click({161, 101});
        check("magnetic: a click on the start closes", c.done() && c.points().size() > 100, "");
        S::MagneticTrace o;
        o.start({162, 100}, disc, 200, 200, {}, 4);
        check("magnetic: Backspace with only the start: nothing", !o.back() && o.active(), "");
        o.cancel();
        check("magnetic: Esc drops it", !o.active() && o.points().empty(), "");
    }
    {
        S::MagneticTrace s;
        s.start({0.5, 0.5}, nullptr, 50, 50, {}, 4);
        const auto p = s.path({0.5, 0.5}, {10.5, 0.5});
        check("magnetic: nothing to read -- a straight wire", p.size() == 11 && p.back().x == 10.5 && p[5].y == 0.5, "");
    }
}
