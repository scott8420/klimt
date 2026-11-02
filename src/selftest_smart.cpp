// klimt_selftest -- s082: SMART LAYERS (core/Smart.hpp). A placed picture
// kept whole: placed at 100 % exactly (or fitted), drawn fresh at every map
// -- a tenth and back loses nothing, where a pixel layer blurs; Move, Free
// Transform (turns, distort), Crop / Image Size / turns compose into its map;
// Warp and Liquify refused; live filters on it; Convert / Rasterize /
// Replace / Embed; linked files -- current, modified, missing, updated; the
// .klimt keeps it all; pasted into another document it stays smart; a
// colour conversion rasterizes it. Rules only.
#include "core/Canvas.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Filter.hpp"
#include "core/Liquify.hpp"
#include "core/Live.hpp"
#include "core/Move.hpp"
#include "core/Profile.hpp"
#include "core/Smart.hpp"
#include "core/Transfer.hpp"
#include "core/Transform.hpp"
#include "formats/Formats.hpp"
#include "pixel/Pixel.hpp"
#include "tint/Tint.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace X = klimt::core::xform;
namespace sm = klimt::core::smart;
namespace fs = std::filesystem;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {

fs::path scratch() {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_smart_" + std::to_string(::getpid()));
        fs::create_directories(p);
        return p;
    }();
    return d;
}

struct Doc {
    core::Document d;
    core::DocJournal j{d};
    Doc() { d.set_door([this] { return j.open(); }); L::load_fixture(d); }
};

// A 120 x 80 picture: a colour ramp with a sharp checker (so a blur shows).
P::TileMap picture(int w = 120, int h = 80, bool alpha = true) {
    const P::Format f{P::Depth::U8, P::Mode::RGB, alpha};
    return P::TileMap(f, w, h).paint({0, 0, w, h}, [&](int x, int y, const P::Px&) {
        const bool chk = ((x / 4) + (y / 4)) % 2 == 0;
        return P::rgba(static_cast<float>(x) / w, chk ? 1.f : 0.f, static_cast<float>(y) / h, 1.f);
    });
}

// The largest channel difference between `m` at (ox, oy) and the picture.
double diff_at(const P::TileMap& m, const P::TileMap& pic, int ox, int oy) {
    double worst = 0;
    for (int y = 0; y < pic.height(); ++y)
        for (int x = 0; x < pic.width(); ++x) {
            const auto a = m.get(ox + x, oy + y), b = pic.get(x, y);
            for (int c = 0; c < 4; ++c) worst = std::max(worst, static_cast<double>(std::abs(a.c[static_cast<std::size_t>(c)] - b.c[static_cast<std::size_t>(c)])));
        }
    return worst;
}
bool same(const P::TileMap& a, const P::TileMap& b) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (!(a.get(x, y) == b.get(x, y))) return false;
    return true;
}
std::string top_smart(const core::Document& d) {
    for (const auto& id : d.walk())
        if (const auto l = d.get(id); l && l->kind == core::Kind::Smart) return id;
    return {};
}
bool write_png(const std::string& f, const P::TileMap& m) {
    klimt::formats::Raster r;
    r.pixels = m;
    std::string err;
    return klimt::formats::write_png(f, r, err);
}

}  // namespace

void selftest_smart(const Check& check) {
    const auto pic = picture();
    // ── m1: placed, kept whole, saved ───────────────────────────────────────
    {
        Doc t;
        const auto id = L::place_smart(t.j, t.d, pic, "ramp.png", 300);
        const auto l = id.empty() ? std::nullopt : t.d.get(id);
        check("smart: placed -- a smart layer, ONE step \"Place “ramp.png”\"",
              l && l->kind == core::Kind::Smart && l->smart && t.j.undo_label() == "Place “ramp.png”", t.j.undo_label());
        check("smart: at 100 %, centred on whole pixels, drawn exactly", l && l->pixels && diff_at(*l->pixels, pic, 240, 160) == 0 &&
              l->smart->source->width() == 120 && sm::scale_of(*l->smart) == 1, l && l->pixels ? std::to_string(diff_at(*l->pixels, pic, 240, 160)) : "");
        check("smart: its resolution kept (said, not drawn)", l && l->smart->dpi == 300, "");
        check("smart: the tree holds", t.d.check().empty(), t.d.check());
        t.j.undo();
        check("smart: undo takes it away", top_smart(t.d).empty(), "");
        // Bigger than the canvas: fitted.
        const auto big = picture(1200, 800);
        const auto bid = L::place_smart(t.j, t.d, big, "big.png", 72);
        const auto bl = bid.empty() ? std::nullopt : t.d.get(bid);
        check("smart: a picture bigger than the canvas fitted (50 %), the picture kept whole",
              bl && std::abs(sm::scale_of(*bl->smart) - 0.5) < 1e-9 && bl->smart->source->width() == 1200, "");
        // A JPEG-like picture with no alpha gains it; refusals.
        const auto opaque = picture(60, 40, false);
        check("smart: a picture with no alpha placed (it gains alpha)", !L::place_smart(t.j, t.d, opaque, "o.jpg", 72).empty(), "");
        core::Image noalpha = t.d.image();
        noalpha.format.alpha = false;
        check("smart: refused into a document with no transparency", !sm::refusal(pic.format(), noalpha).empty(), "");
        check("smart: a CMYK picture refused into RGB", !sm::refusal({P::Depth::U8, P::Mode::CMYK, true}, t.d.image()).empty(), "");
        check("smart: painting refused, in words", L::not_pixels(t.d, bid).find("smart layer") != std::string::npos, L::not_pixels(t.d, bid));
        // The .klimt keeps it whole.
        core::file::Contents c;
        c.image = t.d.image();
        c.layers = t.d.snapshot();
        c.next_id = t.d.next_id();
        std::string err;
        const auto f = (scratch() / "smart.klimt").string();
        auto back = core::file::save_klimt(f, c, err) ? core::file::open(f, err) : std::nullopt;
        const auto bb = back && back->layers.count(bid) ? std::optional<core::Layer>(back->layers.at(bid)) : std::nullopt;
        check("smart: the .klimt keeps the picture whole, its map and resolution",
              bb && bb->kind == core::Kind::Smart && bb->smart && bb->smart->source->width() == 1200 && same(*bb->smart->source, *bl->smart->source) &&
              bb->smart->m == bl->smart->m && bb->smart->name == "big.png", err);
        check("smart: ... and what it draws", bb && bb->pixels && same(*bb->pixels, *bl->pixels), "");
    }
    // ── m1: lossless -- the point of it ─────────────────────────────────────
    {
        Doc t;
        const auto id = L::place_smart(t.j, t.d, pic, "ramp.png", 72);
        const auto src = *t.d.get(id)->smart;
        const X::Projective small = X::Projective(X::Affine::about({300, 200}, X::Affine::scale(0.1, 0.1))) * sm::map_of(src);
        L::set_smart_map(t.j, t.d, id, small, "Scale");
        L::set_smart_map(t.j, t.d, id, sm::map_of(src), "Scale");
        check("smart: scaled to a tenth and back -- exactly the picture again", diff_at(*t.d.get(id)->pixels, pic, 240, 160) == 0,
              std::to_string(diff_at(*t.d.get(id)->pixels, pic, 240, 160)));
        // A pixel layer through the same two transforms blurs (for the record).
        Doc u;
        auto lp = *u.d.get("L4");
        {
            core::Document::Loading load(u.d);
            lp.pixels = std::make_shared<const P::TileMap>(u.d.blank().paint({240, 160, 120, 80}, [&](int x, int y, const P::Px&) { return pic.get(x - 240, y - 160); }));
            u.d.put("L4", lp);
        }
        X::apply(u.j, u.d, {"L4"}, X::Affine::about({300, 200}, X::Affine::scale(0.1, 0.1)), "Scale", P::rgba(1, 1, 1, 1));
        X::apply(u.j, u.d, {"L4"}, X::Affine::about({300, 200}, X::Affine::scale(10, 10)), "Scale", P::rgba(1, 1, 1, 1));
        check("smart: ... where a pixel layer through the same comes back blurred", diff_at(*u.d.get("L4")->pixels, pic, 240, 160) > 0.2,
              std::to_string(diff_at(*u.d.get("L4")->pixels, pic, 240, 160)));
    }
    // ── m2: the transforms compose into its map ─────────────────────────────
    {
        Doc t;
        const auto id = L::place_smart(t.j, t.d, pic, "ramp.png", 72);
        const auto mp = core::move::plan(t.d, {id}, P::rgba(1, 1, 1, 1));
        const auto mr = mp ? core::move::compute(*mp, 13, 7) : std::nullopt;
        if (mp && mr) t.j.run("Move", core::move::step_ids(*mp), [&] { core::move::put(t.d, *mp, *mr); });
        auto l = t.d.get(id);
        check("smart: Move slides its map -- drawn exactly where it went", l->kind == core::Kind::Smart && diff_at(*l->pixels, pic, 253, 167) == 0 &&
              std::abs(sm::corners(*l->smart).p[0].x - 253) < 1e-9, "");
        X::rotate(t.j, t.d, {id}, 90, P::rgba(1, 1, 1, 1));
        l = t.d.get(id);
        const auto q = sm::corners(*l->smart);
        const auto rb = core::canvas::content_bounds(*l->pixels);
        check("smart: Rotate 90 -- still smart, its map turned, drawn 80 x 120",
              l->kind == core::Kind::Smart && std::abs(q.p[1].x - q.p[0].x) < 1e-6 && std::abs(std::abs(q.p[1].y - q.p[0].y) - 120) < 1e-6 &&
              rb.w == 80 && rb.h == 120, std::to_string(rb.w) + " x " + std::to_string(rb.h));
        const auto before = l->smart;
        const bool distorted = X::apply(t.j, t.d, {id}, *X::Projective::box_to_quad(core::canvas::content_bounds(*l->pixels),
                                        {{{200, 120}, {420, 140}, {400, 300}, {190, 280}}}), "Distort", P::rgba(1, 1, 1, 1));
        l = t.d.get(id);
        check("smart: Distort (perspective) -- still smart, the map projective", distorted && l->kind == core::Kind::Smart && l->smart != before &&
              !sm::map_of(*l->smart).affine(), "");
        const auto xp = X::plan(t.d, {id}, P::rgba(1, 1, 1, 1));
        check("smart: Warp refused, in words", xp && !X::warp_refusal(*xp).empty() &&
              !X::warp(t.j, t.d, {id}, [](const P::Rect& b) { return X::warp_preset(X::WarpStyle::Arc, b, 50); }, "Warp", P::rgba(1, 1, 1, 1)),
              xp ? X::warp_refusal(*xp) : "");
        check("smart: Liquify refused, in words", core::liquify::refusal(t.d, id).find("smart layer") != std::string::npos, core::liquify::refusal(t.d, id));
    }
    {
        Doc t;
        const auto id = L::place_smart(t.j, t.d, pic, "ramp.png", 72);
        core::canvas::crop(t.j, t.d, {200, 100, 300, 200}, P::rgba(1, 1, 1, 1));
        auto l = t.d.get(id);
        check("smart: Crop -- its map moves with the picture, drawn exactly", l->kind == core::Kind::Smart && diff_at(*l->pixels, pic, 40, 60) == 0, "");
        core::canvas::image_size(t.j, t.d, 150, 100, 72, core::canvas::Filter::Smooth);
        l = t.d.get(id);
        check("smart: Image Size to half -- its map at 50 %, the picture still whole", std::abs(sm::scale_of(*l->smart) - 0.5) < 1e-9 &&
              l->smart->source->width() == 120 && l->pixels->width() == 150, "");
        core::canvas::image_size(t.j, t.d, 300, 200, 72, core::canvas::Filter::Smooth);
        l = t.d.get(id);
        check("smart: ... and back -- exactly the picture again", diff_at(*l->pixels, pic, 40, 60) == 0, std::to_string(diff_at(*l->pixels, pic, 40, 60)));
        X::turn(t.j, t.d, X::Turn::Clockwise);
        l = t.d.get(id);
        check("smart: the canvas turned -- still smart", l->kind == core::Kind::Smart && l->pixels->width() == 200, "");
        // Live filters run on it -- the smart filters.
        core::Filter blur = core::filter::fresh(core::FilterKind::GaussianBlur);
        check("smart: a live filter on it", L::add_live(t.j, t.d, id, blur) && t.d.get(id)->filters.size() == 1, "");
    }
    // ── m2: converted, rasterized, pasted, recoloured ───────────────────────
    {
        Doc t;
        const auto look = *t.d.get("L4")->pixels;
        check("smart: Convert to Smart Layer -- the look unchanged, the picture its pixels' box",
              L::to_smart(t.j, t.d, "L4") && t.d.get("L4")->kind == core::Kind::Smart && same(*t.d.get("L4")->pixels, look) &&
              t.d.get("L4")->smart->source->width() == 200 && t.d.get("L4")->smart->source->height() == 150, "");
        check("smart: Rasterize -- a pixel layer, the look unchanged, ONE step",
              L::rasterize_smart(t.j, t.d, "L4") && t.d.get("L4")->kind == core::Kind::Pixel && !t.d.get("L4")->smart &&
              same(*t.d.get("L4")->pixels, look) && t.j.undo_label().rfind("Rasterize Smart Layer", 0) == 0, t.j.undo_label());
        check("smart: not on a group", !L::to_smart(t.j, t.d, "L2"), "");
        // Pasted into another document: still smart, the picture whole.
        const auto id = L::place_smart(t.j, t.d, pic, "ramp.png", 72);
        Doc u;
        const auto b = core::transfer::take(t.d, {id});
        const auto made = L::paste(u.j, u.d, b);
        const auto pl = made.empty() ? std::nullopt : u.d.get(made.front());
        check("smart: pasted into another document -- still smart, its picture whole", pl && pl->kind == core::Kind::Smart && pl->smart &&
              pl->smart->source->width() == 120 && u.d.check().empty(), u.d.check());
        // A colour conversion: rasterized (the picture isn't converted yet).
        core::profile::convert(t.j, t.d, std::make_shared<const std::vector<std::uint8_t>>(klimt::tint::adobe_rgb()));
        check("smart: Convert to Profile -- the smart layer becomes pixels of what it drew", t.d.get(id)->kind == core::Kind::Pixel &&
              !t.d.get(id)->smart && t.d.check().empty(), t.d.check());
    }
    // ── m3: linked files ─────────────────────────────────────────────────────
    {
        Doc t;
        const auto f = (scratch() / "linked.png").string();
        write_png(f, pic);
        std::string err;
        const auto ld = sm::load(f, err);
        check("smart: a file read to place -- its pixels, name and stamp", ld && ld->pixels.width() == 120 && ld->name == "linked.png" && ld->bytes > 0, err);
        const auto id = ld ? L::place_smart(t.j, t.d, ld->pixels, ld->name, ld->dpi, f, ld->stamp, ld->bytes) : std::string{};
        const auto l = id.empty() ? std::nullopt : t.d.get(id);
        check("smart: placed linked -- its file current", l && sm::status(*l->smart) == sm::Link::Current, l ? sm::link_label(sm::status(*l->smart)) : "");
        // The file changes.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto pic2 = picture(90, 60);
        write_png(f, pic2);
        check("smart: the file changed -- Modified", l && sm::status(*l->smart) == sm::Link::Modified, "");
        const auto ld2 = sm::load(f, err);
        const bool up = ld2 && L::replace_smart(t.j, t.d, id, ld2->pixels, ld2->name, ld2->dpi, f, ld2->stamp, ld2->bytes, "Update Modified Content");
        const auto l2 = t.d.get(id);
        check("smart: Update -- the new picture, centred where the old one was, current again, ONE step",
              up && l2->smart->source->width() == 90 && sm::status(*l2->smart) == sm::Link::Current && diff_at(*l2->pixels, pic2, 255, 170) == 0 &&
              t.j.undo_label().rfind("Update Modified Content", 0) == 0, t.j.undo_label());
        fs::remove(f);
        check("smart: the file gone -- Missing", sm::status(*t.d.get(id)->smart) == sm::Link::Missing, "");
        check("smart: Embed Linked -- the picture kept, the file forgotten", L::embed_smart(t.j, t.d, id) && t.d.get(id)->smart->link.empty() &&
              sm::status(*t.d.get(id)->smart) == sm::Link::Embedded && t.d.get(id)->smart->source->width() == 90, "");
        check("smart: Embed on an embedded one changes nothing", !L::embed_smart(t.j, t.d, id), "");
        check("smart: reading a file that isn't there -- refused in words", !sm::load(f, err) && !err.empty(), err);
    }
    std::error_code ec;
    fs::remove_all(scratch(), ec);
}
