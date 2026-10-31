// klimt_selftest -- RENDER > LENS FLARE, DISTORT > SHEAR (s044). docs/filters.md.
// Rules, not looks: the flare is brightest at its centre and fades; it
// scales with Brightness; it is screened (black shows the light, white stays
// white, clear stays clear); the ghosts lie on the line through the middle;
// the movie lens streaks across; the preview's rectangle == the result; the
// settings survive a .klimt.
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Filter.hpp"
#include "core/Live.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

#include <unistd.h>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace F = klimt::core::filter;
using Check = std::function<void(const std::string&, bool, const std::string&)>;
using core::FilterKind;

namespace {
std::string num(double v) { return std::to_string(v); }
P::Format f32() { P::Format f; f.depth = P::Depth::F32; return f; }
bool same_in(const P::TileMap& a, const P::TileMap& b, const P::Rect& r, float eps = 0) {
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            for (int c = 0; c < 4; ++c)
                if (std::abs(a.get(x, y).c[static_cast<std::size_t>(c)] - b.get(x, y).c[static_cast<std::size_t>(c)]) > eps) return false;
    return true;
}
double lum(const F::Light3& l) { return 0.299 * l.r + 0.587 * l.g + 0.114 * l.b; }
bool saved_back(core::Document& d, const std::string& tag, std::string& err) {
    namespace fs = std::filesystem;
    const auto path = (fs::temp_directory_path() / ("klimt_s044" + tag + "_" + std::to_string(::getpid()) + ".klimt")).string();
    core::file::Contents c;
    c.image = d.image();
    c.layers = d.snapshot();
    c.next_id = d.next_id();
    if (!core::file::save_klimt(path, c, err)) return false;
    const auto back = core::file::open(path, err);
    fs::remove(path);
    return back && back->layers.at("L4").filters == d.get("L4")->filters;
}
}  // namespace

void selftest_render(const Check& check) {
    std::cout << "\n-- filters: Render -- Lens Flare (s044 m1) --\n";
    {
        auto lf = F::fresh(FilterKind::LensFlare);
        check("flare: under Render; Brightness 100, 50-300mm Zoom, centre (35 %, 35 %); live, placed",
              F::group(FilterKind::LensFlare) == "Render" && lf.amount == 100 && lf.axis == 0 && lf.cx == 35 && lf.cy == 35 &&
                  F::livable(FilterKind::LensFlare) && F::placed(lf), "");
        auto t = lf; t.amount = 999; t.axis = 9; t.cx = 140; t.cy = -3;
        auto g = F::fresh(FilterKind::GaussianBlur); g.cx = 10;
        check("flare: normalised -- Brightness 10..300, four lenses, the centre 0..100 %; another kind's centre back",
              F::normalised(t).amount == 300 && F::normalised(t).axis == 3 && F::normalised(t).cx == 100 && F::normalised(t).cy == 0 &&
                  F::normalised(g).cx == 50, "");
        const int W = 400, H = 300;
        lf.cx = 30; lf.cy = 40;
        const double cx = 120, cy = 120;
        const auto at = [&](const core::Filter& f, double x, double y) { return F::flare_at(f, W, H, x, y); };
        check("flare: white-hot at its centre", at(lf, cx, cy).r >= 1.f && at(lf, cx, cy).g >= 1.f && at(lf, cx, cy).b >= 1.f, num(at(lf, cx, cy).r));
        // It fades: the glow's mean on rings round the centre falls outward (rays and ghosts aside).
        const auto ring = [&](double r) {
            double s = 0;
            for (int k = 0; k < 72; ++k) s += lum(at(lf, cx + r * std::cos(k * 0.0872665), cy + r * std::sin(k * 0.0872665)));
            return s / 72;
        };
        check("flare: fades from the centre (rings at 5, 20, 60 px)", ring(5) > ring(20) && ring(20) > ring(60), num(ring(5)) + " " + num(ring(20)) + " " + num(ring(60)));
        auto twice = lf; twice.amount = 200;
        check("flare: Brightness 200 % -- twice the light", std::abs(lum(at(twice, 200, 200)) - 2 * lum(at(lf, 200, 200))) < 1e-4, "");
        // The ghosts: on the line from the centre through the middle (200, 150), past it.
        const double gx = cx + 1.15 * (200 - cx), gy = cy + 1.15 * (150 - cy);   // the zoom's third ghost
        const double off = std::hypot(gx - cx, gy - cy);
        const double px = cx + off * std::cos(std::atan2(gy - cy, gx - cx) + 1.2), py = cy + off * std::sin(std::atan2(gy - cy, gx - cx) + 1.2);
        check("flare: a ghost on the line through the middle -- brighter than as far off the line", lum(at(lf, gx, gy)) > lum(at(lf, px, py)) + 0.02,
              num(lum(at(lf, gx, gy))) + " vs " + num(lum(at(lf, px, py))));
        auto mv = lf; mv.axis = 3;
        check("flare: Movie Prime streaks across, not down", lum(at(mv, cx + 100, cy)) > 2 * lum(at(mv, cx, cy + 100)),
              num(lum(at(mv, cx + 100, cy))) + " vs " + num(lum(at(mv, cx, cy + 100))));
        // Screened: black shows the light, white stays white, clear stays clear.
        const auto black = P::TileMap(f32(), W, H).fill({0, 0, W, H}, P::rgba(0, 0, 0, 1));
        const auto white = P::TileMap(f32(), W, H).fill({0, 0, W, H}, P::rgba(1, 1, 1, 1));
        const auto clear = P::TileMap(f32(), W, H);
        const auto ob = F::filtered(black, lf, nullptr), ow = F::filtered(white, lf, nullptr), oc = F::filtered(clear, lf, nullptr);
        const auto l2 = at(lf, 250.5, 180.5);
        check("flare: on black -- the light itself (clamped to 1)", ob && std::abs(ob->get(250, 180).c[0] - std::min(l2.r, 1.f)) < 1e-5f &&
              std::abs(ob->get(250, 180).c[2] - std::min(l2.b, 1.f)) < 1e-5f && ob->get(cx, cy).c[0] == 1.f, "");
        check("flare: white stays white; clear stays clear; alpha kept", ow && same_in(white, *ow, {0, 0, W, H}) && oc && same_in(clear, *oc, {0, 0, W, H}) &&
              ob->get(10, 10).c[3] == 1.f, "");
        const P::Rect view{80, 60, 160, 120};
        check("flare: the preview's rectangle == the result there", same_in(*ob, *F::filtered(black, lf, nullptr, false, &view), view), "");
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        auto lv = lf; lv.axis = 1; lv.amount = 150; lv.cx = 12.5; lv.cy = 80;
        check("flare: live", L::add_live(j, d, "L4", lv), "");
        std::string err;
        check("flare: Brightness, lens and centre back from the file", saved_back(d, "l", err), err);
    }

    std::cout << "\n-- filters: Distort -- Shear (s044 m2) --\n";
    {
        auto sh = F::fresh(FilterKind::Shear);
        check("shear: under Distort; a straight line (0,0)-(1,0), Wrap Around; live",
              F::group(FilterKind::Shear) == "Distort" && sh.shear.size() == 2 && sh.shear[0] == (std::array<double, 2>{0, 0}) &&
                  sh.shear[1] == (std::array<double, 2>{1, 0}) && sh.wrap && F::livable(FilterKind::Shear), "");
        auto t = sh; t.shear = {{0.7, 3}, {0.2, -0.5}, {0.205, 0.4}};
        const auto n = F::normalised(t);
        check("shear: normalised -- sorted, clamped, two at one height one (the later), both ends added",
              n.shear.size() == 4 && n.shear[0] == (std::array<double, 2>{0, 0.4}) && n.shear[1] == (std::array<double, 2>{0.2, 0.4}) &&
                  n.shear[2] == (std::array<double, 2>{0.7, 1}) && n.shear[3] == (std::array<double, 2>{1, 1}), "");
        auto many = sh; many.shear.clear();
        for (int i = 0; i <= 30; ++i) many.shear.push_back({i / 30.0, 0.0});
        check("shear: at most 16 points, the ends kept", F::normalised(many).shear.size() == 16 && F::normalised(many).shear.back()[0] == 1, "");
        auto g = F::fresh(FilterKind::GaussianBlur); g.shear = {{0, 1}};
        check("shear: another kind's curve back to straight", F::normalised(g).shear == sh.shear, "");
        // The curve goes through its points, and is smooth between.
        auto c = sh; c.shear = {{0, 0}, {0.5, 0.6}, {1, -0.2}};
        c = F::normalised(c);
        bool smooth = true;
        for (int i = 1; i < 100; ++i) smooth = smooth && std::abs(F::shear_at(c, i / 100.0) - F::shear_at(c, (i - 1) / 100.0)) < 0.05;
        check("shear: the curve through its points, smooth between", std::abs(F::shear_at(c, 0.5) - 0.6) < 1e-9 && std::abs(F::shear_at(c, 0)) < 1e-9 &&
              std::abs(F::shear_at(c, 1) + 0.2) < 1e-9 && smooth, "");
        check("shear: a click finds the point near it, not one far off",
              F::shear_hit(c, 0.51, 0.58, 0.03, 0.05) == 1 && F::shear_hit(c, 0.3, 0.0, 0.03, 0.05) == -1, "");
        // A straight line: nothing moves. A line all at 0.5: every row a quarter of the width right.
        const int W = 200, H = 100;
        P::TileMap rp(f32(), W, H);
        for (int x = 0; x < W; ++x) rp = rp.fill({x, 0, 1, H}, P::rgba(static_cast<float>(x) / W, 0.3f, 0.6f, 1));
        check("shear: straight -- nothing moves", same_in(rp, *F::filtered(rp, sh, nullptr), {0, 0, W, H}, 1e-6f), "");
        auto q = sh; q.shear = {{0, 0.5}, {1, 0.5}};
        const auto moved = F::filtered(rp, q, nullptr);
        check("shear: at 0.5 everywhere -- every row 50 px right; Wrap Around brings the right edge round",
              moved && std::abs(moved->get(120, 30).c[0] - 70.f / W) < 1e-4f && std::abs(moved->get(20, 70).c[0] - 170.f / W) < 1e-4f,
              moved ? num(moved->get(120, 30).c[0] * W) : "");
        auto rep = q; rep.wrap = false;
        const auto rp2 = F::filtered(rp, rep, nullptr);
        check("shear: Repeat Edge -- the left edge's colour fills in", rp2 && rp2->get(20, 70).c[0] == 0.f, "");
        // A bend: the top row stays, the bottom moves.
        auto bend = sh; bend.shear = {{0, 0}, {1, 0.4}};
        const auto bn = F::filtered(rp, bend, nullptr);
        check("shear: a slope -- the top row stays, lower rows move more",
              bn && std::abs(bn->get(100, 0).c[0] - rp.get(100, 0).c[0]) < 0.01f && bn->get(100, 99).c[0] < bn->get(100, 50).c[0] &&
                  bn->get(100, 50).c[0] < bn->get(100, 1).c[0], "");
        // Through a selection: only its bounds' rows, by its width.
        const auto sel = P::TileMap(P::Format{P::Depth::U8, P::Mode::Gray, false}, W, H).fill({50, 20, 100, 60}, P::rgba(1, 1, 1, 1));
        const auto sq = F::filtered(rp, q, &sel);
        check("shear: in a selection -- rows move by half ITS width (25 px), wrapping inside it; outside untouched",
              sq && std::abs(sq->get(100, 40).c[0] - 75.f / W) < 1e-4f && std::abs(sq->get(60, 40).c[0] - 135.f / W) < 1e-4f &&
                  same_in(rp, *sq, {0, 0, W, 20}), sq ? num(sq->get(60, 40).c[0] * W) : "");
        const P::Rect view{30, 10, 90, 60};
        check("shear: the preview's rectangle == the result there", same_in(*bn, *F::filtered(rp, bend, nullptr, false, &view), view), "");
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        auto lv = c; lv.wrap = false;
        check("shear: live", L::add_live(j, d, "L4", lv), "");
        std::string err;
        check("shear: the curve and Undefined Areas back from the file", saved_back(d, "s", err), err);
    }

    std::cout << "\n-- filters: Distort in a selection fits its exact bounds (s044) --\n";
    {
        // A 300 x 300 ramp, a 60 x 60 selection at (100, 100): Twirl's ellipse is that square's, not the 256 px tile's.
        P::TileMap rp(f32(), 300, 300);
        for (int y = 0; y < 300; ++y)
            for (int x = 0; x < 300; x += 1) rp = rp.fill({x, y, 1, 1}, P::rgba(static_cast<float>(x) / 300, static_cast<float>(y) / 300, 0.5f, 1));
        const auto sel = P::TileMap(P::Format{P::Depth::U8, P::Mode::Gray, false}, 300, 300).fill({100, 100, 60, 60}, P::rgba(1, 1, 1, 1));
        auto tw = F::fresh(FilterKind::Twirl);
        tw.angle = 300;
        const auto o = F::filtered(rp, tw, &sel);
        const auto moved = [&](int x, int y) { return std::abs(o->get(x, y).c[0] - rp.get(x, y).c[0]) + std::abs(o->get(x, y).c[1] - rp.get(x, y).c[1]) > 1e-4f; };
        check("bounds: Twirl in a 60 px selection -- its middle turned, its corner (outside its ellipse) not",
              o && moved(135, 125) && !moved(102, 102) && !moved(157, 157), "");
    }
}
