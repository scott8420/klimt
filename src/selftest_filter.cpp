// klimt_selftest -- FILTERS (s014): the blur line, the four filters on a map
// (premultiplied edges, the selection, a transparency lock, the preview's
// rectangle == the result), the verb and its refusals.
#include "core/Document.hpp"
#include "core/Filter.hpp"
#include "core/Composite.hpp"
#include "core/Canvas.hpp"
#include "core/File.hpp"
#include "core/Live.hpp"
#include "core/Prefs.hpp"
#include "core/Selection.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace F = klimt::core::filter;
using Check = std::function<void(const std::string&, bool, const std::string&)>;
using core::FilterKind;

namespace {
bool near(double a, double b, double eps = 0.01) { return std::abs(a - b) <= eps; }
std::string num(double v) { return std::to_string(v); }

P::Format f32() { P::Format f; f.depth = P::Depth::F32; return f; }

// A 600 x 400 transparent map with a red square (100, 100) 200 x 150.
P::TileMap square(P::Format f = f32()) {
    return P::TileMap(f, 600, 400).fill({100, 100, 200, 150}, P::rgba(1, 0, 0, 1));
}
// Left half dark grey 0.25, right half light 0.75, opaque.
P::TileMap step(P::Format f = f32()) {
    return P::TileMap(f, 600, 400).fill({0, 0, 300, 400}, P::rgba(0.25f, 0.25f, 0.25f, 1)).fill({300, 0, 300, 400}, P::rgba(0.75f, 0.75f, 0.75f, 1));
}
bool same_in(const P::TileMap& a, const P::TileMap& b, const P::Rect& r) {
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            if (!(a.get(x, y) == b.get(x, y))) return false;
    return true;
}
core::Filter blur(double r) { auto f = F::fresh(FilterKind::GaussianBlur); f.radius = r; return f; }
}  // namespace

void selftest_filter(const Check& check) {
    std::cout << "\n-- filters: the blur (s014) --\n";
    for (const double sigma : {0.5, 1.0, 2.0, 5.0, 20.0}) {
        const int n = 401;
        std::vector<float> v(n, 0.f);
        v[200] = 1.f;
        F::blur_line(v.data(), n, 1, sigma);
        double sum = 0, var = 0;
        for (int i = 0; i < n; ++i) { sum += v[static_cast<std::size_t>(i)]; var += v[static_cast<std::size_t>(i)] * (i - 200.0) * (i - 200.0); }
        const bool sym = near(v[190], v[210], 1e-6) && near(v[199], v[201], 1e-6);
        check("blur line sigma " + num(sigma) + ": keeps its mass, symmetric", near(sum, 1, 1e-4) && sym, num(sum));
        if (sigma >= 1)
            check("blur line sigma " + num(sigma) + ": spread ~ sigma^2", std::abs(var - sigma * sigma) <= 0.12 * sigma * sigma + 0.05, num(var));
        std::vector<float> c(n, 0.4f);
        F::blur_line(c.data(), n, 1, sigma);
        check("blur line sigma " + num(sigma) + ": flat stays flat (edges clamp)", near(c[0], 0.4, 1e-5) && near(c[200], 0.4, 1e-5), num(c[0]));
    }
    {
        std::vector<float> v(101, 0.f);
        v[50] = 1.f;
        F::blur_line(v.data(), 101, 1, 1.0);
        check("blur line sigma 1: the Gaussian's peak", near(v[50], 1 / std::sqrt(2 * M_PI), 0.004), num(v[50]));
        // Stride: an interleaved channel blurs alone.
        std::vector<float> w(2 * 101, 0.f);
        w[2 * 50] = 1.f;
        w[2 * 50 + 1] = 7.f;
        F::blur_line(w.data(), 101, 2, 1.0);
        check("blur line: stride keeps the channels apart", near(w[2 * 50], v[50], 1e-6) && near(w[2 * 50 + 1], 7, 1e-6), num(w[101]));
    }
    check("reach: grows with the radius", F::reach(blur(2)) > 0 && F::reach(blur(20)) > F::reach(blur(2)), num(F::reach(blur(2))));
    check("reach: Add Noise reads no neighbours", F::reach(F::fresh(FilterKind::AddNoise)) == 0, "");
    {
        core::Filter f = F::fresh(FilterKind::UnsharpMask);
        f.radius = 900; f.amount = 0; f.threshold = 400;
        const auto n = F::normalised(f);
        check("normalised: clamped to the ranges", n.radius == 250 && n.amount == 1 && n.threshold == 255, num(n.radius));
        check("fresh: Photoshop's defaults", F::fresh(FilterKind::UnsharpMask).amount == 100 && F::fresh(FilterKind::HighPass).radius == 10 &&
                                             F::fresh(FilterKind::GaussianBlur).radius == 2, "");
        check("kinds / keys / labels", F::kinds().size() == 14 && F::key(FilterKind::HighPass) == "high-pass" &&
                                       F::label(FilterKind::AddNoise) == "Add Noise", "");
    }

    std::cout << "\n-- filters: on the pixels (s014) --\n";
    {
        const auto src = square();
        const auto out = F::filtered(src, blur(4), nullptr);
        check("gaussian: done", out.has_value(), "");
        const auto e = out->get(100, 175);   // on the square's left edge
        const auto in = out->get(200, 175), far = out->get(500, 350), off = out->get(90, 175);
        check("gaussian: the edge goes half transparent", near(e.c[3], 0.5, 0.06), num(e.c[3]));
        check("gaussian: no black bleeds in (premultiplied)", near(e.c[0], 1, 1e-3) && near(off.c[0], 1, 1e-3) && off.c[3] > 0, num(e.c[0]));
        check("gaussian: the middle stays", near(in.c[0], 1, 1e-4) && near(in.c[3], 1, 1e-4), num(in.c[3]));
        check("gaussian: far away stays transparent", far.c[3] == 0.f && !out->tile(2, 1), "");
        const auto locked = F::filtered(src, blur(4), nullptr, true);
        check("gaussian, transparency locked: alpha as it was", locked->get(100, 175).c[3] == 1.f && locked->get(90, 175).c[3] == 0.f, "");
        const P::TileMap sel = core::select::rect_mask({P::Depth::F32, P::Mode::Gray, false}, 600, 400, {0, 0, 200, 400});
        const auto through = F::filtered(src, blur(4), &sel);
        check("gaussian through a selection: inside changes", near(through->get(100, 175).c[3], e.c[3], 1e-6), num(through->get(100, 175).c[3]));
        check("gaussian through a selection: outside doesn't", through->get(299, 175) == src.get(299, 175) &&
                                                              through->get(300, 175) == src.get(300, 175), "");
        P::Rect view{80, 80, 100, 100};
        const auto part = F::filtered(src, blur(4), nullptr, false, &view);
        check("gaussian: the preview's rectangle is the result there", same_in(*part, *out, view), "");
        check("gaussian: and nothing outside it", part->get(299, 175) == src.get(299, 175), "");
        bool cancelled = false;
        const auto stop = F::filtered(src, blur(4), nullptr, false, nullptr, [&] { cancelled = true; return true; });
        check("gaussian: cancelled -> nothing", !stop && cancelled, "");
        const auto u8 = F::filtered(square(P::Format{}), blur(4), nullptr);
        check("gaussian on 8-bit", u8 && near(u8->get(100, 175).c[3], e.c[3], 2.0 / 255) && near(u8->get(90, 175).c[0], 1, 2.0 / 255), "");
        P::Format cmyk; cmyk.mode = P::Mode::CMYK;
        check("CMYK: refused (colour management first)", !F::filtered(P::TileMap(cmyk, 64, 64), blur(2), nullptr), "");
        P::Format gray; gray.mode = P::Mode::Gray; gray.depth = P::Depth::F32;
        P::Px g; g.c[0] = 1; g.c[1] = 1;
        const auto gm = F::filtered(P::TileMap(gray, 300, 300).fill({100, 100, 100, 100}, g), blur(3), nullptr);
        check("Gray: blurs its alpha, keeps its grey", gm && near(gm->get(100, 150).c[1], 0.5 + std::erf(0.5 / 3 / std::sqrt(2.0)) / 2, 0.01) && near(gm->get(98, 150).c[0], 1, 1e-3), "");
        // Pixels beyond the image (s010) stay.
        P::TileMap::Outside o;
        o[{-1, 0}] = P::Tile::uniform(f32(), P::rgba(0, 1, 0, 1));
        const auto wide = src.with_outside(o);
        const auto wo = F::filtered(wide, blur(4), nullptr);
        check("outside tiles kept as they were", wo->outside(-1, 0) == wide.outside(-1, 0), "");
    }
    {
        const auto src = step();
        auto f = F::fresh(FilterKind::UnsharpMask);
        f.radius = 3; f.amount = 100;
        const auto out = F::filtered(src, f, nullptr);
        check("unsharp: the light side brightens at the edge", out->get(301, 50).c[0] > 0.76f, num(out->get(301, 50).c[0]));
        check("unsharp: the dark side darkens at the edge", out->get(298, 50).c[0] < 0.24f, num(out->get(298, 50).c[0]));
        check("unsharp: flat areas stay", near(out->get(100, 50).c[0], 0.25, 1e-5) && near(out->get(500, 50).c[0], 0.75, 1e-5), "");
        check("unsharp: alpha stays", out->get(301, 50).c[3] == 1.f, "");
        f.threshold = 255;
        const auto none = F::filtered(src, f, nullptr);
        check("unsharp: a high threshold leaves it", none->get(301, 50) == src.get(301, 50), "");
        const auto hp = F::filtered(src, F::fresh(FilterKind::HighPass), nullptr);
        check("high pass: flat areas go mid grey", near(hp->get(100, 50).c[0], 0.5, 1e-4) && near(hp->get(500, 50).c[0], 0.5, 1e-4), num(hp->get(100, 50).c[0]));
        check("high pass: the edge stands out", hp->get(301, 50).c[0] > 0.6f && hp->get(298, 50).c[0] < 0.4f, num(hp->get(301, 50).c[0]));
        check("high pass: transparent stays transparent", F::filtered(square(), F::fresh(FilterKind::HighPass), nullptr)->get(500, 350).c[3] == 0.f, "");
    }
    {
        const auto src = P::TileMap(f32(), 600, 400).fill({0, 0, 600, 300}, P::rgba(0.5f, 0.5f, 0.5f, 1));
        auto f = F::fresh(FilterKind::AddNoise);
        f.amount = 20; f.seed = 7;
        const auto a = F::filtered(src, f, nullptr), b = F::filtered(src, f, nullptr);
        bool same = true, mono_ok = true;
        double mean = 0, spread = 0;
        int n = 0;
        for (int y = 0; y < 300; y += 3)
            for (int x = 0; x < 600; x += 3) {
                const auto p = a->get(x, y);
                same = same && p == b->get(x, y);
                mean += p.c[0];
                spread += (p.c[0] - 0.5) * (p.c[0] - 0.5);
                ++n;
            }
        mean /= n;
        spread = std::sqrt(spread / n);
        check("noise: the same seed, the same noise", same, "");
        check("noise: the mean stays", near(mean, 0.5, 0.01), num(mean));
        check("noise: uniform 20% spreads +-0.1 (sd ~0.058)", near(spread, 0.1 / std::sqrt(3.0), 0.01), num(spread));
        check("noise: transparent pixels stay", a->get(10, 350).c[3] == 0.f && !a->tile(0, 1) == !src.tile(0, 1), "");
        f.seed = 8;
        check("noise: another seed, other noise", !(F::filtered(src, f, nullptr)->get(10, 10) == a->get(10, 10)), "");
        f.mono = true;
        const auto m = F::filtered(src, f, nullptr);
        for (int x = 0; x < 50; ++x) {
            const auto p = m->get(x, 20);
            mono_ok = mono_ok && p.c[0] == p.c[1] && p.c[1] == p.c[2];
        }
        check("noise: monochromatic is grey noise", mono_ok, "");
        f.mono = false; f.gaussian = true;
        const auto gs = F::filtered(src, f, nullptr);
        double gsd = 0;
        n = 0;
        for (int y = 0; y < 300; y += 3)
            for (int x = 0; x < 600; x += 3) { const double d = gs->get(x, y).c[1] - 0.5; gsd += d * d; ++n; }
        gsd = std::sqrt(gsd / n);
        check("noise: gaussian 20% has sd ~0.05", near(gsd, 0.05, 0.008), num(gsd));
        P::Rect view{250, 0, 100, 300};
        check("noise: the preview's rectangle is the result there", same_in(*F::filtered(src, f, nullptr, false, &view), *gs, view), "");
    }

    std::cout << "\n-- filters: Box Blur, Motion Blur, Median (s014 m3) --\n";
    {
        // One white pixel on opaque black.
        const auto dot = P::TileMap(f32(), 300, 300).fill({0, 0, 300, 300}, P::rgba(0, 0, 0, 1)).fill({150, 150, 1, 1}, P::rgba(1, 1, 1, 1));
        auto bx = F::fresh(FilterKind::BoxBlur);
        bx.radius = 2;
        const auto b = F::filtered(dot, bx, nullptr);
        double mass = 0;
        for (int y = 140; y < 161; ++y)
            for (int x = 140; x < 161; ++x) mass += b->get(x, y).c[0];
        check("box blur: a 5 x 5 square of 1/25", near(b->get(152, 148).c[0], 1.0 / 25, 1e-5) && b->get(153, 150).c[0] == 0.f, num(b->get(152, 148).c[0]));
        check("box blur: keeps the mass", near(mass, 1, 1e-4), num(mass));
        check("box blur: reach is the radius", F::reach(bx) == 2, "");

        auto mo = F::fresh(FilterKind::MotionBlur);
        mo.radius = 10;
        mo.angle = 0;
        const auto h = F::filtered(dot, mo, nullptr);
        check("motion 0 deg: a horizontal streak", near(h->get(155, 150).c[0], 1.0 / 11, 1e-4) && near(h->get(145, 150).c[0], 1.0 / 11, 1e-4) &&
                                                   h->get(157, 150).c[0] == 0.f, num(h->get(155, 150).c[0]));
        check("motion 0 deg: nothing above or below", h->get(150, 149).c[0] == 0.f && h->get(150, 151).c[0] == 0.f, "");
        mo.angle = 90;
        const auto v = F::filtered(dot, mo, nullptr);
        check("motion 90 deg: a vertical streak", near(v->get(150, 145).c[0], 1.0 / 11, 1e-4) && v->get(145, 150).c[0] == 0.f, num(v->get(150, 145).c[0]));
        mo.angle = 45;
        const auto dg = F::filtered(dot, mo, nullptr);
        check("motion 45 deg: up and to the right", dg->get(153, 147).c[0] > 0.02f && dg->get(147, 147).c[0] == 0.f, num(dg->get(153, 147).c[0]));
        check("motion: normalised clamps the angle", F::normalised([] { auto f = F::fresh(FilterKind::MotionBlur); f.angle = 400; return f; }()).angle == 90, "");

        auto md = F::fresh(FilterKind::Median);
        const auto m = F::filtered(dot, md, nullptr);
        check("median: a speck goes", m->get(150, 150).c[0] == 0.f, num(m->get(150, 150).c[0]));
        const auto st = step(P::Format{});
        md.radius = 3;
        const auto se = F::filtered(st, md, nullptr);
        check("median: a straight edge stays where it was", se->get(299, 50) == st.get(299, 50) && se->get(300, 50) == st.get(300, 50) &&
                                                           se->get(0, 0) == st.get(0, 0), "");
        // Salt on grey, 8-bit: back to the grey exactly.
        auto salty = P::TileMap(P::Format{}, 300, 300).fill({0, 0, 300, 300}, P::rgba(0.4f, 0.4f, 0.4f, 1));
        for (int i = 0; i < 40; ++i) salty = salty.fill({(i * 37) % 290 + 5, (i * 53) % 290 + 5, 1, 1}, P::rgba(1, 1, 1, 1));
        md.radius = 1;
        const auto cl = F::filtered(salty, md, nullptr);
        bool clean = true;
        for (int i = 0; i < 40; ++i) clean = clean && cl->get((i * 37) % 290 + 5, (i * 53) % 290 + 5) == salty.get(0, 0);
        check("median 8-bit: salt goes, the grey exactly", clean, "");
        const P::Rect view{120, 120, 70, 60};
        mo.angle = 30;
        check("motion: the preview's rectangle is the result there",
              same_in(*F::filtered(dot, mo, nullptr, false, &view), *F::filtered(dot, mo, nullptr), view), "");
        md.radius = 4;
        check("median: the preview's rectangle is the result there",
              same_in(*F::filtered(salty, md, nullptr, false, &view), *F::filtered(salty, md, nullptr), view), "");
        // Transparent neighbours: a red square's motion streak stays red.
        mo.angle = 0;
        mo.radius = 20;
        const auto sq = F::filtered(square(), mo, nullptr);
        check("motion: transparent neighbours add no black", near(sq->get(95, 175).c[0], 1, 1e-3) && sq->get(95, 175).c[3] > 0.1f, num(sq->get(95, 175).c[3]));
    }

    std::cout << "\n-- filters: the verb (s014) --\n";
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        const auto before = d.get("L4")->pixels;
        const auto steps = j.size();
        check("apply_filter: done, one step", L::apply_filter(j, d, "L4", blur(3)) && j.size() == steps + 1, "");
        check("apply_filter: the step's label", j.undo_label().find("Gaussian Blur") != std::string::npos, j.undo_label());
        check("apply_filter: the pixels changed", d.get("L4")->pixels != before, "");
        j.undo();
        check("apply_filter: undo gives them back", d.get("L4")->pixels == before, "");
        check("apply_filter: refused on a group", !L::apply_filter(j, d, "L2", blur(3)), "");
        check("apply_filter: refused on None", !L::apply_filter(j, d, "L4", core::Filter{}), "");
        auto l = *d.get("L4");
        l.locks = core::lock::Pixels;
        { core::Document::Loading load(d); d.put("L4", l); }
        check("apply_filter: refused under a Pixels lock", !L::apply_filter(j, d, "L4", blur(3)), "");
        l.locks = core::lock::Alpha;
        l.pixels = std::make_shared<const P::TileMap>(l.pixels->fill({100, 100, 100, 150}, P::rgba(0, 0, 1, 1)));   // half blue: colour to blur
        { core::Document::Loading load(d); d.put("L4", l); }
        const auto a0 = d.get("L4")->pixels;
        L::apply_filter(j, d, "L4", blur(6));
        bool kept = true;
        for (int x = 80; x < 120; ++x) kept = kept && d.get("L4")->pixels->get(x, 150).c[3] == a0->get(x, 150).c[3];
        check("apply_filter: a transparency lock keeps the blur off alpha", kept && d.get("L4")->pixels != a0, "");
    }

    std::cout << "\n-- prefs: the app's settings (s014 m4) --\n";
    {
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / ("klimt_prefs_" + std::to_string(::getpid()));
        const auto file = (dir / "sub" / "prefs.json").string();
        fs::remove_all(dir);
        check("prefs: first run -> the defaults", core::load_prefs(file) == core::Prefs{}, "");
        core::Prefs p;
        p.dock_split = 512;
        check("prefs: saved (making the folder)", core::save_prefs(file, p), "");
        check("prefs: round trip", core::load_prefs(file) == p, "");
        { std::ofstream f(file); f << "{\"dock_split\": 9, \"future\": [1, 2]}"; }
        check("prefs: clamped on load", core::load_prefs(file).dock_split == 120, "");
        core::save_prefs(file, p);
        std::ifstream in(file);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        check("prefs: keys it doesn't know survive a save", text.find("future") != std::string::npos && core::load_prefs(file) == p, "");
        { std::ofstream f(file); f << "not json {"; }
        check("prefs: garbage -> the defaults", core::load_prefs(file) == core::Prefs{}, "");
        fs::remove_all(dir);
    }

    std::cout << "\n-- live filters (s018) --\n";
    {
        core::live::clear_cache();
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);   // white Background; L4 a red square (100, 100) 200 x 150
        const auto seen = [&](int x, int y) { return core::sample_composite(d.image(), d.snapshot(), x, y, 0); };
        const auto px0 = d.get("L4")->pixels;
        check("live: the square's edge is crisp first", near(seen(98, 175).g, 1, 0.01), num(seen(98, 175).g));
        check("live: add a blur, ONE step", L::add_live(j, d, "L4", blur(4)) && j.size() == 1 &&
                                            j.undo_label().find("(live)") != std::string::npos, j.undo_label());
        check("live: the pixels never change", d.get("L4")->pixels == px0, "");
        check("live: the canvas shows it blurred (red spreads over the white)", seen(98, 175).g < 0.8, num(seen(98, 175).g));
        check("live: the look is cached (the same map twice)", core::live::look(*d.get("L4")) == core::live::look(*d.get("L4")), "");
        check("live: changed_tiles redraws everything on a change", [&] {
            auto before = d.snapshot();
            L::toggle_live(j, d, "L4", 0);
            const auto t = core::changed_tiles(d.image(), before, d.image(), d.snapshot());
            return t.all;
        }(), "");
        check("live: hidden, crisp again", near(seen(98, 175).g, 1, 0.01), num(seen(98, 175).g));
        L::toggle_live(j, d, "L4", 0);
        auto bigger = blur(10);
        check("live: edit the radius", L::set_live(j, d, "L4", 0, bigger) && d.get("L4")->filters[0].f.radius == 10, "");
        check("live: further out now", seen(85, 175).g < 0.95, num(seen(85, 175).g));
        check("live: a kind can't change by editing", !L::set_live(j, d, "L4", 0, F::fresh(FilterKind::HighPass)), "");
        auto noisy = F::fresh(FilterKind::AddNoise);
        noisy.amount = 60;
        L::add_live(j, d, "L4", noisy);
        const auto a = seen(200, 175);
        check("live: two in the stack", d.get("L4")->filters.size() == 2, "");
        check("live: reorder", L::move_live(j, d, "L4", 1, 0) && d.get("L4")->filters[0].f.kind == FilterKind::AddNoise, "");
        const auto b = seen(200, 175);
        check("live: the order matters", !(near(a.r, b.r, 1e-6) && near(a.g, b.g, 1e-6)), num(a.r) + "," + num(a.g) + " / " + num(b.r) + "," + num(b.g));
        check("live: delete one", L::remove_live(j, d, "L4", 0) && d.get("L4")->filters.size() == 1, "");
        // The preview's rectangle == the full run there.
        std::vector<core::LiveFilter> stack{{blur(3), true}, {F::fresh(FilterKind::UnsharpMask), true}};
        const P::Rect view{80, 80, 60, 60};
        const auto full = core::live::run(square(), stack), part = core::live::run(square(), stack, &view);
        check("live: a rectangle of the stack is exact there", full && part && same_in(*full, *part, view), "");
        // File.
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_live_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image();
        c.layers = d.snapshot();
        c.next_id = d.next_id();
        std::string err;
        check("live: saved", core::file::save_klimt(path, c, err), err);
        const auto back = core::file::open(path, err);
        check("live: opened with its stack", back && back->layers.at("L4").filters == d.get("L4")->filters, err);
        fs::remove(path);
        // Rasterize: the look becomes the pixels.
        const auto look = core::live::look(*d.get("L4"));
        check("live: rasterize", L::bake_live(j, d, "L4") && d.get("L4")->filters.empty() && d.get("L4")->pixels == look, "");
        j.undo();
        check("live: undo brings the stack back", d.get("L4")->filters.size() == 1 && d.get("L4")->pixels == px0, "");
        check("live: refused on a group", !L::add_live(j, d, "L2", blur(2)), "");
    }

    std::cout << "\n-- live filters: the filter mask (s018 m3) --\n";
    {
        core::live::clear_cache();
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        const auto seen = [&](int x, int y) { return core::sample_composite(d.image(), d.snapshot(), x, y, 0); };
        check("filter mask: refused without live filters", !L::set_filter_mask(j, d, "L4", std::make_shared<const P::TileMap>(d.blank_mask())), "");
        L::add_live(j, d, "L4", blur(6));
        const double right_before = seen(299 + 3, 175).g;   // outside the square's right edge: pink
        // Hide the filters on the left half (x < 200): black there.
        auto hide = std::make_shared<const P::TileMap>(core::select::rect_mask(d.mask_format(), 600, 400, {0, 0, 200, 400}));
        check("filter mask: set, ONE step", L::set_filter_mask(j, d, "L4", hide) && j.undo_label().find("Filter Mask") != std::string::npos, j.undo_label());
        check("filter mask: hidden where it says -- crisp edge on the left", near(seen(98, 175).g, 1, 0.01), num(seen(98, 175).g));
        check("filter mask: the filters still show on the right", near(seen(302, 175).g, right_before, 0.01) && right_before < 0.9, num(seen(302, 175).g));
        check("filter mask: the pixels never change", d.get("L4")->filter_mask == hide && d.get("L4")->pixels, "");
        // The canvas verbs carry it.
        core::canvas::canvas_size(j, d, 800, 400, 0, 1, P::rgba(1, 1, 1, 1));
        check("filter mask: Canvas Size carries it", d.get("L4")->filter_mask && d.get("L4")->filter_mask->width() == 800, "");
        j.undo();
        // File.
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_fmask_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image(); c.layers = d.snapshot(); c.next_id = d.next_id();
        std::string err;
        core::file::save_klimt(path, c, err);
        const auto back = core::file::open(path, err);
        check("filter mask: in the file", back && back->layers.at("L4").filter_mask &&
                                          core::select::coverage(*back->layers.at("L4").filter_mask, 50, 50) == 1.f, err);
        fs::remove(path);
        check("filter mask: goes with the last filter", L::remove_live(j, d, "L4", 0) && !d.get("L4")->filter_mask, "");
    }
}
