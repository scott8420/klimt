// klimt_selftest -- ADJUSTMENTS (s013): the maths, adjustment layers in the
// compositor, their verbs and the file; Image > Adjustments on pixels.
#include "core/Adjust.hpp"
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Selection.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace A = klimt::core::adjust;
namespace fs = std::filesystem;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
bool near(double a, double b, double eps = 0.01) { return std::abs(a - b) <= eps; }
std::string num(double v) { return std::to_string(v); }

struct Fix {
    core::Document d;
    core::DocJournal j{d};
    Fix() {
        d.set_door([this] { return j.open(); });
        L::load_fixture(d);   // white Background; "A" (L4, in Group L2) a red square (100, 100) 200 x 150
    }
    core::Sampled seen(int x, int y) const { return core::sample_composite(d.image(), d.snapshot(), x, y, 0); }
    std::string adj(core::AdjustKind k, const std::string& above = {}) { return L::add_adjustment(j, d, k, above); }
    void set(const std::string& id, const core::Adjust& a) { L::set_adjust(j, d, id, a); }
};
core::Adjust hue120() { auto a = A::fresh(core::AdjustKind::HueSat); a.hue = 120; return a; }
}  // namespace

void selftest_adjust(const Check& check) {
    using core::AdjustKind;
    std::cout << "\n-- adjustments: the maths (s013) --\n";
    {
        core::LevelsCh c;
        check("levels: untouched is the identity", near(A::levels_map(c, 0.3), 0.3, 1e-9), "");
        c.in_black = 0.2; c.in_white = 0.8;
        check("levels: input black / white stretch", near(A::levels_map(c, 0.2), 0) && near(A::levels_map(c, 0.8), 1) &&
                                                     near(A::levels_map(c, 0.5), 0.5), num(A::levels_map(c, 0.5)));
        c = {};
        c.gamma = 2;
        check("levels: gamma 2 lifts the middle", near(A::levels_map(c, 0.5), std::sqrt(0.5)), num(A::levels_map(c, 0.5)));
        c = {};
        c.out_black = 0.1; c.out_white = 0.9;
        check("levels: output range", near(A::levels_map(c, 0), 0.1) && near(A::levels_map(c, 1), 0.9), "");
    }
    {
        const auto t = A::curve_table({{0, 0}, {1, 1}}, 101);
        check("curves: the diagonal is the identity", near(t[37], 0.37, 1e-9), num(t[37]));
        const auto s = A::curve_table({{0, 0}, {0.5, 0.7}, {1, 1}}, 101);
        check("curves: the curve passes through its points", near(s[50], 0.7, 1e-6) && near(s[0], 0) && near(s[100], 1), num(s[50]));
        check("curves: smooth between them", s[25] > 0.25 && s[25] < 0.7 && s[75] > 0.7 && s[75] < 1, num(s[25]));
        const auto f = A::curve_table({{0.2, 0.1}, {0.8, 0.9}}, 101);
        check("curves: flat past the end points", near(f[0], 0.1) && near(f[10], 0.1) && near(f[100], 0.9) && near(f[90], 0.9), "");
        const auto c = A::curve_table({{0, 0}, {0.1, 1}, {0.2, 0}, {1, 1}}, 101);
        bool in = true;
        for (const double v : c) in = in && v >= 0 && v <= 1;
        check("curves: an overshoot is clamped to 0..1", in, "");
    }
    {
        float r = 1, g = 0, b = 0;
        A::apply_rgb(hue120(), r, g, b);
        check("hue/saturation: +120 turns red green", near(r, 0) && near(g, 1) && near(b, 0), num(r) + "," + num(g) + "," + num(b));
        auto a = A::fresh(AdjustKind::HueSat);
        a.saturation = -100;
        r = 0.8f; g = 0.2f; b = 0.4f;
        A::apply_rgb(a, r, g, b);
        check("hue/saturation: -100 saturation is grey", near(r, g) && near(g, b), "");
        a = A::fresh(AdjustKind::HueSat);
        a.lightness = 100;
        r = 0.3f; g = 0.5f; b = 0.1f;
        A::apply_rgb(a, r, g, b);
        check("hue/saturation: +100 lightness is white", near(r, 1) && near(g, 1) && near(b, 1), "");
        a = A::fresh(AdjustKind::HueSat);
        a.colorize = true; a.hue = 240; a.saturation = 100;
        r = g = b = 0.5f;
        A::apply_rgb(a, r, g, b);
        check("hue/saturation: Colorize tints a grey", b > r && b > g, num(r) + "," + num(g) + "," + num(b));
    }
    {
        auto a = A::fresh(AdjustKind::BrightCon);
        a.brightness = 60; a.contrast = 40;
        float r = 0, g = 0.5f, b = 1;
        A::apply_rgb(a, r, g, b);
        check("brightness/contrast: black and white stay put", near(r, 0) && near(b, 1), "");
        a.contrast = 0;
        r = 0, g = 0.5f, b = 1;
        A::apply_rgb(a, r, g, b);
        check("brightness/contrast: brighter lifts the middle", g > 0.6f, num(g));
        a.brightness = 0; a.contrast = -50;
        r = 0, g = 0.5f, b = 1;
        A::apply_rgb(a, r, g, b);
        check("brightness/contrast: less contrast pulls toward grey", r > 0.2f && b < 0.8f && near(g, 0.5), "");
    }
    {
        auto a = A::fresh(AdjustKind::Levels);
        check("adjust: a fresh one changes nothing", A::identity(a) && A::identity(A::fresh(AdjustKind::Curves)), "");
        a.levels[0].gamma = 50;
        check("adjust: out of range is refused", !A::refusal(a).empty(), "");
        check("adjust: ...and normalised into range", A::normalised(a).levels[0].gamma <= 9.99, "");
        auto c = A::fresh(AdjustKind::Curves);
        c.curves[0] = {{1, 1}, {0.5, 0.6}, {0, 0}};
        const auto n = A::normalised(c);
        check("adjust: curve points sorted", n.curves[0].size() == 3 && n.curves[0][1].x == 0.5 && n.curves[0][0].x == 0, "");
        check("adjust: keys round trip", A::from_key(A::key(AdjustKind::HueSat)) == AdjustKind::HueSat &&
                                         !A::from_key("colour-lookup"), "");
    }

    std::cout << "\n-- adjustments: adjustment layers (s013) --\n";
    {
        Fix t;
        const auto id = t.adj(AdjustKind::HueSat);
        check("adjustment layer: made, one step", !id.empty() && t.j.size() == 1, std::to_string(t.j.size()));
        const auto l = t.d.get(id);
        check("adjustment layer: named \"Hue/Saturation 1\", at the top, with a mask",
              l && l->name == "Hue/Saturation 1" && l->kind == core::Kind::Adjustment && l->mask && l->mask->tile_count() == 0 &&
                  t.d.panel("").front() == id, l ? l->name : "");
        check("adjustment layer: a fresh one shows nothing changed", near(t.seen(150, 150).r, 1) && near(t.seen(150, 150).g, 0), "");
        t.set(id, hue120());
        check("adjustment layer: Hue +120 -- the red square shows green",
              near(t.seen(150, 150).r, 0) && near(t.seen(150, 150).g, 1), num(t.seen(150, 150).g));
        check("adjustment layer: white stays white", near(t.seen(20, 20).r, 1) && near(t.seen(20, 20).g, 1), "");
        check("adjustment layer: the pixels below are untouched", t.d.get("L4")->pixels->get(150, 150).c[0] > 0.99f, "");
        check("adjustment layer: the document holds", t.d.check().empty(), t.d.check());
        auto op = *t.d.get(id);
        L::set_opacity(t.j, t.d, id, 50);
        check("adjustment layer: at 50% -- half way", near(t.seen(150, 150).r, 0.5) && near(t.seen(150, 150).g, 0.5),
              num(t.seen(150, 150).r));
        L::set_visible(t.j, t.d, id, false);
        check("adjustment layer: hidden -- no effect", near(t.seen(150, 150).r, 1), "");
        L::set_visible(t.j, t.d, id, true);
        L::set_opacity(t.j, t.d, id, 100);
        const auto second = t.adj(AdjustKind::HueSat);
        check("adjustment layer: the next is \"Hue/Saturation 2\"", t.d.get(second)->name == "Hue/Saturation 2", "");
        t.set(second, hue120());
        check("adjustment layer: two stack (red -> green -> blue)", near(t.seen(150, 150).b, 1) && near(t.seen(150, 150).g, 0), "");
        t.j.undo();
        t.j.undo();
        check("adjustment layer: undo", t.d.get(second) == std::nullopt && near(t.seen(150, 150).g, 1), "");
        check("adjustment layer: refused on a pixel layer", !L::set_adjust(t.j, t.d, "L4", hue120()), "");
        auto other = A::fresh(AdjustKind::Levels);
        check("adjustment layer: refused for another kind", !L::set_adjust(t.j, t.d, id, other), "");
        (void)op;
    }
    {
        Fix t;
        const auto id = t.adj(AdjustKind::HueSat);
        t.set(id, hue120());
        const std::size_t steps = t.j.size();
        auto a = hue120();
        a.saturation = -10;
        t.set(id, a);
        a.saturation = -20;
        t.set(id, a);
        check("adjustment layer: presses fold into one step", t.j.size() == steps, std::to_string(t.j.size()));
        L::adjust_begin(t.j, t.d, id);
        for (int h = 10; h <= 90; h += 10) { a.hue = h; L::adjust_to(t.j, t.d, id, a); }
        L::adjust_end(t.j);
        check("adjustment layer: a drag is ONE step", t.j.size() == steps + 1 && t.d.get(id)->adjust.hue == 90,
              std::to_string(t.j.size()));
    }
    {
        Fix t;
        L::load_selection(t.d, {100, 100, 50, 150});
        const auto id = t.adj(AdjustKind::HueSat);
        t.set(id, hue120());
        check("adjustment layer: made with a selection -- only there", near(t.seen(120, 150).g, 1) && near(t.seen(200, 150).r, 1),
              "");
        L::set_mask_on(t.j, t.d, id, false);
        check("adjustment layer: mask off -- everywhere", near(t.seen(200, 150).g, 1), "");
    }
    {
        Fix t;   // inside the group (Pass Through): what's below in the document too
        const auto id = t.adj(AdjustKind::Levels, "L4");
        auto a = A::fresh(AdjustKind::Levels);
        a.levels[0].out_white = 0.5;
        t.set(id, a);
        check("adjustment layer: in a Pass Through group -- reaches the background", near(t.seen(20, 20).r, 0.5, 0.02),
              num(t.seen(20, 20).r));
        L::set_blend(t.j, t.d, "L2", core::Blend::Normal);
        check("adjustment layer: in a Normal group -- only the group's contents", near(t.seen(20, 20).r, 1) &&
                                                                               near(t.seen(150, 150).r, 0.5, 0.02),
              num(t.seen(20, 20).r));
    }
    {
        Fix t;   // clipped to the red square: the background stays
        const auto id = t.adj(AdjustKind::Levels, "L4");
        auto a = A::fresh(AdjustKind::Levels);
        a.levels[0].out_white = 0.5;
        t.set(id, a);
        L::set_clipped(t.j, t.d, id, true);
        check("adjustment layer: clipped -- only where its base has pixels", near(t.seen(20, 20).r, 1) &&
                                                                          near(t.seen(150, 150).r, 0.5, 0.02), "");
    }
    {
        Fix t;
        const auto id = t.adj(AdjustKind::HueSat);
        const auto before = t.d.snapshot();
        t.set(id, hue120());
        const auto ts = core::changed_tiles(t.d.image(), before, t.d.image(), t.d.snapshot());
        check("adjustment layer: a change redraws every tile", ts.count() == static_cast<std::size_t>(ts.cols * ts.rows), "");
    }
    {
        Fix t;
        core::Layer bad = *t.d.get("L4");
        bad.adjust = hue120();
        {
            core::Document::Loading load(t.d);
            t.d.put("L4", bad);
        }
        check("adjustment layer: check() -- no adjustment on a pixel layer", !t.d.check().empty(), "");
    }

    std::cout << "\n-- adjustments: the file (s013) --\n";
    {
        Fix t;
        const auto a1 = t.adj(AdjustKind::Levels);
        auto lv = A::fresh(AdjustKind::Levels);
        lv.levels[0] = {0.1, 0.9, 1.5, 0.05, 0.95};
        lv.levels[2].gamma = 0.7;
        t.set(a1, lv);
        const auto a2 = t.adj(AdjustKind::Curves);
        auto cv = A::fresh(AdjustKind::Curves);
        cv.curves[0] = {{0, 0}, {0.25, 0.4}, {1, 1}};
        cv.curves[3] = {{0, 0.1}, {1, 0.9}};
        t.set(a2, cv);
        const auto a3 = t.adj(AdjustKind::HueSat);
        auto hs = A::fresh(AdjustKind::HueSat);
        hs.colorize = true; hs.hue = 200; hs.saturation = 40; hs.lightness = -10;
        t.set(a3, hs);
        const auto a4 = t.adj(AdjustKind::BrightCon);
        auto bc = A::fresh(AdjustKind::BrightCon);
        bc.brightness = -30; bc.contrast = 70;
        t.set(a4, bc);
        L::set_mask_on(t.j, t.d, a4, false);
        core::file::Contents c;
        c.image = t.d.image();
        c.layers = t.d.snapshot();
        c.next_id = t.d.next_id();
        const auto dir = fs::temp_directory_path() / "klimt_selftest_adjust";
        fs::create_directories(dir);
        const auto path = (dir / "adjusted.klimt").string();
        std::string err;
        check("file: saves adjustment layers", core::file::save_klimt(path, c, err), err);
        const auto back = core::file::open(path, err);
        check("file: opens them", back.has_value(), err);
        if (back) {
            bool same = true;
            for (const auto& id : {a1, a2, a3, a4}) {
                const auto& x = back->layers.at(id);
                const auto& y = c.layers.at(id);
                same = same && x.kind == core::Kind::Adjustment && x.adjust == y.adjust && x.name == y.name &&
                       (x.mask != nullptr) == (y.mask != nullptr) && x.mask_on == y.mask_on;
            }
            check("file: every setting comes back", same, "");
            const auto s1 = core::sample_composite(c.image, c.layers, 150, 150, 0);
            const auto s2 = core::sample_composite(back->image, back->layers, 150, 150, 0);
            check("file: ...and looks the same", near(s1.r, s2.r, 1e-4) && near(s1.g, s2.g, 1e-4) && near(s1.b, s2.b, 1e-4), "");
        }
        fs::remove_all(dir);
    }

    std::cout << "\n-- adjustments: Image > Adjustments on pixels (s013) --\n";
    {
        Fix t;
        check("apply: Hue +120 on the red layer", L::apply_adjust(t.j, t.d, "L4", hue120()) && t.j.size() == 1, "");
        const auto p = t.d.get("L4")->pixels->get(150, 150);
        check("apply: ...the pixels are green now", near(p.c[0], 0) && near(p.c[1], 1), "");
        check("apply: transparent stays transparent", t.d.get("L4")->pixels->get(20, 20).c[3] <= 0.f, "");
        check("apply: nothing to change -- refused", !L::apply_adjust(t.j, t.d, "L4", A::fresh(AdjustKind::Levels)), "");
        check("apply: not on a group", !L::apply_adjust(t.j, t.d, "L2", hue120()), "");
    }
    {
        Fix t;
        L::load_selection(t.d, {100, 100, 50, 150});
        L::apply_adjust(t.j, t.d, "L4", hue120());
        const auto& px = *t.d.get("L4")->pixels;
        check("apply: through the selection only", near(px.get(120, 150).c[1], 1) && near(px.get(200, 150).c[0], 1), "");
    }
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        {
            core::Document::Loading load(d);
            d.set_image({.width = 64, .height = 64, .format = P::Format{P::Depth::U8, P::Mode::Gray, false}});
            core::Layer l;
            l.name = "Gray";
            l.pos = 1;
            l.pixels = std::make_shared<const P::TileMap>(d.blank().fill({0, 0, 64, 64}, [] { P::Px p; p.c[0] = 0.5f; return p; }()));
            d.put("L1", l);
        }
        auto a = A::fresh(AdjustKind::Levels);
        a.levels[0].in_white = 0.5;
        check("apply: a Gray document -- the composite channel", L::apply_adjust(j, d, "L1", a) &&
                                                                 near(d.get("L1")->pixels->get(10, 10).c[0], 1), "");
    }
    {
        std::vector<std::uint32_t> px{0xffff0000u, 0xffff0000u, 0xff000000u, 0x00000000u};
        const auto h = A::histogram(px);
        check("histogram: counts, transparent left out", h[1][255] == 2 && h[1][0] == 1 && h[2][0] == 3, "");
    }
    // s056 (WEAKNESSES 32): what an adjustment layer's histogram depends on.
    {
        namespace Px = klimt::pixel;
        const Px::Format f{Px::Depth::U8, Px::Mode::RGB, true};
        const auto px = [&](float v) { return std::make_shared<const Px::TileMap>(Px::TileMap(f, 300, 200).fill({0, 0, 300, 200}, Px::rgba(v, v, v))); };
        core::Layers ls;
        core::Layer bg; bg.name = "Bg"; bg.pos = 1; bg.pixels = px(0.2f); ls["L1"] = bg;
        core::Layer lv; lv.name = "Levels"; lv.kind = core::Kind::Adjustment; lv.pos = 2; lv.adjust.kind = core::AdjustKind::Levels; ls["L2"] = lv;
        core::Layer top; top.name = "Top"; top.pos = 3; top.pixels = px(0.9f); ls["L3"] = top;
        const auto key = core::histogram_key(ls, "L2");
        auto a = ls; a["L2"].adjust.kind = core::AdjustKind::Curves; a["L2"].opacity = 40;
        auto b = ls; b["L3"].pixels = px(0.5f);
        auto c = ls; c["L1"].pixels = px(0.6f);
        auto d = ls; d["L1"].visible = false;
        auto e = ls; e["L3"].pos = 1.5;   // Top moved under the Levels
        check("histogram: s056 -- the layer's own settings change nothing it counts", core::histogram_key(a, "L2") == key, "");
        check("histogram: s056 -- painting a layer ABOVE it changes nothing it counts", core::histogram_key(b, "L2") == key, "");
        check("histogram: s056 -- painting the layer below: counted again", !(core::histogram_key(c, "L2") == key), "");
        check("histogram: s056 -- hiding the layer below: counted again", !(core::histogram_key(d, "L2") == key), "");
        check("histogram: s056 -- a layer moved under it: counted again", !(core::histogram_key(e, "L2") == key), "");
    }

    std::cout << "\n-- adjustments: the preview IS the result (s060, WEAKNESSES 33) --\n";
    {
        namespace Px = klimt::pixel;
        const Px::Format f{Px::Depth::U8, Px::Mode::RGB, true};
        core::Image im;
        im.width = 300; im.height = 200; im.format = f;
        const auto fill = [&](Px::Px c) { return std::make_shared<const Px::TileMap>(Px::TileMap(f, 300, 200).fill({0, 0, 300, 200}, c)); };
        // White background; over it "Top", red at HALF alpha (Normal).
        core::Layers ls;
        core::Layer bg; bg.name = "Bg"; bg.pos = 1; bg.pixels = fill(Px::rgba(1, 1, 1)); ls["L1"] = bg;
        core::Layer top; top.name = "Top"; top.pos = 2; top.pixels = fill(Px::rgba(1, 0, 0, 0.5f)); ls["L2"] = top;
        const auto a = hue120();
        const auto res = [&](const core::Layers& from, const core::Adjust& adj) {   // what OK leaves
            auto r = from;
            r["L2"].pixels = std::make_shared<const Px::TileMap>(*A::applied(*from.at("L2").pixels, adj, nullptr));
            return core::sample_composite(im, r, 150, 100, 0);
        };
        const auto same = [](core::Sampled x, core::Sampled y) { return near(x.r, y.r) && near(x.g, y.g) && near(x.b, y.b) && near(x.a, y.a); };
        const auto txt = [](core::Sampled x) { return num(x.r) + "," + num(x.g) + "," + num(x.b); };
        const auto want = res(ls, a);
        auto standin = ls;
        core::preview_adjust(standin, "L2", a, nullptr, nullptr);
        const auto sv = core::sample_composite(im, standin, 150, 100, 0);
        check("adjust preview: the stand-in is a clipped adjustment just above the layer",
              standin.count(core::kAdjustPreview) && standin.at(core::kAdjustPreview).clipped &&
              standin.at(core::kAdjustPreview).pos > 2 && standin.at(core::kAdjustPreview).kind == core::Kind::Adjustment, "");
        check("adjust preview: half-transparent -- the stand-in is off (why it's replaced)", !same(sv, want),
              txt(sv) + " vs " + txt(want));
        const auto exact = std::make_shared<const Px::TileMap>(*A::applied(*ls.at("L2").pixels, a, nullptr));
        auto ex = ls;
        core::preview_adjust(ex, "L2", a, exact, nullptr);
        const auto ev = core::sample_composite(im, ex, 150, 100, 0);
        check("adjust preview: half-transparent -- the exact one IS the result", same(ev, want), txt(ev) + " vs " + txt(want));
        check("adjust preview: the exact one adds no layer", ex.size() == ls.size() && !ex.count(core::kAdjustPreview), "");

        // A Screen layer, opaque: the stand-in adjusts the screened colour, the
        // result screens the adjusted layer.
        auto sc = ls;
        sc["L1"].pixels = fill(Px::rgba(0.6f, 0.6f, 0.6f));
        sc["L2"].pixels = fill(Px::rgba(1, 1, 1));
        sc["L2"].blend = core::Blend::Screen;
        auto lv = A::fresh(core::AdjustKind::Levels);
        lv.levels[0].out_white = 0.5;
        const auto want2 = res(sc, lv);
        auto s2 = sc;
        core::preview_adjust(s2, "L2", lv, nullptr, nullptr);
        auto e2 = sc;
        core::preview_adjust(e2, "L2", lv, std::make_shared<const Px::TileMap>(*A::applied(*sc.at("L2").pixels, lv, nullptr)), nullptr);
        check("adjust preview: a Screen layer -- the stand-in is off",
              !same(core::sample_composite(im, s2, 150, 100, 0), want2), txt(core::sample_composite(im, s2, 150, 100, 0)));
        check("adjust preview: a Screen layer -- the exact one IS the result (0.8)",
              same(core::sample_composite(im, e2, 150, 100, 0), want2) && near(want2.r, 0.8), txt(want2));

        // The layer's mask still hides: the left half hidden shows the background.
        auto mk = ls;
        P::TileMap m(P::Format{P::Depth::U8, P::Mode::Gray, false}, 300, 200);
        mk["L2"].mask = std::make_shared<const P::TileMap>(m.fill({0, 0, 150, 200}, P::Px{{1, 0, 0, 0}}));
        auto e3 = mk;
        core::preview_adjust(e3, "L2", a, exact, nullptr);
        const auto hidden = core::sample_composite(im, e3, 20, 100, 0);
        check("adjust preview: the exact one keeps the layer's mask", near(hidden.r, 1) && near(hidden.g, 1) && near(hidden.b, 1), txt(hidden));
        check("adjust preview: ...and shows where the mask reveals", same(core::sample_composite(im, e3, 250, 100, 0), want), "");

        auto id = ls;
        core::preview_adjust(id, "L2", A::fresh(core::AdjustKind::Curves), exact, nullptr);
        check("adjust preview: settings that change nothing -- nothing shown", id.at("L2").pixels == ls.at("L2").pixels && id.size() == ls.size(), "");
    }

    std::cout << "\n-- adjustments: Invert, Posterize, Threshold, Exposure (s061, WEAKNESSES 34) --\n";
    {
        using K = core::AdjustKind;
        const auto rgb = [](const core::Adjust& a, float r, float g, float b) { A::apply_rgb(a, r, g, b); return std::array<float, 3>{r, g, b}; };
        check("s061: the kinds, keys round trip", A::kinds().size() >= 8 && A::from_key("invert") == K::Invert &&
              A::from_key("posterize") == K::Posterize && A::from_key("threshold") == K::Threshold && A::from_key("exposure") == K::Exposure, "");
        const auto iv = rgb(A::fresh(K::Invert), 0.2f, 0.5f, 1.f);
        check("invert: each channel turned over", near(iv[0], 0.8) && near(iv[1], 0.5) && near(iv[2], 0), num(iv[0]));
        check("invert: never the identity; settingless", !A::identity(A::fresh(K::Invert)) && A::settingless(K::Invert) && !A::settingless(K::Posterize), "");
        auto po = A::fresh(K::Posterize);
        check("posterize: starts at 4 levels, Photoshop's", po.posterize == 4 && !A::identity(po), "");
        const auto p1 = rgb(po, 0.f, 0.3f, 1.f), p2 = rgb(po, 0.6f, 0.74f, 0.99f);
        check("posterize 4: black / white kept, steps of a third", near(p1[0], 0) && near(p1[1], 1.0 / 3) && near(p1[2], 1) &&
              near(p2[0], 2.0 / 3) && near(p2[1], 2.0 / 3) && near(p2[2], 1), num(p1[1]) + "," + num(p2[0]));
        po.posterize = 2;
        const auto p3 = rgb(po, 0.49f, 0.51f, 0.1f);
        check("posterize 2: black or white per channel", near(p3[0], 0) && near(p3[1], 1) && near(p3[2], 0), "");
        auto th = A::fresh(K::Threshold);
        const auto t1 = rgb(th, 0.6f, 0.6f, 0.6f), t2 = rgb(th, 0.4f, 0.4f, 0.4f), t3 = rgb(th, 1.f, 0.f, 0.f);
        check("threshold 128: lighter -> white, darker -> black, by luminance (pure red is dark)",
              near(t1[0], 1) && near(t1[2], 1) && near(t2[0], 0) && near(t3[0], 0) && near(t3[1], 0), "");
        th.threshold = 50;
        check("threshold 50: pure red (lum 76) turns white", near(rgb(th, 1.f, 0.f, 0.f)[1], 1), "");
        auto ex = A::fresh(K::Exposure);
        check("exposure: a fresh one changes nothing", A::identity(ex) && near(rgb(ex, 0.3f, 0.3f, 0.3f)[0], 0.3, 0.003), "");
        ex.exposure = 1;
        const float lin = std::pow((0.5f + 0.055f) / 1.055f, 2.4f) * 2;   // mid grey's light, doubled
        const float want = 1.055f * std::pow(lin, 1 / 2.4f) - 0.055f;
        check("exposure +1: the light doubled, in linear light", near(rgb(ex, 0.5f, 0.5f, 0.5f)[0], want, 0.003) &&
              near(rgb(ex, 0.f, 0.f, 0.f)[0], 0), num(rgb(ex, 0.5f, 0.5f, 0.5f)[0]) + " vs " + num(want));
        ex = A::fresh(K::Exposure); ex.offset = 0.1;
        check("exposure offset +0.1: black lifts", rgb(ex, 0.f, 0.f, 0.f)[0] > 0.3f, num(rgb(ex, 0.f, 0.f, 0.f)[0]));
        ex = A::fresh(K::Exposure); ex.exp_gamma = 2;
        check("exposure gamma 2: the middle lightens, ends stay", rgb(ex, 0.5f, 0.5f, 0.5f)[0] > 0.6f &&
              near(rgb(ex, 1.f, 1.f, 1.f)[0], 1) && near(rgb(ex, 0.f, 0.f, 0.f)[0], 0), "");
        auto bad = A::fresh(K::Exposure); bad.exposure = 30; bad.offset = -1; bad.exp_gamma = 0;
        auto bp = A::fresh(K::Posterize); bp.posterize = 1;
        auto bt = A::fresh(K::Threshold); bt.threshold = 300;
        check("s061: out of range refused, normalised into range", !A::refusal(bad).empty() && !A::refusal(bp).empty() && !A::refusal(bt).empty() &&
              A::normalised(bad).exposure == 20 && A::normalised(bad).offset == -0.5 && A::normalised(bad).exp_gamma == 0.01 &&
              A::normalised(bp).posterize == 2 && A::normalised(bt).threshold == 255, "");

        // As layers: Invert over the red square shows cyan; white shows black.
        Fix t;
        const auto id = t.adj(K::Invert);
        check("invert layer: made, one step", !id.empty() && t.j.size() == 1, "");
        check("invert layer: red -> cyan, white -> black", near(t.seen(150, 150).r, 0) && near(t.seen(150, 150).g, 1) && near(t.seen(150, 150).b, 1) &&
              near(t.seen(20, 20).r, 0), "");
        const auto th2 = t.adj(K::Threshold);   // over the cyan (lum 179) and the black
        check("threshold layer over it: cyan -> white, black stays", near(t.seen(150, 150).r, 1) && near(t.seen(20, 20).g, 0), "");
        t.j.undo(); t.j.undo();
        check("undo: both gone, red again", !t.d.get(id) && !t.d.get(th2) && near(t.seen(150, 150).r, 1) && near(t.seen(150, 150).g, 0), "");

        // On pixels: Invert, one step; twice is back where it was.
        Fix u;
        const auto before = u.d.get("L4")->pixels;
        check("invert on pixels: one step", L::apply_adjust(u.j, u.d, "L4", A::fresh(K::Invert)) && u.j.size() == 1 &&
              near(u.seen(150, 150).g, 1) && near(u.seen(150, 150).r, 0), "");
        L::apply_adjust(u.j, u.d, "L4", A::fresh(K::Invert));
        const auto px = u.d.get("L4")->pixels->get(150, 150), was = before->get(150, 150);
        check("invert twice: the pixels back", near(px.c[0], was.c[0], 1e-3) && near(px.c[1], was.c[1], 1e-3) && near(px.c[3], was.c[3], 1e-3), "");

        // The .klimt keeps them all.
        Fix f;
        auto pz = A::fresh(K::Posterize); pz.posterize = 7;
        auto tz = A::fresh(K::Threshold); tz.threshold = 90;
        auto ez = A::fresh(K::Exposure); ez.exposure = -1.25; ez.offset = 0.0123; ez.exp_gamma = 1.4;
        const auto i1 = f.adj(K::Invert), i2 = f.adj(K::Posterize), i3 = f.adj(K::Threshold), i4 = f.adj(K::Exposure);
        f.set(i2, pz); f.set(i3, tz); f.set(i4, ez);
        core::file::Contents c;
        c.image = f.d.image(); c.layers = f.d.snapshot(); c.next_id = f.d.next_id();
        const auto dir = fs::temp_directory_path() / "klimt_selftest_adjust61";
        fs::create_directories(dir);
        const auto path = (dir / "s061.klimt").string();
        std::string err;
        const bool saved = core::file::save_klimt(path, c, err);
        const auto back = saved ? core::file::open(path, err) : std::nullopt;
        bool same = back.has_value();
        if (back)
            for (const auto& i : {i1, i2, i3, i4}) same = same && back->layers.at(i).adjust == c.layers.at(i).adjust;
        check("file: Invert, Posterize 7, Threshold 90, Exposure -1.25 / 0.0123 / 1.4 come back", same, err);
        fs::remove_all(dir);
    }

    std::cout << "\n-- adjustments: Vibrance, Black & White, Photo Filter, Threshold's histogram (s062) --\n";
    {
        using K = core::AdjustKind;
        const auto rgb = [](const core::Adjust& a, float r, float g, float b) { A::apply_rgb(a, r, g, b); return std::array<float, 3>{r, g, b}; };
        const auto chroma = [](std::array<float, 3> c) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); };
        check("s062: eleven kinds (s063: more); keys round trip", A::kinds().size() >= 11 && A::from_key("vibrance") == K::Vibrance &&
              A::from_key("black-white") == K::BlackWhite && A::from_key("photo-filter") == K::PhotoFilter &&
              A::label(K::BlackWhite) == "Black & White", "");
        check("s062: Levels, Curves, Threshold want the histogram; Hue/Saturation doesn't",
              A::wants_histogram(K::Threshold) && A::wants_histogram(K::Levels) && A::wants_histogram(K::Curves) && !A::wants_histogram(K::HueSat), "");
        // Vibrance: a dull colour gains more than a strong one; grey stays grey.
        auto vb = A::fresh(K::Vibrance);
        check("vibrance: a fresh one changes nothing", A::identity(vb), "");
        vb.vibrance = 80;
        const std::array<float, 3> dull{0.55f, 0.5f, 0.45f}, strong{0.9f, 0.1f, 0.1f};
        const auto d2 = rgb(vb, dull[0], dull[1], dull[2]), s2 = rgb(vb, strong[0], strong[1], strong[2]);
        check("vibrance +80: the dull colour's chroma grows by more than the strong one's (relatively)",
              chroma(d2) / chroma(dull) > chroma(s2) / chroma(strong) && chroma(d2) > chroma(dull) + 0.05f, num(chroma(d2)) + " / " + num(chroma(s2)));
        const auto g2 = rgb(vb, 0.4f, 0.4f, 0.4f);
        check("vibrance: grey stays grey", near(g2[0], 0.4) && near(g2[1], 0.4) && near(g2[2], 0.4), "");
        auto sat = A::fresh(K::Vibrance); sat.saturation = -100;
        const auto s3 = rgb(sat, 0.9f, 0.1f, 0.1f);
        check("vibrance: Saturation -100 is grey (its luma)", near(s3[0], s3[1]) && near(s3[1], s3[2]) && near(s3[0], 0.299 * 0.9 + 0.587 * 0.1 + 0.114 * 0.1), num(s3[0]));
        // Black & White: Photoshop's defaults -- red 40 %, yellow 60 %, blue 20 %.
        auto bw = A::fresh(K::BlackWhite);
        const auto r1 = rgb(bw, 1, 0, 0), y1 = rgb(bw, 1, 1, 0), b1 = rgb(bw, 0, 0, 1), w1 = rgb(bw, 1, 1, 1), m1 = rgb(bw, 0.5f, 0.5f, 0.5f);
        check("black & white (defaults): red 0.4, yellow 0.6, blue 0.2", near(r1[0], 0.4) && near(r1[2], 0.4) && near(y1[1], 0.6) && near(b1[0], 0.2), "");
        check("black & white: white and grey stay", near(w1[0], 1) && near(m1[1], 0.5), "");
        bw.bw[0] = 100;
        check("black & white: Reds 100 -- red turns white", near(rgb(bw, 1, 0, 0)[1], 1), "");
        bw = A::fresh(K::BlackWhite); bw.bw_tint = true; bw.tint_hue = 30; bw.tint_sat = 60;
        const auto t1 = rgb(bw, 0.5f, 0.5f, 0.5f);
        check("black & white: Tint colours the grey (orange: red > green > blue)", t1[0] > t1[1] && t1[1] > t1[2], num(t1[0]) + "," + num(t1[2]));
        // Photo Filter.
        auto pf = A::fresh(K::PhotoFilter);
        check("photo filter: Warming (85), 25 %, luminosity kept, to start", near(pf.filter[0] * 255, 236, 1e-6) && pf.density == 25 && pf.keep_luma && !A::identity(pf), "");
        const auto p1 = rgb(pf, 0.5f, 0.5f, 0.5f);
        const double l0 = 0.5, l1 = 0.299 * p1[0] + 0.587 * p1[1] + 0.114 * p1[2];
        check("photo filter: grey warms (red > blue), as light as before", p1[0] > p1[2] + 0.05f && near(l1, l0, 0.005), num(l1));
        pf.keep_luma = false;
        const auto p2 = rgb(pf, 0.5f, 0.5f, 0.5f);
        check("photo filter: without Preserve Luminosity it darkens", 0.299 * p2[0] + 0.587 * p2[1] + 0.114 * p2[2] < 0.48, "");
        pf.density = 100; pf.filter = {0, 0, 1};
        const auto p3 = rgb(pf, 1, 1, 1);
        check("photo filter: Deep blue at 100 %, no luminosity -- white turns blue", near(p3[0], 0) && near(p3[2], 1), "");
        auto bad = A::fresh(K::BlackWhite); bad.bw[2] = 400; bad.tint_sat = 120;
        auto badp = A::fresh(K::PhotoFilter); badp.density = 0; badp.filter[1] = 2;
        auto badv = A::fresh(K::Vibrance); badv.vibrance = 150;
        check("s062: out of range refused, normalised into range", !A::refusal(bad).empty() && !A::refusal(badp).empty() && !A::refusal(badv).empty() &&
              A::normalised(bad).bw[2] == 300 && A::normalised(bad).tint_sat == 100 && A::normalised(badp).density == 1 &&
              A::normalised(badp).filter[1] == 1 && A::normalised(badv).vibrance == 100, "");
        // As a layer: Black & White over the red square, at the defaults.
        Fix t;
        const auto id = t.adj(K::BlackWhite);
        check("black & white layer: the red square turns grey 0.4, white stays", near(t.seen(150, 150).r, 0.4) && near(t.seen(150, 150).b, 0.4) &&
              near(t.seen(20, 20).g, 1), num(t.seen(150, 150).r));
        // The .klimt keeps them.
        Fix f;
        auto v = A::fresh(K::Vibrance); v.vibrance = 35; v.saturation = -12;
        auto b = A::fresh(K::BlackWhite); b.bw = {10, -50, 120, 300, -200, 77}; b.bw_tint = true; b.tint_hue = 210; b.tint_sat = 33;
        auto p = A::fresh(K::PhotoFilter); p.filter = {0.1, 0.2, 0.3}; p.density = 64; p.keep_luma = false;
        const auto i1 = f.adj(K::Vibrance), i2 = f.adj(K::BlackWhite), i3 = f.adj(K::PhotoFilter);
        f.set(i1, v); f.set(i2, b); f.set(i3, p);
        core::file::Contents c;
        c.image = f.d.image(); c.layers = f.d.snapshot(); c.next_id = f.d.next_id();
        const auto dir = fs::temp_directory_path() / "klimt_selftest_adjust62";
        fs::create_directories(dir);
        const auto path = (dir / "s062.klimt").string();
        std::string err;
        const bool saved = core::file::save_klimt(path, c, err);
        const auto back = saved ? core::file::open(path, err) : std::nullopt;
        bool same = back.has_value();
        if (back)
            for (const auto& i : {i1, i2, i3}) same = same && back->layers.at(i).adjust == c.layers.at(i).adjust;
        check("file: Vibrance, Black & White (mix, tint), Photo Filter (colour, density, luminosity) come back", same, err);
        fs::remove_all(dir);
        (void)id;
    }

    std::cout << "\n-- adjustments: Colour Balance, Channel Mixer, Selective Colour, Gradient Map (s063) --\n";
    {
        using K = core::AdjustKind;
        const auto rgb = [](const core::Adjust& a, float r, float g, float b) { A::apply_rgb(a, r, g, b); return std::array<float, 3>{r, g, b}; };
        const auto hsl_l = [](std::array<float, 3> c) { return (std::max({c[0], c[1], c[2]}) + std::min({c[0], c[1], c[2]})) / 2; };
        // ── Colour Balance ──
        auto cb = A::fresh(K::ColourBalance);
        check("colour balance: key, label; a fresh one changes nothing, Preserve Luminosity on",
              A::from_key("colour-balance") == K::ColourBalance && A::label(K::ColourBalance) == "Colour Balance" && A::identity(cb) && cb.keep_luma, "");
        cb.balance[1][0] = 100; cb.keep_luma = false;
        const auto m1 = rgb(cb, 0.5f, 0.5f, 0.5f);
        check("colour balance: Midtones toward Red reddens mid grey", m1[0] > 0.6f && near(m1[1], 0.5) && near(m1[2], 0.5), num(m1[0]));
        cb.keep_luma = true;
        const auto m2 = rgb(cb, 0.5f, 0.5f, 0.5f);
        check("colour balance: with Preserve Luminosity -- redder, as light (HSL) as before", m2[0] > m2[1] + 0.05f && near(hsl_l(m2), 0.5, 0.005), num(hsl_l(m2)));
        auto sh = A::fresh(K::ColourBalance); sh.balance[0][2] = 100; sh.keep_luma = false;   // Shadows toward Blue
        const float dark = rgb(sh, 0.1f, 0.1f, 0.1f)[2] - 0.1f, light = rgb(sh, 0.9f, 0.9f, 0.9f)[2] - 0.9f;
        check("colour balance: Shadows move the darks, not the lights", dark > 0.3f && std::abs(light) < 0.01f, num(dark) + " / " + num(light));
        auto hi = A::fresh(K::ColourBalance); hi.balance[2][1] = -100; hi.keep_luma = false;   // Highlights toward Magenta
        const float hl = 0.8f - rgb(hi, 0.8f, 0.8f, 0.8f)[1], hd = 0.15f - rgb(hi, 0.15f, 0.15f, 0.15f)[1];
        check("colour balance: Highlights move the lights, not the darks", hl > 0.3f && std::abs(hd) < 0.01f, num(hl) + " / " + num(hd));
        auto bad = A::fresh(K::ColourBalance); bad.balance[2][0] = 150;
        check("colour balance: out of range refused, normalised to 100", !A::refusal(bad).empty() && A::normalised(bad).balance[2][0] == 100, "");
        Fix t;
        const auto id = t.adj(K::ColourBalance);
        auto warm = A::fresh(K::ColourBalance); warm.balance[2][2] = -100; warm.keep_luma = false;   // Highlights toward Yellow
        t.set(id, warm);
        check("colour balance layer: the white background loses blue, one step more", t.seen(20, 20).b < 0.8 && t.j.size() == 2, num(t.seen(20, 20).b));

        // ── Channel Mixer ──
        auto mx = A::fresh(K::ChannelMixer);
        check("channel mixer: a fresh one changes nothing (each output its own channel)", A::identity(mx) && A::from_key("channel-mixer") == K::ChannelMixer, "");
        mx.mixer[0] = {0, 0, 100, 0}; mx.mixer[2] = {100, 0, 0, 0};   // swap red and blue
        const auto sw = rgb(mx, 0.9f, 0.5f, 0.1f);
        check("channel mixer: red and blue swapped", near(sw[0], 0.1) && near(sw[1], 0.5) && near(sw[2], 0.9), "");
        mx = A::fresh(K::ChannelMixer); mx.mixer[1] = {0, 100, 0, 20};
        check("channel mixer: Constant 20 lifts green by 0.2", near(rgb(mx, 0.5f, 0.5f, 0.5f)[1], 0.7) && near(rgb(mx, 0.5f, 0.5f, 0.5f)[0], 0.5), "");
        mx = A::fresh(K::ChannelMixer); mx.monochrome = true;
        const auto gr = rgb(mx, 1, 0, 0);
        check("channel mixer: Monochrome (40 40 20) -- pure red is grey 0.4", !A::identity(mx) && near(gr[0], 0.4) && near(gr[1], 0.4) && near(gr[2], 0.4), num(gr[0]));
        mx = A::fresh(K::ChannelMixer); mx.mixer[0] = {200, 0, 0, 0};
        check("channel mixer: 200 % red burns out (clamped)", near(rgb(mx, 0.8f, 0, 0)[0], 1), "");
        auto bm = A::fresh(K::ChannelMixer); bm.mono[3] = -300;
        check("channel mixer: out of range refused, normalised to -200", !A::refusal(bm).empty() && A::normalised(bm).mono[3] == -200, "");

        // ── Selective Colour ──
        auto sc = A::fresh(K::SelectiveColour);
        check("selective colour: a fresh one changes nothing, Relative", A::identity(sc) && !sc.selective_absolute &&
              A::from_key("selective-colour") == K::SelectiveColour, "");
        sc.selective[0][0] = 100; sc.selective_absolute = true;   // Reds: +100 cyan, Absolute
        const auto sr = rgb(sc, 1, 0, 0), sb = rgb(sc, 0, 0, 1);
        check("selective colour: Reds +100 cyan (Absolute) -- pure red loses its red; blue untouched",
              near(sr[0], 0) && near(sb[2], 1) && near(sb[0], 0), num(sr[0]));
        sc.selective_absolute = false;
        const auto so = rgb(sc, 0.8f, 0.2f, 0.2f);
        check("selective colour: Relative -- pure red (no cyan ink) stays, a duller red loses some",
              near(rgb(sc, 1, 0, 0)[0], 1) && so[0] < 0.75f && so[0] > 0.6f, num(so[0]));
        auto nk = A::fresh(K::SelectiveColour); nk.selective[7][3] = 50;   // Neutrals: +50 black
        const auto ng = rgb(nk, 0.5f, 0.5f, 0.5f), nw = rgb(nk, 1, 1, 1), nr = rgb(nk, 1, 0, 0);
        check("selective colour: Neutrals +50 black darkens mid grey; white and pure red stay",
              ng[0] < 0.4f && near(ng[0], ng[2]) && near(nw[0], 1) && near(nr[0], 1), num(ng[0]));
        auto wk = A::fresh(K::SelectiveColour); wk.selective[6][2] = 100; wk.selective_absolute = true;   // Whites: +100 yellow
        const auto wy = rgb(wk, 1, 1, 1), wd = rgb(wk, 0.2f, 0.2f, 0.2f);
        check("selective colour: Whites +100 yellow (Absolute) -- white turns yellow, a dark grey doesn't",
              near(wy[2], 0) && near(wy[0], 1) && near(wd[2], 0.2), num(wy[2]));
        auto bs = A::fresh(K::SelectiveColour); bs.selective[3][1] = -101;
        check("selective colour: out of range refused, normalised to -100", !A::refusal(bs).empty() && A::normalised(bs).selective[3][1] == -100, "");

        // ── Gradient Map ──
        auto gm = A::fresh(K::GradientMap);
        check("gradient map: black to white to start, its own effect", gm.grad.size() == 2 && gm.grad[0].rgb[0] == 0 && gm.grad[1].rgb[2] == 1 &&
              !A::identity(gm) && A::from_key("gradient-map") == K::GradientMap, "");
        const auto gred = rgb(gm, 1, 0, 0);
        check("gradient map: pure red turns its luminosity's grey (0.299)", near(gred[0], 0.299) && near(gred[1], 0.299) && near(gred[2], 0.299), num(gred[0]));
        gm.grad_reverse = true;
        check("gradient map: Reverse -- white turns black", near(rgb(gm, 1, 1, 1)[0], 0), "");
        gm = A::fresh(K::GradientMap);
        gm.grad = {{0, 0.5, {0, 0, 1}}, {0.5, 0.5, {1, 0, 0}}, {1, 0.5, {1, 1, 0}}};
        const auto mid = rgb(gm, 0.5f, 0.5f, 0.5f), q = rgb(gm, 0.25f, 0.25f, 0.25f);
        check("gradient map: three stops -- mid grey is the middle stop's red, a quarter halfway blue to red",
              near(mid[0], 1) && near(mid[2], 0) && near(q[0], 0.5) && near(q[2], 0.5), num(q[0]));
        gm.grad[1].mid = 0.2;   // blue..red mix half and half at a fifth of the way
        const auto c5 = A::gradient_at(gm.grad, 0.1);
        check("gradient map: a midpoint moved to 20 % -- half and half there", near(c5[0], 0.5) && near(c5[2], 0.5), num(c5[0]));
        auto un = A::fresh(K::GradientMap);
        un.grad = {{0.9, 0.5, {1, 1, 1}}, {0.1, 0.5, {0, 0, 0}}};
        auto one = A::fresh(K::GradientMap); one.grad = {{0.5, 0.5, {1, 0, 0}}};
        check("gradient map: stops out of order or fewer than two refused; normalised sorted / back to black and white",
              !A::refusal(un).empty() && A::normalised(un).grad[0].at == 0.1 && !A::refusal(one).empty() && A::normalised(one).grad == A::fresh(K::GradientMap).grad, "");
        auto many = A::fresh(K::GradientMap);
        many.grad.clear();
        for (int i = 0; i < 40; ++i) many.grad.push_back({i / 39.0, 0.5, {0, 0, 0}});
        check("gradient map: at most 32 stops", A::normalised(many).grad.size() == core::kGradStops && !A::refusal(many).empty(), "");
        Fix g;
        const auto gid = g.adj(K::GradientMap);
        check("gradient map layer: the red square turns grey 0.299, white stays white", near(g.seen(150, 150).r, 0.299) && near(g.seen(150, 150).b, 0.299) &&
              near(g.seen(20, 20).g, 1) && !gid.empty(), num(g.seen(150, 150).r));

        // ── s064: Hue/Saturation's colour ranges ──
        auto hs = A::fresh(K::HueSat);
        check("hue/sat ranges: Photoshop's six bands to start, changing nothing", A::identity(hs) && hs.ranges[0].band == core::kHueBands[0] &&
              hs.ranges[5].band == std::array<int, 4>{255, 285, 315, 345}, "");
        check("hue/sat ranges: Reds' weight -- 1 at 0, 1 at 15, half at 30, none at 60, half at 330",
              near(A::range_weight(core::kHueBands[0], 0), 1) && near(A::range_weight(core::kHueBands[0], 15), 1) &&
              near(A::range_weight(core::kHueBands[0], 30), 0.5) && near(A::range_weight(core::kHueBands[0], 60), 0) &&
              near(A::range_weight(core::kHueBands[0], 330), 0.5), "");
        hs.ranges[0].saturation = -100;
        const auto rr = rgb(hs, 1, 0, 0), bb = rgb(hs, 0, 0, 1), gg = rgb(hs, 0.4f, 0.4f, 0.4f);
        check("hue/sat ranges: Reds saturation -100 -- red turns grey, blue stays, grey stays", !A::identity(hs) &&
              near(rr[0], rr[1]) && near(rr[1], rr[2]) && near(bb[2], 1) && near(bb[0], 0) && near(gg[0], 0.4), num(rr[0]));
        hs = A::fresh(K::HueSat); hs.ranges[0].lightness = 100;
        const auto gl = rgb(hs, 0.4f, 0.4f, 0.4f);
        check("hue/sat ranges: Reds lightness +100 -- red turns white, a grey (hue 0 in HSL) doesn't move", near(rgb(hs, 1, 0, 0)[1], 1) && near(gl[0], 0.4), num(gl[0]));
        hs = A::fresh(K::HueSat); hs.ranges[4].hue = 180; hs.hue = 10;
        const auto bh = rgb(hs, 0, 0, 1), rh = rgb(hs, 1, 0, 0);
        check("hue/sat ranges: Blues hue +180 with the master's +10 -- blue goes yellowish, red only the master's 10",
              bh[0] > 0.8f && bh[1] > 0.6f && bh[2] < 0.1f && rh[0] > 0.9f && rh[1] > 0.1f && rh[1] < 0.25f && rh[2] < 0.02f, num(bh[1]) + "," + num(rh[1]));
        hs = A::fresh(K::HueSat); hs.colorize = true; hs.hue = 200; hs.saturation = 50; hs.ranges[0].lightness = 100;
        auto hs2 = hs; hs2.ranges[0].lightness = 0;
        const auto cz1 = rgb(hs, 1, 0, 0), cz2 = rgb(hs2, 1, 0, 0);
        check("hue/sat ranges: Colorize leaves the ranges be", near(cz1[0], cz2[0], 1e-6) && near(cz1[2], cz2[2], 1e-6), "");
        auto bnd = A::fresh(K::HueSat); bnd.ranges[1].band = {400, 30, 20, 120}; bnd.ranges[2].hue = 190;
        const auto nb = A::normalised(bnd);
        check("hue/sat ranges: bands wrapped into 0..359 and kept in order; out of range refused",
              !A::refusal(bnd).empty() && nb.ranges[1].band[0] == 40 && nb.ranges[1].band[2] >= 30 && nb.ranges[2].hue == 180 &&
              A::refusal(nb).empty(), std::to_string(nb.ranges[1].band[2]));

        // ── s064: Auto ──
        A::Histogram h{};
        for (int v = 40; v <= 200; ++v) { h[1][static_cast<std::size_t>(v)] = 100; h[2][static_cast<std::size_t>(v + 20)] = 100; }
        h[3][128] = 1000;          // blue: one tone -- left
        h[1][0] = 5; h[1][255] = 5;   // a few strays: under 0.1 %, let go
        const auto al = A::auto_tone(A::fresh(K::Levels), h);
        check("auto: Levels -- red 40..200, green 60..220 stretched; strays let go; one-tone blue and RGB left",
              near(al.levels[1].in_black * 255, 40, 1e-6) && near(al.levels[1].in_white * 255, 200, 1e-6) &&
              near(al.levels[2].in_black * 255, 60, 1e-6) && near(al.levels[2].in_white * 255, 220, 1e-6) &&
              al.levels[3] == core::LevelsCh{} && al.levels[0] == core::LevelsCh{}, num(al.levels[1].in_black * 255));
        const auto ac = A::auto_tone(A::fresh(K::Curves), h);
        const auto ct = A::curve_table(ac.curves[1], 256);
        check("auto: Curves -- red a straight line from 40 to 200 (120 maps to half)", ac.curves[1].size() == 2 && near(ct[40], 0) &&
              near(ct[200], 1) && near(ct[120], 0.5, 0.01) && ac.curves[3].size() == 2 && near(ac.curves[3][1].y, 1), num(ct[120]));
        A::Histogram empty{};
        check("auto: an empty histogram changes nothing", A::auto_tone(A::fresh(K::Levels), empty) == A::fresh(K::Levels), "");

        // ── s064: the eyedroppers ──
        {
            const std::array<double, 3> warm{0.6, 0.5, 0.3};
            const auto lb = A::dropped(A::fresh(K::Levels), A::Dropper::Black, warm);
            const auto lbo = rgb(lb, 0.6f, 0.5f, 0.3f);
            check("eyedropper: Levels black point -- the picked colour turns black, channel by channel",
                  near(lb.levels[1].in_black, 0.6) && near(lb.levels[3].in_black, 0.3) && near(lbo[0], 0) && near(lbo[1], 0) && near(lbo[2], 0), "");
            const auto lw = A::dropped(A::fresh(K::Levels), A::Dropper::White, warm);
            const auto lwo = rgb(lw, 0.6f, 0.5f, 0.3f);
            check("eyedropper: Levels white point -- the picked colour turns white", near(lwo[0], 1) && near(lwo[1], 1) && near(lwo[2], 1), "");
            const auto lg = A::dropped(A::fresh(K::Levels), A::Dropper::Grey, warm);
            const auto lgo = rgb(lg, 0.6f, 0.5f, 0.3f);
            check("eyedropper: Levels grey point -- the picked colour turns neutral (its average, 0.467)", near(lgo[0], lgo[1], 0.004) &&
                  near(lgo[1], lgo[2], 0.004) && near(lgo[0], 1.4 / 3, 0.006) && lg.levels[0] == core::LevelsCh{}, num(lgo[0]) + "," + num(lgo[2]));
            const auto cg = A::dropped(A::fresh(K::Curves), A::Dropper::Grey, warm);
            const auto cgo = rgb(cg, 0.6f, 0.5f, 0.3f);
            check("eyedropper: Curves grey point -- a point per channel, the colour neutral", cg.curves[1].size() == 3 && near(cgo[0], cgo[2], 0.004) &&
                  near(cgo[0], 1.4 / 3, 0.006), num(cgo[0]));
            const auto cb = A::dropped(A::fresh(K::Curves), A::Dropper::Black, warm), cw = A::dropped(cb, A::Dropper::White, {0.9, 0.95, 0.8});
            check("eyedropper: Curves black then white -- the end points move", near(cw.curves[1].front().x, 0.6) && near(cw.curves[1].front().y, 0) &&
                  near(cw.curves[3].back().x, 0.8) && near(cw.curves[3].back().y, 1) && cw.curves[1].size() == 2, "");
            check("eyedropper: other kinds untouched", A::dropped(A::fresh(K::HueSat), A::Dropper::Black, warm) == A::fresh(K::HueSat), "");
        }

        // ── the .klimt keeps them ──
        Fix f;
        auto c1 = A::fresh(K::ColourBalance); c1.balance = {{{10, -20, 30}, {-40, 50, -60}, {70, -80, 90}}}; c1.keep_luma = false;
        auto c2 = A::fresh(K::ChannelMixer); c2.mixer = {{{10, 20, 30, -5}, {-200, 0, 200, 7}, {1, 2, 3, 4}}}; c2.mono = {33, 33, 34, -10}; c2.monochrome = true;
        auto c3 = A::fresh(K::SelectiveColour); c3.selective[0] = {1, -2, 3, -4}; c3.selective[8] = {100, -100, 50, 7}; c3.selective_absolute = true;
        auto c4 = A::fresh(K::GradientMap); c4.grad = {{0, 0.5, {0.1, 0.2, 0.3}}, {0.4, 0.3, {1, 0, 0.5}}, {1, 0.7, {0, 1, 0}}};
        c4.grad_reverse = true; c4.grad_dither = true;
        auto c6 = A::fresh(K::HueSat); c6.hue = 12; c6.ranges[3] = {{140, 160, 200, 230}, 25, -60, 10};
        const auto i1 = f.adj(K::ColourBalance), i2 = f.adj(K::ChannelMixer), i3 = f.adj(K::SelectiveColour), i4 = f.adj(K::GradientMap), i5 = f.adj(K::HueSat);
        f.set(i1, c1); f.set(i2, c2); f.set(i3, c3); f.set(i4, c4); f.set(i5, c6);
        core::file::Contents c;
        c.image = f.d.image(); c.layers = f.d.snapshot(); c.next_id = f.d.next_id();
        const auto dir = fs::temp_directory_path() / "klimt_selftest_adjust63";
        fs::create_directories(dir);
        const auto path = (dir / "s063.klimt").string();
        std::string err;
        const bool saved = core::file::save_klimt(path, c, err);
        const auto back = saved ? core::file::open(path, err) : std::nullopt;
        bool same = back.has_value();
        if (back)
            for (const auto& i : {i1, i2, i3, i4, i5}) same = same && back->layers.at(i).adjust == c.layers.at(i).adjust;
        check("file (s064: + Hue/Saturation's ranges): Colour Balance (three tones, luminosity), Channel Mixer (outputs, grey, Monochrome), Selective Colour (inks, Absolute), Gradient Map (stops, midpoints, Reverse, Dither) come back", same, err);
        fs::remove_all(dir);
    }
}
