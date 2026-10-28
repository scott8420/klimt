// klimt_selftest -- layer masks (s012 m2): what they show, the verbs, and
// that every verb that moves a layer moves its mask with it.
#include "core/Canvas.hpp"
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Move.hpp"
#include "core/Selection.hpp"
#include "core/Transform.hpp"

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
}
