// klimt_selftest -- the Move tool (s009 m1): shifted maps, a layer, a group,
// a lifted selection, the locks, nudges folding into one step.
#include "core/Document.hpp"
#include "core/Move.hpp"
#include "core/Selection.hpp"
#include "core/Transform.hpp"   // s058

#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace M = klimt::core::move;
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
bool red(const P::Px& p) { return p.c[0] > 0.99f && p.c[1] < 0.01f && p.c[3] > 0.99f; }
bool clear(const P::Px& p) { return p.c[3] <= 0.f; }
core::Layer background_layer(std::shared_ptr<const P::TileMap> px) {
    core::Layer l;
    l.name = "Background";
    l.pos = 1;
    l.pixels = std::move(px);
    return l;
}
}  // namespace

void selftest_move(const Check& check) {
    std::cout << "\n-- move: shifted maps (s009) --\n";
    {
        // A map with a pattern across tile seams, slid by odd amounts.
        const P::Format f{P::Depth::U8, P::Mode::RGB, true};
        P::TileMap m(f, 700, 520);
        m = m.paint({0, 0, 700, 520}, [](int x, int y, const P::Px&) {
            return P::rgba(static_cast<float>(x % 251) / 255.f, static_cast<float>(y % 241) / 255.f, 0.5f, 1);
        });
        bool ok = true;
        for (const auto [dx, dy] : {std::array{37, -11}, std::array{-300, 260}, std::array{256, 0}, std::array{1, 1}}) {
            const auto s = M::shifted(m, dx, dy, P::transparent());
            for (int y = 0; y < 520 && ok; y += 7)
                for (int x = 0; x < 700 && ok; x += 5) {
                    const int sx = x - dx, sy = y - dy;
                    const auto want = (sx >= 0 && sy >= 0 && sx < 700 && sy < 520) ? m.get(sx, sy) : P::transparent();
                    if (!(s.get(x, y) == want)) ok = false;
                }
        }
        check("move: shifted copies every pixel to (x+dx, y+dy), across seams", ok, "");
        // s010: slid wholly off, the image holds nothing -- the pixels wait beyond it.
        const auto gone = M::shifted(m, 700, 0, P::transparent());
        bool none_in = true;
        for (int r = 0; r < gone.rows(); ++r) for (int c = 0; c < gone.cols(); ++c) if (gone.tile(c, r)) none_in = false;
        check("move: slid wholly off, nothing inside the image", none_in, "");
        check("move: ...but the pixels are kept beyond it", gone.has_outside() && gone.get(700 + 37, 11) == m.get(37, 11), "");
        const auto back = M::shifted(gone, -700, 0, P::transparent());
        bool same = !back.has_outside();
        for (int y = 0; y < 520 && same; y += 3)
            for (int x = 0; x < 700 && same; x += 5) same = back.get(x, y) == m.get(x, y);
        check("move: slid back, every pixel is home and nothing waits outside", same, "");
        const auto uni = M::shifted(P::TileMap(f, 768, 512).fill({0, 0, 768, 512}, P::rgba(0, 1, 0, 1)), 256, 0, P::transparent());
        check("move: a whole tile of one colour stays uniform", uni.tile(2, 1) && uni.tile(2, 1)->is_uniform() && !uni.tile(0, 0), "");
        const P::Format nf{P::Depth::U8, P::Mode::RGB, false};
        const auto nb = M::shifted(P::TileMap(nf, 300, 300).fill({0, 0, 300, 300}, P::rgba(1, 0, 0)), 50, 0, P::rgba(1, 1, 1));
        check("move: no alpha -- what comes in is the fill colour",
              nb.get(10, 10).c[1] > 0.99f && nb.get(60, 10).c[1] < 0.01f, "");
        check("move: (0, 0) is the very same map", M::shifted(m, 0, 0, P::transparent()).same_tile(m, 0, 0), "");
    }

    std::cout << "\n-- move: layers (s009) --\n";
    {
        Fix t;
        // "A" (L4) holds a red square at (100, 100) 200 x 150.
        check("move: a layer moves", M::by(t.j, t.d, {"L4"}, 37, -20, P::rgba(1, 1, 1, 1)), "");
        check("move: its pixels are (dx, dy) away", red(t.at("L4", 137, 80)) && clear(t.at("L4", 120, 90)) &&
                                                     red(t.at("L4", 336, 229)) && clear(t.at("L4", 338, 229)), "");
        check("move: the other layers stay", red(t.at("L4", 300, 200)) == true && t.at("L1", 0, 0).c[0] > 0.99f, "");
        check("move: one step, labelled Move", t.j.size() == 1 && t.j.undo_label() == "Move", t.j.undo_label());
        t.j.undo();
        check("move: undo puts it back", red(t.at("L4", 100, 100)) && clear(t.at("L4", 300, 250)), "");
        check("move: nothing written outside the door", t.d.strays() == 0, std::to_string(t.d.strays()));
        check("move: (0, 0) keeps no step", !M::by(t.j, t.d, {"L4"}, 0, 0, P::rgba(1, 1, 1, 1)), "");
    }
    {
        Fix t;
        const auto ids = M::targets(t.d, {"L2"});
        check("move: a group carries its pixel layers", ids.size() == 2 && ids[0] == "L4" && ids[1] == "L3", ids.empty() ? "" : ids[0]);
        M::by(t.j, t.d, {"L2"}, -50, 0, P::rgba(1, 1, 1, 1));
        check("move: ...and they move", red(t.at("L4", 50, 100)) && clear(t.at("L4", 260, 100)), "");
        check("move: one step for the whole group", t.j.size() == 1, std::to_string(t.j.size()));
    }
    {
        Fix t;
        core::layers::set_locks(t.j, t.d, "L4", core::lock::Position);
        check("move: the Position lock refuses", !M::refusal(t.d, {"L4"}).empty() && !M::by(t.j, t.d, {"L4"}, 5, 5, P::rgba(1, 1, 1, 1)), "");
        core::layers::set_locks(t.j, t.d, "L4", 0);
        core::layers::set_locks(t.j, t.d, "L2", core::lock::Position);
        check("move: a group's Position lock holds what's inside", !M::refusal(t.d, {"L4"}).empty(), M::refusal(t.d, {"L4"}));
        core::layers::set_locks(t.j, t.d, "L2", core::lock::All);
        check("move: so does a group's All lock", !M::refusal(t.d, {"L3"}).empty(), "");
        check("move: nothing selected: refused", !M::refusal(t.d, {}).empty(), "");
    }

    std::cout << "\n-- move: a lifted selection (s009) --\n";
    {
        Fix t;
        core::layers::load_selection(t.d, {150, 120, 100, 80});   // inside the red square
        check("move: with a selection", M::by(t.j, t.d, {"L4"}, 200, 100, P::rgba(1, 1, 1, 1)), "");
        check("move: the hole it leaves is transparent", clear(t.at("L4", 160, 130)) && clear(t.at("L4", 249, 199)), "");
        check("move: the rest of the square stays", red(t.at("L4", 140, 130)) && red(t.at("L4", 260, 130)), "");
        check("move: the lifted pixels land (dx, dy) away", red(t.at("L4", 350, 220)) && red(t.at("L4", 449, 299)) &&
                                                            clear(t.at("L4", 451, 299)), "");
        const auto* m = t.d.selection();
        check("move: the selection travels with them", m && S::coverage(*m, 355, 225) > 0.99f && S::coverage(*m, 155, 125) < 0.01f, "");
        check("move: the document holds", t.d.check().empty(), t.d.check());
        check("move: one step for pixels and selection", t.j.size() == 1, std::to_string(t.j.size()));
        t.j.undo();
        check("move: undo restores both", red(t.at("L4", 160, 130)) && t.d.selection() &&
                                          S::coverage(*t.d.selection(), 155, 125) > 0.99f, "");
    }
    {
        Fix t;
        core::layers::load_selection(t.d, {150, 120, 100, 80});
        M::by(t.j, t.d, {"L4"}, 1000, 0, P::rgba(1, 1, 1, 1));
        check("move: a selection moved wholly off is none", t.d.selection() == nullptr && t.d.check().empty(), t.d.check());
    }
    {
        Fix t;
        {
            core::Document::Loading load(t.d);
            const P::Format nf{P::Depth::U8, P::Mode::RGB, false};
            t.d.replace({.width = 400, .height = 300, .format = nf},
                        {{"L1", background_layer(std::make_shared<const P::TileMap>(P::TileMap(nf, 400, 300).fill({0, 0, 400, 300}, P::rgba(1, 0, 0))))}});
        }
        core::layers::load_selection(t.d, {10, 10, 50, 50});
        M::by(t.j, t.d, {"L1"}, 100, 0, P::rgba(0, 0, 1, 1));
        check("move: no alpha -- the hole is the background colour", t.at("L1", 20, 20).c[2] > 0.99f && t.at("L1", 20, 20).c[0] < 0.01f, "");
        check("move: ...and the lifted pixels are red", t.at("L1", 120, 20).c[0] > 0.99f, "");
    }

    std::cout << "\n-- move: nudges (s009) --\n";
    {
        Fix t;
        M::by(t.j, t.d, {"L4"}, 1, 0, P::rgba(1, 1, 1, 1), "nudge:L4");
        M::by(t.j, t.d, {"L4"}, 1, 0, P::rgba(1, 1, 1, 1), "nudge:L4");
        M::by(t.j, t.d, {"L4"}, 0, 10, P::rgba(1, 1, 1, 1), "nudge:L4");
        check("move: nudges in a row fold into one step", t.j.size() == 1, std::to_string(t.j.size()));
        check("move: ...that moved by their sum", red(t.at("L4", 102, 110)) && clear(t.at("L4", 101, 109)), "");
        t.j.undo();
        check("move: one undo takes them all back", red(t.at("L4", 100, 100)), "");
    }
    std::cout << "\n-- move: an adjustment layer's mask (s058, WEAKNESSES 31) --\n";
    {
        Fix f;
        const auto id = core::layers::add_adjustment(f.j, f.d, core::AdjustKind::Levels, "L5");
        // Its mask hides a 40 x 30 block at (100, 50).
        const auto hide = std::make_shared<const P::TileMap>(f.d.blank_mask().fill({100, 50, 40, 30}, P::rgba(1, 1, 1)));
        const auto hidden = [&](int x, int y) { const auto l = f.d.get(id); return l && l->mask && l->mask->get(x, y).c[0] > 0.5f; };
        f.j.run("unmask", {id}, [&] { auto l = *f.d.get(id); l.mask = nullptr; f.d.put(id, l); });   // a new one comes with an empty mask
        check("adjust mask: no mask at all -- Move refuses, in words",
              M::refusal(f.d, {id}).find("no mask to move") != std::string::npos, M::refusal(f.d, {id}));
        f.j.run("mask", {id}, [&] { auto l = *f.d.get(id); l.mask = hide; f.d.put(id, l); });
        check("adjust mask: with one, Move takes it", M::refusal(f.d, {id}).empty(), M::refusal(f.d, {id}));
        const bool moved = M::by(f.j, f.d, {id}, 30, 20, P::rgba(1, 1, 1));
        check("adjust mask: the mask slides -- what it hid is now 30 right, 20 down",
              moved && hidden(135, 75) && !hidden(105, 55) && f.d.get(id)->adjust.kind == core::AdjustKind::Levels, "");
        f.j.undo();
        check("adjust mask: one step -- undo puts it back", hidden(105, 55) && !hidden(165, 95), "");
        // A whole group move carries an adjustment inside it.
        // Free Transform: the box is what the mask hides; a flip mirrors it.
        auto mp = M::plan(f.d, {id}, P::rgba(1, 1, 1));
        const auto box = mp ? klimt::core::xform::box_of(*mp) : P::Rect{};
        check("adjust mask: Free Transform's box is what the mask hides", box.x == 100 && box.y == 50 && box.w == 40 && box.h == 30,
              std::to_string(box.x) + "," + std::to_string(box.y) + " " + std::to_string(box.w) + "x" + std::to_string(box.h));
        const auto flip = klimt::core::xform::Affine::about({300, 200}, klimt::core::xform::Affine::scale(-1, 1));
        const bool flipped = klimt::core::xform::apply(f.j, f.d, {id}, flip, "Flip", P::rgba(1, 1, 1));
        check("adjust mask: Flip Horizontal mirrors the mask (600 wide: 100..140 -> 460..500)",
              flipped && hidden(470, 60) && !hidden(110, 60), "");
        f.j.undo();
        // A pixel selection: a mask moves whole -- refused, said.
        f.j.run("sel", {core::kSelectionId}, [&] {
            core::Layer sl; sl.name = "Selection"; sl.pixels = std::make_shared<const P::TileMap>(f.d.blank_mask().fill({0, 0, 10, 10}, P::rgba(1, 1, 1)));
            f.d.put(core::kSelectionId, sl);
        });
        check("adjust mask: with a selection, refused -- \"a mask moves whole\"",
              M::refusal(f.d, {id}).find("moves whole") != std::string::npos, M::refusal(f.d, {id}));
        f.j.undo();
        // Locked in place: refused.
        f.j.run("lock", {id}, [&] { auto l = *f.d.get(id); l.locks = core::lock::Position; f.d.put(id, l); });
        check("adjust mask: locked in place -- refused", M::refusal(f.d, {id}).find("locked") != std::string::npos, "");
    }
    std::cout << "\n-- move: the layer-mask link (s059) --\n";
    {
        Fix f;
        // L5 "Top": red 20 x 20 at (10, 10); its mask hides a block at (200, 100).
        f.j.run("setup", {"L5"}, [&] {
            auto l = *f.d.get("L5");
            l.pixels = std::make_shared<const P::TileMap>(f.d.blank().fill({10, 10, 20, 20}, P::rgba(1, 0, 0)));
            l.mask = std::make_shared<const P::TileMap>(f.d.blank_mask().fill({200, 100, 30, 30}, P::rgba(1, 1, 1)));
            f.d.put("L5", l);
        });
        const auto hidden = [&](int x, int y) { const auto l = f.d.get("L5"); return l->mask && l->mask->get(x, y).c[0] > 0.5f; };
        check("link: a new mask is linked", f.d.get("L5")->mask_linked, "");
        check("link: unlink is one step; again changes nothing (refused)",
              core::layers::set_mask_linked(f.j, f.d, "L5", false) && !f.d.get("L5")->mask_linked &&
                  !core::layers::set_mask_linked(f.j, f.d, "L5", false), "");
        check("link: a group's mask has no link (refused)", !core::layers::set_mask_linked(f.j, f.d, "L2", false), "");
        // Unlinked, the layer picked: its pixels move, the mask stays.
        M::by(f.j, f.d, {"L5"}, 50, 0, P::rgba(1, 1, 1));
        check("link: unlinked -- Move moves the pixels, the mask stays", red(f.at("L5", 65, 15)) && hidden(210, 110) && !hidden(260, 110), "");
        f.j.undo();
        // Unlinked, the MASK what's painted: the mask moves, the pixels stay.
        auto p = M::plan(f.d, {"L5"}, P::rgba(1, 1, 1), "L5");
        auto r = p ? M::compute(*p, 50, 0) : std::nullopt;
        if (r) f.j.run("Move", M::step_ids(*p), [&] { M::put(f.d, *p, *r); });
        check("link: unlinked, the mask painted -- Move moves the mask, the pixels stay",
              r && red(f.at("L5", 15, 15)) && !red(f.at("L5", 65, 15)) && hidden(260, 110) && !hidden(210, 110), "");
        f.j.undo();
        // Linked again, the mask target changes nothing: both move.
        core::layers::set_mask_linked(f.j, f.d, "L5", true);
        auto p2 = M::plan(f.d, {"L5"}, P::rgba(1, 1, 1), "L5");
        auto r2 = p2 ? M::compute(*p2, 50, 0) : std::nullopt;
        if (r2) f.j.run("Move", M::step_ids(*p2), [&] { M::put(f.d, *p2, *r2); });
        check("link: linked -- both move, whichever is painted", r2 && red(f.at("L5", 65, 15)) && hidden(260, 110), "");
        f.j.undo();
        core::layers::set_mask_linked(f.j, f.d, "L5", false);
        // Free Transform on the unlinked mask: its box is what it hides.
        auto xp = klimt::core::xform::plan(f.d, {"L5"}, P::rgba(1, 1, 1), "L5");
        check("link: Free Transform on the unlinked mask -- the box is the mask's", xp && xp->box.x == 200 && xp->box.w == 30, "");
        // A selection with the unlinked mask painted: refused, said.
        f.j.run("sel", {core::kSelectionId}, [&] {
            core::Layer sl; sl.name = "Selection"; sl.pixels = std::make_shared<const P::TileMap>(f.d.blank_mask().fill({0, 0, 50, 50}, P::rgba(1, 1, 1)));
            f.d.put(core::kSelectionId, sl);
        });
        check("link: a selection with the unlinked mask painted -- refused", M::refusal(f.d, {"L5"}, "L5").find("moves whole") != std::string::npos, "");
        check("link: ... the layer painted: lifts pixels as ever", M::refusal(f.d, {"L5"}).empty(), "");
    }
}
