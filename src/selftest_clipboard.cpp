// klimt_selftest -- s096: THE CLIPBOARD (core/Clipboard.hpp). Copy takes
// the layer's pixels through the selection, cut to what isn't clear, a soft
// edge's coverage kept; Copy Merged what you see; Paste is a new layer,
// centred, Paste in Place where it came from -- ONE step each; the 8-bit
// RGBA other apps get comes back the same; a Gray picture takes an RGB piece
// converted, a CMYK one refuses it. Rules only.
#include "core/Clipboard.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Selection.hpp"
#include "pixel/Pixel.hpp"

#include <cmath>
#include <functional>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace C = klimt::core::clipboard;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {

struct Doc {
    core::Document d;
    core::DocJournal j{d};
    Doc() { d.set_door([this] { return j.open(); }); L::load_fixture(d); }
};
bool near(float a, float b, float tol = 0.02f) { return std::fabs(a - b) <= tol; }
std::string wh(const C::Piece& p) { return std::to_string(p.width()) + " x " + std::to_string(p.height()) + " at " + std::to_string(p.x) + ", " + std::to_string(p.y); }
bool red(const P::Px& p) { return near(p.c[0], 1) && near(p.c[1], 0) && near(p.c[3], 1); }

}  // namespace

void selftest_clipboard(const Check& check) {
    // ── Copy: through the selection, cut to what isn't clear ─────────────
    {
        Doc t;
        L::load_selection(t.d, {90, 90, 150, 300});   // over the red square's left part (100..300 x 100..250)
        const auto a = C::area(t.d);
        check("clipboard: Copy reads the selection's bounds", a == P::Rect{90, 90, 150, 300}, "");
        const auto pc = C::cut_out(*t.d.get("L4")->pixels, t.d.selection(), a);
        check("clipboard: Copy -- cut to what isn't clear", pc && pc->width() == 140 && pc->height() == 150 && pc->x == 100 && pc->y == 100, pc ? wh(*pc) : "none");
        check("clipboard: Copy -- the pixels are the layer's", pc && red(pc->pixels->get(0, 0)) && red(pc->pixels->get(139, 149)), "");
        check("clipboard: Copy changes nothing", t.j.size() == 0, "");
    }
    {
        Doc t;
        const auto pc = C::cut_out(*t.d.get("L4")->pixels, nullptr, C::area(t.d));
        check("clipboard: Copy with nothing selected -- the whole layer, trimmed", pc && pc->width() == 200 && pc->height() == 150 && pc->x == 100 && pc->y == 100, pc ? wh(*pc) : "none");
        const auto none = C::cut_out(t.d.blank(), nullptr, C::area(t.d));
        check("clipboard: Copy of a clear layer -- nothing", !none, "");
    }
    // ── a soft selection's coverage is kept ──────────────────────────────
    {
        Doc t;
        {
            core::Document::Loading load(t.d);
            core::Layer s;
            P::Px half{};
            half.c[0] = 0.5f;
            s.pixels = std::make_shared<const P::TileMap>(t.d.blank_mask().fill({0, 0, 600, 400}, half));
            t.d.put(core::kSelectionId, s);
        }
        const auto pc = C::cut_out(*t.d.get("L4")->pixels, t.d.selection(), C::area(t.d));
        check("clipboard: a 50 % selection copies at 50 %", pc && near(pc->pixels->get(10, 10).c[3], 0.5f, 0.03f),
              pc ? std::to_string(pc->pixels->get(10, 10).c[3]) : "none");
    }
    // ── Copy Merged: what you see ────────────────────────────────────────
    {
        Doc t;
        L::load_selection(t.d, {50, 50, 100, 100});
        const auto flat = core::file::flatten(t.d.image(), t.d.snapshot());
        const auto pc = flat ? C::cut_out(*flat, t.d.selection(), C::area(t.d)) : std::nullopt;
        const bool white = pc && near(pc->pixels->get(5, 5).c[0], 1) && near(pc->pixels->get(5, 5).c[1], 1);
        const bool r = pc && red(pc->pixels->get(70, 70));
        check("clipboard: Copy Merged -- the white Background and the red square both", pc && pc->width() == 100 && white && r, pc ? wh(*pc) : "none");
    }
    // ── Paste / Paste in Place ───────────────────────────────────────────
    {
        Doc t;
        const auto pc = C::cut_out(*t.d.get("L4")->pixels, nullptr, C::area(t.d));
        const auto id = C::paste(t.j, t.d, *pc, "L5", true);
        const auto l = t.d.get(id);
        check("clipboard: Paste in Place -- a new layer “Pasted”", l && l->name == "Pasted" && l->kind == core::Kind::Pixel, id);
        check("clipboard: Paste in Place -- where it came from", l && red(l->pixels->get(100, 100)) && red(l->pixels->get(299, 249)) && near(l->pixels->get(99, 99).c[3], 0), "");
        check("clipboard: Paste in Place -- one step", t.j.size() == 1 && t.j.undo_label().rfind("Paste in Place", 0) == 0, t.j.undo_label());
        check("clipboard: Paste in Place -- above the picked layer", l && l->pos > t.d.get("L5")->pos, "");
        t.j.undo();
        check("clipboard: Paste undone", !t.d.get(id) && t.d.count() == 5, "");
        const auto id2 = C::paste(t.j, t.d, *pc, "L5", false);
        const auto c = t.d.get(id2);   // 200 x 150 centred on 600 x 400: (200, 125)
        check("clipboard: Paste -- centred on the canvas", c && red(c->pixels->get(200, 125)) && red(c->pixels->get(399, 274)) && near(c->pixels->get(199, 125).c[3], 0), "");
        check("clipboard: Paste -- the tree holds", t.d.check().empty(), t.d.check());
    }
    // ── other apps: straight 8-bit RGBA, there and back ─────────────────
    {
        Doc t;
        const auto pc = C::cut_out(*t.d.get("L4")->pixels, nullptr, C::area(t.d));
        const auto bytes = C::rgba8(*pc);
        check("clipboard: RGBA for other apps -- w x h x 4, red", bytes.size() == 200u * 150u * 4u && bytes[0] == 255 && bytes[1] == 0 && bytes[3] == 255,
              std::to_string(bytes.size()));
        const auto back = C::from_rgba8(bytes.data(), 200, 150, 200 * 4);
        check("clipboard: RGBA from another app -- the same picture", back.pixels && back.width() == 200 && red(back.pixels->get(150, 100)), "");
    }
    // ── another format ───────────────────────────────────────────────────
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        {
            core::Document::Loading load(d);
            d.set_image({.width = 200, .height = 100, .format = P::Format{P::Depth::U16, P::Mode::Gray, true}});
            core::Layer a;
            a.name = "Grey";
            a.pos = 1;
            d.put("L1", a);
        }
        C::Piece pc;
        pc.pixels = std::make_shared<const P::TileMap>(P::TileMap(P::Format{P::Depth::U8, P::Mode::RGB, true}, 20, 10).fill({0, 0, 20, 10}, P::rgba(1, 1, 1, 1)));
        const auto id = C::paste(j, d, pc, "L1", false);
        const auto l = d.get(id);
        check("clipboard: an RGB piece into a 16-bit Gray picture -- converted", l && l->pixels && l->pixels->format() == d.image().format && near(l->pixels->get(100, 50).c[0], 1), "");
        core::Document k;
        {
            core::Document::Loading load(k);
            k.set_image({.width = 50, .height = 50, .format = P::Format{P::Depth::U8, P::Mode::CMYK, true}});
            core::Layer a;
            a.name = "Ink";
            a.pos = 1;
            k.put("L1", a);
        }
        check("clipboard: an RGB piece into a CMYK picture -- refused, in words", !C::refusal(k, pc).empty(), C::refusal(k, pc));
    }
}
