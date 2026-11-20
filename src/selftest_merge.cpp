// klimt_selftest -- s096: MERGE AND FLATTEN (core/Merge.hpp). Merge Down
// bakes the upper layer (its opacity, mask, an adjustment's effect) into the
// lower one, which keeps its name, opacity and place; refused across a group,
// at the bottom of a group, on hidden or path layers. Merge Visible keeps
// what's hidden; Flatten leaves one "Background"; Stamp adds one and changes
// nothing else. Each is ONE step and undoes whole; what you see is the same
// before and after. A Gray picture merges to Gray. Rules only.
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Merge.hpp"
#include "pixel/Pixel.hpp"

#include <cmath>
#include <functional>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace M = klimt::core::merge;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {

struct Doc {
    core::Document d;
    core::DocJournal j{d};
    Doc() { d.set_door([this] { return j.open(); }); L::load_fixture(d); }
};

bool near(float a, float b, float tol = 0.02f) { return std::fabs(a - b) <= tol; }
std::string px_s(const P::Px& p) {
    return std::to_string(p.c[0]).substr(0, 4) + " " + std::to_string(p.c[1]).substr(0, 4) + " " +
           std::to_string(p.c[2]).substr(0, 4) + " " + std::to_string(p.c[3]).substr(0, 4);
}
P::Px seen(const core::Document& d, int x, int y) {
    const auto f = core::file::flatten(d.image(), d.snapshot());
    return f ? f->get(x, y) : P::Px{};
}
bool same_look(const P::Px& a, const P::Px& b) {
    for (int i = 0; i < 4; ++i) if (!near(a.c[i], b.c[i])) return false;
    return true;
}

}  // namespace

void selftest_merge(const Check& check) {
    // ── Merge Down: A (red square) into B, in the group ──────────────────
    {
        Doc t;
        const auto before = seen(t.d, 150, 120);
        const auto id = M::run(t.j, t.d, M::How::Down, "L4");
        check("merge: Merge Down lands in the lower layer", id == "L3", id);
        check("merge: Merge Down -- the upper layer is gone", !t.d.get("L4"), "");
        const auto b = t.d.get("L3");
        check("merge: Merge Down -- the lower one keeps its name", b && b->name == "B", b ? b->name : "");
        check("merge: Merge Down -- red in its pixels", b && b->pixels && near(b->pixels->get(150, 120).c[0], 1) && near(b->pixels->get(150, 120).c[3], 1),
              b && b->pixels ? px_s(b->pixels->get(150, 120)) : "none");
        check("merge: Merge Down -- one step, its label", t.j.size() == 1 && t.j.undo_label() == "Merge Down “A”", t.j.undo_label());
        check("merge: Merge Down -- the picture looks the same", same_look(before, seen(t.d, 150, 120)), px_s(seen(t.d, 150, 120)));
        check("merge: Merge Down -- the tree holds", t.d.check().empty(), t.d.check());
        t.j.undo();
        check("merge: Merge Down undone -- A is back", t.d.get("L4") && t.d.get("L4")->name == "A" && t.d.get("L3") && !t.d.get("L3")->pixels, "");
    }
    // ── the upper one's opacity is baked; the lower one's is kept ────────
    {
        Doc t;
        L::set_opacity(t.j, t.d, "L4", 50);
        L::set_opacity(t.j, t.d, "L3", 40);
        M::run(t.j, t.d, M::How::Down, "L4");
        const auto b = t.d.get("L3");
        check("merge: Merge Down -- the upper's 50 % is in the pixels", b && b->pixels && near(b->pixels->get(150, 120).c[3], 0.5f, 0.03f),
              b && b->pixels ? px_s(b->pixels->get(150, 120)) : "none");
        check("merge: Merge Down -- the lower's 40 % stays on the layer", b && b->opacity == 40, b ? std::to_string(b->opacity) : "");
    }
    // ── a mask on the upper one is baked ─────────────────────────────────
    {
        Doc t;
        L::load_mask(t.d, "L4");   // hides all but (120, 110) 60 x 60
        M::run(t.j, t.d, M::How::Down, "L4");
        const auto b = t.d.get("L3");
        const bool in = b && b->pixels && near(b->pixels->get(130, 120).c[3], 1);
        const bool out = b && b->pixels && near(b->pixels->get(105, 105).c[3], 0);
        check("merge: Merge Down -- the upper's mask is baked (shown inside, clear outside)", in && out,
              b && b->pixels ? px_s(b->pixels->get(130, 120)) + " / " + px_s(b->pixels->get(105, 105)) : "none");
        check("merge: Merge Down -- the result has no mask of its own", b && !b->mask, "");
    }
    // ── an adjustment layer merged down applies to the lower one ────────
    {
        Doc t;
        {
            core::Document::Loading load(t.d);
            auto top = *t.d.get("L5");
            top.pixels = std::make_shared<const P::TileMap>(t.d.blank().fill({100, 100, 200, 100}, P::rgba(1, 1, 1, 1)));
            t.d.put("L5", top);
        }
        L::load_adjustment(t.d, core::AdjustKind::Invert);   // L6 at the top, its mask showing (120, 110) 60 x 60
        const auto id = M::run(t.j, t.d, M::How::Down, "L6");
        const auto top = t.d.get("L5");
        const bool dark = top && top->pixels && near(top->pixels->get(130, 120).c[0], 0);
        const bool white = top && top->pixels && near(top->pixels->get(250, 150).c[0], 1);
        check("merge: an Invert layer merged down inverts the layer below (inside its mask only)", id == "L5" && dark && white,
              top && top->pixels ? px_s(top->pixels->get(130, 120)) + " / " + px_s(top->pixels->get(250, 150)) : "none");
        check("merge: ... and the adjustment layer is gone", !t.d.get("L6"), "");
    }
    // ── refusals ──────────────────────────────────────────────────────────
    {
        Doc t;
        check("merge: Merge Down refused onto a group", !M::refusal(t.d, M::How::Down, "L5").empty() && M::run(t.j, t.d, M::How::Down, "L5").empty(),
              M::refusal(t.d, M::How::Down, "L5"));
        check("merge: Merge Down refused at the bottom of a group", !M::refusal(t.d, M::How::Down, "L3").empty(), M::refusal(t.d, M::How::Down, "L3"));
        check("merge: Merge Down refused on a group", !M::refusal(t.d, M::How::Down, "L2").empty(), M::refusal(t.d, M::How::Down, "L2"));
        L::set_visible(t.j, t.d, "L4", false);
        check("merge: Merge Down refused on a hidden layer", !M::refusal(t.d, M::How::Down, "L4").empty(), M::refusal(t.d, M::How::Down, "L4"));
        check("merge: refused -- no step", t.j.size() == 1, std::to_string(t.j.size()));
    }
    // ── Merge Visible: hidden ones stay ──────────────────────────────────
    {
        Doc t;
        {
            core::Document::Loading load(t.d);
            auto top = *t.d.get("L5");
            top.pixels = std::make_shared<const P::TileMap>(t.d.blank().fill({0, 0, 50, 50}, P::rgba(0, 0, 1, 1)));
            t.d.put("L5", top);
        }
        L::set_visible(t.j, t.d, "L5", false);
        const auto a = seen(t.d, 150, 120), b = seen(t.d, 10, 10);
        const auto id = M::run(t.j, t.d, M::How::Visible, "L4");
        const auto m = t.d.get(id);
        check("merge: Merge Visible makes one layer “Merged”", m && m->name == "Merged" && m->kind == core::Kind::Pixel, m ? m->name : id);
        check("merge: Merge Visible -- the hidden layer stays", t.d.get("L5") && !t.d.get("L5")->visible, "");
        check("merge: Merge Visible -- the shown ones and their group go", !t.d.get("L1") && !t.d.get("L2") && !t.d.get("L3") && !t.d.get("L4"), "");
        check("merge: Merge Visible -- two layers left", t.d.count() == 2, std::to_string(t.d.count()));
        check("merge: Merge Visible -- looks the same", same_look(a, seen(t.d, 150, 120)) && same_look(b, seen(t.d, 10, 10)),
              px_s(seen(t.d, 150, 120)) + " / " + px_s(seen(t.d, 10, 10)));
        check("merge: Merge Visible -- the tree holds", t.d.check().empty(), t.d.check());
        t.j.undo();
        check("merge: Merge Visible undone -- five layers again", t.d.count() == 5 && t.d.check().empty(), std::to_string(t.d.count()));
        t.j.redo();
        check("merge: Merge Visible redone", t.d.count() == 2 && t.d.check().empty(), std::to_string(t.d.count()));
    }
    // ── Flatten Image ─────────────────────────────────────────────────────
    {
        Doc t;
        L::set_visible(t.j, t.d, "L5", false);
        check("merge: Flatten counts the hidden it drops", M::hidden_dropped(t.d) == 1, std::to_string(M::hidden_dropped(t.d)));
        const auto a = seen(t.d, 150, 120);
        const auto id = M::run(t.j, t.d, M::How::Flatten, "L4");
        const auto bg = t.d.get(id);
        check("merge: Flatten leaves one layer “Background”", t.d.count() == 1 && bg && bg->name == "Background", std::to_string(t.d.count()));
        check("merge: Flatten -- looks the same", same_look(a, seen(t.d, 150, 120)), px_s(seen(t.d, 150, 120)));
        check("merge: Flatten -- one step", t.j.undo_label() == "Flatten Image", t.j.undo_label());
        t.j.undo();
        check("merge: Flatten undone", t.d.count() == 5 && t.d.check().empty(), std::to_string(t.d.count()));
    }
    // ── Stamp Visible ─────────────────────────────────────────────────────
    {
        Doc t;
        const auto snap = t.d.snapshot();
        const auto id = M::run(t.j, t.d, M::How::Stamp, "L5");
        const auto s = t.d.get(id);
        check("merge: Stamp Visible adds one layer", t.d.count() == 6 && s && s->pixels && near(s->pixels->get(150, 120).c[0], 1), std::to_string(t.d.count()));
        bool kept = true;
        for (const auto& [k, v] : snap) { const auto now = t.d.get(k); if (!now || now->pixels != v.pixels || now->name != v.name) kept = false; }
        check("merge: Stamp Visible -- the others untouched", kept, "");
        check("merge: Stamp Visible -- above the picked one", s && s->parent.empty() && s->pos > t.d.get("L5")->pos, "");
    }
    // ── a Gray picture merges to Gray ─────────────────────────────────────
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        {
            core::Document::Loading load(d);
            d.set_image({.width = 300, .height = 200, .format = P::Format{P::Depth::U8, P::Mode::Gray, true}});
            core::Layer a, b;
            a.name = "Low"; a.pos = 1;
            P::Px grey{}; grey.c[0] = 0.5f; grey.c[1] = 1;
            a.pixels = std::make_shared<const P::TileMap>(d.blank().fill({0, 0, 300, 200}, grey));
            b.name = "High"; b.pos = 2;
            P::Px white{}; white.c[0] = 1; white.c[1] = 1;
            b.pixels = std::make_shared<const P::TileMap>(d.blank().fill({0, 0, 100, 100}, white));
            d.put("L1", a);
            d.put("L2", b);
        }
        const auto id = M::run(j, d, M::How::Down, "L2");
        const auto r = d.get(id);
        check("merge: Gray -- Merge Down keeps the picture's format", r && r->pixels && r->pixels->format() == d.image().format, "");
        check("merge: Gray -- white over 50 % grey", r && r->pixels && near(r->pixels->get(50, 50).c[0], 1) && near(r->pixels->get(200, 150).c[0], 0.5f, 0.03f),
              r && r->pixels ? px_s(r->pixels->get(50, 50)) + " / " + px_s(r->pixels->get(200, 150)) : "none");
    }
}
