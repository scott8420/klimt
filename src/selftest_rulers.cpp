// klimt_selftest -- RULERS (s031): units and conversions, the major step,
// ticks counted from the zero point, labels; the zero point as a step, in
// the file, carried by Crop / Canvas Size / Image Size.
#include "core/Canvas.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Rulers.hpp"
#include "core/Transform.hpp"

#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace R = klimt::core::rulers;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
std::string num(double v) { return std::to_string(v); }
bool near(double a, double b, double e = 1e-6) { return std::abs(a - b) <= e; }
}  // namespace

void selftest_rulers(const Check& check) {
    std::cout << "\n-- rulers (s031) --\n";
    {
        check("units: seven, keys round-trip", R::units().size() == 7 && [] {
            for (const auto u : R::units()) if (R::from_key(R::key(u)) != u) return false;
            return R::from_key("nope") == R::Unit::Pixels;
        }(), "");
        check("px per unit at 300 dpi: in 300, cm 118.11, mm 11.811, pt 4.1667, pc 50",
              near(R::px_per_unit(R::Unit::Inches, 300, 0), 300) && near(R::px_per_unit(R::Unit::Centimetres, 300, 0), 300 / 2.54) &&
              near(R::px_per_unit(R::Unit::Millimetres, 300, 0), 300 / 25.4) && near(R::px_per_unit(R::Unit::Points, 300, 0), 300.0 / 72) &&
              near(R::px_per_unit(R::Unit::Picas, 300, 0), 50), "");
        check("percent: of the side measured", near(R::px_per_unit(R::Unit::Percent, 72, 2000), 20), "");
        check("format: 600 px at 300 dpi = 2 in; 5.08 cm", R::format(600, R::Unit::Inches, 300, 0) == "2 in" && R::format(600, R::Unit::Centimetres, 300, 0) == "5.08 cm",
              R::format(600, R::Unit::Centimetres, 300, 0));
        check("format: 1000 of 2000 px = 50%", R::format(1000, R::Unit::Percent, 72, 2000) == "50%", R::format(1000, R::Unit::Percent, 72, 2000));
    }
    {
        // The major step: labels at least min_gap screen px apart, a round number.
        const double s1 = R::major_step(R::Unit::Pixels, 72, 0, 1, 64);
        check("step at 100 %: 100 px (64 px apart at least)", near(s1, 100), num(s1));
        const double s2 = R::major_step(R::Unit::Pixels, 72, 0, 0.1, 64);
        check("step at 10 %: 1000 px", near(s2, 1000), num(s2));
        const double s3 = R::major_step(R::Unit::Pixels, 72, 0, 16, 64);
        check("step at 1600 %: 5 px", near(s3, 5), num(s3));
        const double s4 = R::major_step(R::Unit::Inches, 300, 0, 1, 64);
        check("inches at 300 dpi, 100 %: a quarter inch (75 px)", near(s4, 0.25), num(s4));
        bool round = true, apart = true;
        for (const auto u : R::units())
            for (const double z : {0.03, 0.1, 0.33, 1.0, 3.0, 16.0}) {
                const double st = R::major_step(u, 300, 3000, z, 64);
                apart = apart && st * R::px_per_unit(u, 300, 3000) * z >= 64 - 1e-6;
                const double e = st / std::pow(10.0, std::floor(std::log10(st)));
                round = round && (near(e, 1, 1e-6) || near(e, 2, 1e-6) || near(e, 5, 1e-6) || (u == R::Unit::Inches && st < 1));
            }
        check("every unit, every zoom: labels 64+ screen px apart", apart, "");
        check("every unit, every zoom: a round step (1, 2, 5 x 10^n; inch fractions)", round, "");
    }
    {
        // Ticks from the zero point: majors labelled, minors between, 4+ px apart.
        const auto t = R::ticks(R::Unit::Pixels, 72, 0, 0, 0, 1000, 1, 64);
        bool has0 = false, has500 = false;
        double gap = 1e9;
        for (std::size_t i = 0; i < t.size(); ++i) {
            if (t[i].level == 0 && t[i].label == "0" && near(t[i].at, 0)) has0 = true;
            if (t[i].level == 0 && t[i].label == "500" && near(t[i].at, 500)) has500 = true;
            if (i) gap = std::min(gap, t[i].at - t[i - 1].at);
        }
        check("ticks: 0 and 500 labelled at their pixels", has0 && has500, "");
        check("ticks: minors 4+ screen px apart", gap >= 4 - 1e-9, num(gap));
        // A zero point at 250: the label 0 sits at 250; to its left, negatives.
        const auto o = R::ticks(R::Unit::Pixels, 72, 0, 250, 0, 1000, 1, 64);
        bool zero_at = false, neg = false;
        for (const auto& k : o) {
            if (k.level == 0 && k.label == "0") zero_at = near(k.at, 250);
            if (k.level == 0 && k.label == "-200") neg = near(k.at, 50);
        }
        check("ticks: counted from the zero point (0 at 250, -200 at 50)", zero_at && neg, "");
        // Centimetres at 300 dpi, 100 %: a label each centimetre, where it is.
        const auto c = R::ticks(R::Unit::Centimetres, 300, 0, 0, 0, 1200, 1, 64);
        bool cm_ok = false;
        for (const auto& k : c) if (k.level == 0 && k.label == "1") cm_ok = near(k.at, 300 / 2.54, 1e-6);
        check("ticks: 1 cm at 118.11 px (300 dpi)", cm_ok, "");
        check("ticks: nothing when the range is empty", R::ticks(R::Unit::Pixels, 72, 0, 0, 10, 10, 1).empty(), "");
    }
    {
        // The zero point: a step; in the file; carried by Crop / Canvas Size / Image Size.
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);   // 600 x 400
        check("origin: a step", core::canvas::set_ruler_origin(j, d, 120, 80) && d.image().origin_x == 120 && d.image().origin_y == 80, "");
        check("origin: unchanged -- no step", !core::canvas::set_ruler_origin(j, d, 120, 80), "");
        check("origin: its label", j.undo_label() == "Ruler Origin", j.undo_label());
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_origin_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image(); c.layers = d.snapshot(); c.next_id = d.next_id();
        std::string err;
        core::file::save_klimt(path, c, err);
        const auto back = core::file::open(path, err);
        fs::remove(path);
        check("origin: in the file", back && back->image.origin_x == 120 && back->image.origin_y == 80, err);
        core::canvas::crop(j, d, {20, 30, 400, 300}, P::rgba(1, 1, 1, 1));
        check("crop: the zero point stays on the picture (100, 50)", near(d.image().origin_x, 100) && near(d.image().origin_y, 50),
              num(d.image().origin_x));
        j.undo();
        core::canvas::canvas_size(j, d, 800, 600, 1, 1, P::rgba(1, 1, 1, 1));   // anchored at the centre (1 of 0..2)
        check("canvas size (centred): moved with the picture (220, 180)", near(d.image().origin_x, 220) && near(d.image().origin_y, 180),
              num(d.image().origin_x) + "," + num(d.image().origin_y));
        j.undo();
        core::canvas::image_size(j, d, 300, 200, 72);
        check("image size (half): scaled (60, 40)", near(d.image().origin_x, 60) && near(d.image().origin_y, 40), num(d.image().origin_x));
        j.undo();
        j.undo();
        check("undo: back to (0, 0)", d.image().origin_x == 0 && d.image().origin_y == 0, "");
    }
    std::cout << "\n-- guides (s032) --\n";
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);   // 600 x 400
        namespace C = core::canvas;
        namespace G = core::guides;
        check("add: a step, at a whole pixel", C::add_guide(j, d, {true, 150.4}) && d.image().guides.size() == 1 && d.image().guides[0].at == 150 &&
                                               j.undo_label() == "New Vertical Guide", j.undo_label());
        check("add: the same one again refused", !C::add_guide(j, d, {true, 150}), "");
        check("add: off the picture refused", !C::add_guide(j, d, {false, 401}) && !C::add_guide(j, d, {true, -1}), "");
        C::add_guide(j, d, {false, 100});
        check("move: a step", C::move_guide(j, d, 1, 120) && d.image().guides[1].at == 120 && j.undo_label() == "Move Guide", "");
        check("move: off the picture removes it", C::move_guide(j, d, 1, 450) && d.image().guides.size() == 1 && j.undo_label() == "Delete Guide", "");
        j.undo();
        check("undo: back", d.image().guides.size() == 2 && d.image().guides[1].at == 120, "");
        // Snapping: to a guide of the same orientation, within the tolerance.
        bool hit = false;
        check("snap: to the vertical guide at 150 (from 154, tol 5)", near(G::snap(154, true, d.image(), 5, false, &hit), 150) && hit, "");
        check("snap: too far -- unchanged", near(G::snap(160, true, d.image(), 5, false, &hit), 160) && !hit, "");
        check("snap: a horizontal guide doesn't pull x", near(G::snap(122, true, d.image(), 5, false), 122), "");
        check("snap: the picture's edges and centre too", near(G::snap(297, true, d.image(), 5), 300) && near(G::snap(3, false, d.image(), 5), 0) &&
                                                            near(G::snap(398, false, d.image(), 5), 400), "");
        check("hit: the guide near the pointer", G::hit(d.image(), 152, 10, 4) == 0 && G::hit(d.image(), 10, 118, 4) == 1 && G::hit(d.image(), 50, 50, 4) == -1, "");
        // Carried with the picture.
        C::crop(j, d, {50, 20, 400, 300}, P::rgba(1, 1, 1, 1));
        check("crop: guides move with the picture (100, 100)", d.image().guides.size() == 2 && near(d.image().guides[0].at, 100) && near(d.image().guides[1].at, 100), "");
        j.undo();
        C::image_size(j, d, 300, 200, 72);
        check("image size (half): halved (75, 60)", near(d.image().guides[0].at, 75) && near(d.image().guides[1].at, 60), "");
        j.undo();
        core::xform::turn(j, d, core::xform::Turn::Clockwise);   // 600 x 400 -> 400 x 600
        const auto& gs = d.image().guides;
        bool turned = gs.size() == 2 && !gs[0].vertical && gs[1].vertical;
        check("turn 90: a vertical guide is level now, a level one upright", turned,
              gs.size() == 2 ? num(gs[0].at) + (gs[0].vertical ? "v " : "h ") + num(gs[1].at) + (gs[1].vertical ? "v" : "h") : "");
        check("turn 90: at the right places (y 150; x 280)", turned && near(gs[0].at, 150, 1e-6) && near(gs[1].at, 280, 1e-6),
              gs.size() == 2 ? num(gs[0].at) + " / " + num(gs[1].at) : "");
        j.undo();
        core::xform::turn(j, d, core::xform::Turn::FlipHorizontal);
        check("flip: a vertical guide mirrored (450); a level one kept (120)", near(d.image().guides[0].at, 450, 1e-6) && d.image().guides[0].vertical &&
                                                                                near(d.image().guides[1].at, 120, 1e-6), num(d.image().guides[0].at));
        j.undo();
        // The file.
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_guides_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = d.image(); c.layers = d.snapshot(); c.next_id = d.next_id();
        std::string err;
        core::file::save_klimt(path, c, err);
        const auto back = core::file::open(path, err);
        fs::remove(path);
        check("file: the guides kept", back && back->image.guides == d.image().guides, err);
        check("clear: a step", C::clear_guides(j, d) && d.image().guides.empty() && !C::clear_guides(j, d), "");
    }
}
