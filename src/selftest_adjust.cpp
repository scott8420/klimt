// klimt_selftest -- ADJUSTMENTS (s013): the maths, adjustment layers in the
// compositor, their verbs and the file; Image > Adjustments on pixels.
#include "core/Adjust.hpp"
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Selection.hpp"

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
                                         !A::from_key("posterize"), "");
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
}
