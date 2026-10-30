// klimt_selftest -- layer masks (s012 m2): what they show, the verbs, and
// that every verb that moves a layer moves its mask with it.
#include "core/Canvas.hpp"
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Move.hpp"
#include "core/Paths.hpp"   // s036: a path's edges
#include "core/Selection.hpp"
#include "core/Transform.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace S = klimt::core::select;
namespace fs = std::filesystem;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
const P::Px kWhite = P::rgba(1, 1, 1, 1);
struct Fix {
    core::Document d;
    core::DocJournal j{d};
    Fix() {
        d.set_door([this] { return j.open(); });
        L::load_fixture(d);   // Background white; "A" (L4) a red square (100, 100) 200 x 150
    }
    // What you see at (x, y).
    core::Sampled seen(int x, int y) const { return core::sample_composite(d.image(), d.snapshot(), x, y, 0); }
    bool red_at(int x, int y) const { const auto s = seen(x, y); return s.r > 0.99f && s.g < 0.01f; }
    bool white_at(int x, int y) const { const auto s = seen(x, y); return s.r > 0.99f && s.g > 0.99f && s.b > 0.99f; }
    float hide(int x, int y) const {
        const auto l = d.get("L4");
        return l && l->mask ? S::coverage(*l->mask, x, y) : 0.f;
    }
};
}  // namespace

void selftest_masks(const Check& check) {
    std::cout << "\n-- masks: what they show (s012) --\n";
    {
        Fix t;
        check("mask: Reveal All", L::add_mask(t.j, t.d, "L4", L::MaskFrom::RevealAll), "");
        check("mask: ...hides nothing, costs nothing", t.red_at(150, 150) && t.d.get("L4")->mask->tile_count() == 0, "");
        check("mask: one per layer", !L::add_mask(t.j, t.d, "L4", L::MaskFrom::HideAll), "");
        check("mask: the document holds", t.d.check().empty(), t.d.check());
        check("mask: invert -- now it hides all", L::invert_mask(t.j, t.d, "L4") && t.white_at(150, 150), "");
        check("mask: disabled -- it shows nothing", L::set_mask_on(t.j, t.d, "L4", false) && t.red_at(150, 150), "");
        check("mask: enabled again", L::set_mask_on(t.j, t.d, "L4", true) && t.white_at(150, 150), "");
        check("mask: delete", L::delete_mask(t.j, t.d, "L4") && t.red_at(150, 150) && !t.d.get("L4")->mask, "");
        check("mask: a step each", t.j.size() == 5, std::to_string(t.j.size()));
        t.j.undo();
        check("mask: undo brings it back", t.d.get("L4")->mask != nullptr && t.white_at(150, 150), "");
        check("mask: on a group too (s024)", L::add_mask(t.j, t.d, "L2", L::MaskFrom::RevealAll) && (t.j.undo(), true), "");
    }
    {
        Fix t;
        L::load_selection(t.d, {100, 100, 50, 150});
        check("mask: Reveal Selection", L::add_mask(t.j, t.d, "L4", L::MaskFrom::RevealSelection), "");
        check("mask: only the selection shows", t.red_at(120, 150) && t.white_at(200, 150), "");
        check("mask: Apply bakes it into the pixels", L::apply_mask(t.j, t.d, "L4") && !t.d.get("L4")->mask &&
                                                      t.red_at(120, 150) && t.white_at(200, 150) &&
                                                      t.d.get("L4")->pixels->get(200, 150).c[3] <= 0.f, "");
    }
    {
        Fix t;
        L::load_selection(t.d, {100, 100, 50, 150});
        L::add_mask(t.j, t.d, "L4", L::MaskFrom::HideSelection);
        check("mask: Hide Selection", t.white_at(120, 150) && t.red_at(200, 150), "");
    }

    std::cout << "\n-- masks: they go with their layer (s012) --\n";
    {
        Fix t;
        L::load_selection(t.d, {100, 100, 50, 150});
        L::add_mask(t.j, t.d, "L4", L::MaskFrom::RevealSelection);
        S::none(t.j, t.d);
        core::move::by(t.j, t.d, {"L4"}, 100, 0, kWhite);
        check("mask: Move carries it", t.red_at(220, 150) && t.white_at(300, 150) && t.hide(220, 150) < 0.01f &&
                                       t.hide(160, 150) > 0.99f, "");
        check("mask: the document holds", t.d.check().empty(), t.d.check());
    }
    {
        Fix t;
        L::load_selection(t.d, {100, 100, 50, 150});
        L::add_mask(t.j, t.d, "L4", L::MaskFrom::RevealSelection);
        core::move::by(t.j, t.d, {"L4"}, 100, 0, kWhite);   // the selection's pixels move; the mask stays
        check("mask: lifted pixels move alone", t.hide(120, 150) < 0.01f && t.red_at(220, 150) == false, "");
    }
    {
        Fix t;
        L::load_selection(t.d, {100, 100, 50, 150});
        L::add_mask(t.j, t.d, "L4", L::MaskFrom::RevealSelection);
        S::none(t.j, t.d);
        core::xform::flip(t.j, t.d, {"L4"}, true, kWhite);
        check("mask: Flip carries it", t.red_at(280, 150) && t.white_at(120, 150), "");
    }
    {
        Fix t;
        L::load_selection(t.d, {100, 100, 50, 150});
        L::add_mask(t.j, t.d, "L4", L::MaskFrom::RevealSelection);
        S::none(t.j, t.d);
        core::canvas::crop(t.j, t.d, {50, 50, 400, 300}, kWhite);
        check("mask: Crop carries it", t.d.check().empty() && t.red_at(70, 100) && t.white_at(150, 100), t.d.check());
        core::canvas::image_size(t.j, t.d, 200, 150, 72);
        check("mask: Image Size scales it", t.d.check().empty() && t.red_at(35, 50) && t.white_at(75, 50), t.d.check());
        core::xform::turn(t.j, t.d, core::xform::Turn::Clockwise);
        check("mask: a canvas turn turns it", t.d.check().empty() && t.d.image().width == 150, t.d.check());
    }

    std::cout << "\n-- masks: the file (s012) --\n";
    {
        Fix t;
        L::load_selection(t.d, {100, 100, 50, 150});
        L::add_mask(t.j, t.d, "L4", L::MaskFrom::RevealSelection);
        L::set_mask_on(t.j, t.d, "L4", false);
        core::file::Contents c;
        c.image = t.d.image();
        c.layers = t.d.snapshot();
        c.next_id = t.d.next_id();
        const auto dir = fs::temp_directory_path() / "klimt_selftest_masks";
        fs::create_directories(dir);
        const auto path = (dir / "masked.klimt").string();
        std::string err;
        check("mask: saves", core::file::save_klimt(path, c, err), err);
        const auto back = core::file::open(path, err);
        check("mask: opens", back.has_value(), err);
        if (back) {
            const auto& l = back->layers.at("L4");
            check("mask: it comes back, off", l.mask && !l.mask_on && S::coverage(*l.mask, 120, 150) < 0.01f &&
                                              S::coverage(*l.mask, 200, 150) > 0.99f, "");
        }
        fs::remove_all(dir);
    }

    // s036 -- a mask's EDGES: density and feather, live; the mask never changes.
    std::cout << "\n-- masks: edges -- density, feather (s036) --\n";
    {
        Fix t;
        L::load_selection(t.d, {100, 100, 50, 150});   // the square's left 50 px shown
        L::add_mask(t.j, t.d, "L4", L::MaskFrom::RevealSelection);
        const auto raw = t.d.get("L4")->mask;
        core::MaskEdges e;
        check("edges: refused with no change", !L::set_mask_edges(t.j, t.d, "L4", e), "");
        check("edges: refused with no mask", !L::set_mask_edges(t.j, t.d, "L1", core::MaskEdges{50, 0, 0}), "");
        const auto steps = t.j.size();
        e.density = 50;
        check("edges: density 50", L::set_mask_edges(t.j, t.d, "L4", e), "");
        {
            const auto s = t.seen(200, 150);   // hidden half: now half hidden -- pink
            check("edges: ...what it hides shows half", s.r > 0.99f && s.g > 0.45f && s.g < 0.55f, std::to_string(s.g));
            check("edges: ...the shown part as it was", t.red_at(120, 150), "");
            check("edges: ...the mask itself unchanged", t.d.get("L4")->mask == raw, "");
        }
        e.density = 100;
        e.feather = 10;
        check("edges: feather 10", L::set_mask_edges(t.j, t.d, "L4", e), "");
        check("edges: quick presses fold into ONE step", t.j.size() == steps + 1, std::to_string(t.j.size() - steps));
        {
            const auto in = t.seen(110, 150), at = t.seen(152, 150), far = t.seen(200, 150);
            check("edges: ...the edge soft (partly red at the edge)", at.g > 0.2f && at.g < 0.8f, std::to_string(at.g));
            check("edges: ...well inside, still red", in.g < 0.05f, std::to_string(in.g));
            check("edges: ...well outside, still hidden", far.g > 0.98f, std::to_string(far.g));
        }
        check("edges: Apply bakes the soft edge", L::apply_mask(t.j, t.d, "L4") && !t.d.get("L4")->mask &&
                                                   t.d.get("L4")->edges == core::MaskEdges{} &&
                                                   t.seen(152, 150).g > 0.2f && t.seen(152, 150).g < 0.8f, "");
        t.j.undo();
        check("edges: undo -- the mask and its edges back", t.d.get("L4")->mask == raw && t.d.get("L4")->edges.feather == 10, "");
        check("edges: Delete drops them with the mask", L::delete_mask(t.j, t.d, "L4") && t.d.get("L4")->edges == core::MaskEdges{}, "");
        t.j.undo();
        check("edges: clamped", L::set_mask_edges(t.j, t.d, "L4", core::MaskEdges{150, 999, 0}) &&
                                t.d.get("L4")->edges.density == 100 && t.d.get("L4")->edges.feather == 250, "");
        // The file keeps them.
        L::set_mask_edges(t.j, t.d, "L4", core::MaskEdges{70, 6, 0});
        core::file::Contents c;
        c.image = t.d.image();
        c.layers = t.d.snapshot();
        c.next_id = t.d.next_id();
        const auto dir = fs::temp_directory_path() / "klimt_selftest_edges";
        fs::create_directories(dir);
        const auto path = (dir / "edges.klimt").string();
        std::string err;
        check("edges: saves", core::file::save_klimt(path, c, err), err);
        const auto back = core::file::open(path, err);
        check("edges: come back from the file", back && back->layers.at("L4").edges == core::MaskEdges{70, 6, 0}, err);
        fs::remove_all(dir);
    }
    {
        // A path layer's own edges soften ITS mask.
        Fix t;
        const auto id = L::add_path_layer(t.j, t.d, core::paths::rect_path(100, 100, 150, 250), "L4");
        L::set_path_switches(t.j, t.d, id, true, false);
        check("edges: a path's mask -- hard", t.red_at(148, 150) && t.white_at(152, 150), "");
        check("edges: a path layer takes them", L::set_mask_edges(t.j, t.d, id, core::MaskEdges{100, 10, 0}), "");
        const auto at = t.seen(152, 150);
        check("edges: ...its outline softened", at.g > 0.2f && at.g < 0.8f, std::to_string(at.g));
        check("edges: ...no defringe on a path (colours are a pixel layer's)",
              L::set_mask_edges(t.j, t.d, id, core::MaskEdges{100, 10, 8}) == false && t.d.get(id)->edges.defringe == 0, "");
    }

    // s036 m2 -- DEFRINGE: a figure cut from a green ground with a hard mask
    // a few pixels out keeps a green fringe; Defringe takes the green out.
    std::cout << "\n-- masks: Defringe (s036) --\n";
    {
        Fix t;
        const int W = t.d.image().width, H = t.d.image().height;
        auto l = *t.d.get("L4");
        // Red figure (x < 150), a 10 px blend into green, green beyond; a blue
        // detail inside the figure near the edge (x 146..147, y 200..203).
        l.pixels = std::make_shared<const P::TileMap>(l.pixels->paint({0, 0, W, H}, [](int x, int y, const P::Px&) {
            if (x >= 146 && x <= 147 && y >= 200 && y <= 203) return P::rgba(0, 0, 1, 1);
            const float k = std::clamp((x - 150) / 10.f, 0.f, 1.f);   // 0 red .. 1 green
            return P::rgba(1 - k, k, 0, 1);
        }));
        { core::Document::Loading load(t.d); t.d.put("L4", l); }
        L::load_selection(t.d, {0, 0, 155, H});   // the cut: 5 px into the blend -- a fringe
        L::add_mask(t.j, t.d, "L4", L::MaskFrom::RevealSelection);
        const auto before = t.seen(153, 150);
        check("defringe: the fringe is there first (greenish edge)", before.g > 0.2f, std::to_string(before.g));
        check("defringe: 8 px", L::set_mask_edges(t.j, t.d, "L4", core::MaskEdges{100, 0, 8}), "");
        const auto after = t.seen(153, 150);
        check("defringe: ...the green taken out of the edge", after.g < 0.05f && after.r > 0.95f,
              std::to_string(after.r) + " " + std::to_string(after.g));
        check("defringe: ...well inside, untouched", t.red_at(100, 150), "");
        const auto blue = t.seen(147, 201);
        check("defringe: ...a detail that isn't background-coloured keeps its colour", blue.b > 0.95f && blue.r < 0.05f,
              std::to_string(blue.b));
        check("defringe: ...the pixels themselves never change", t.d.get("L4")->pixels == l.pixels, "");
        check("defringe: ...beyond the mask still hidden", t.white_at(200, 150), "");
        check("defringe: Apply bakes the clean colours", L::apply_mask(t.j, t.d, "L4") && t.seen(153, 150).g < 0.05f, "");
        t.j.undo();
        check("defringe: off again", L::set_mask_edges(t.j, t.d, "L4", core::MaskEdges{}) && t.seen(153, 150).g > 0.2f, "");
        // A loose cut: solid background inside the mask (x 160..169) -- it
        // takes the figure's colour.
        L::delete_mask(t.j, t.d, "L4");
        L::load_selection(t.d, {0, 0, 170, H});
        L::add_mask(t.j, t.d, "L4", L::MaskFrom::RevealSelection);
        check("defringe: a loose cut shows solid background inside", t.seen(165, 150).g > 0.95f, "");
        L::set_mask_edges(t.j, t.d, "L4", core::MaskEdges{100, 0, 24});
        const auto ring = t.seen(165, 150);
        check("defringe: ...the solid ring takes the figure's colour", ring.r > 0.9f && ring.g < 0.1f,
              std::to_string(ring.r) + " " + std::to_string(ring.g));
        check("defringe: refused on a group (colours: a pixel layer's)",
              L::add_mask(t.j, t.d, "L2", L::MaskFrom::RevealAll) && !L::set_mask_edges(t.j, t.d, "L2", core::MaskEdges{100, 0, 8}), "");
    }
}
