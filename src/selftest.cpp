// klimt_selftest -- the headless core harness.
//
// A seed is only trustworthy if its paradigms are EXERCISED, not just described
// (CANON: "a claim in the doc isn't a fact until a consumer exercises it"). This
// runs the GTK-free core with no display, so a future you can trust the pump and
// the mapping before wiring any widget on top:
//   * Recents  -- move-to-front + trim + the JSON round-trip with stale-prune;
//   * TextMap  -- forward(selection) -> inverse(map_range) round-trips byte-exact
//                 across single-line / multi-line / cross-paragraph / UTF-8, and
//                 chrome lines are never landed on.
//
//   cmake --build build --target klimt_selftest && ./build/klimt_selftest

#include "core/Commands.hpp"
#include "core/Composite.hpp"
#include "pixel/Blend.hpp"
#include "pixel/Pixel.hpp"
#include "core/Document.hpp"
#include "core/Jobs.hpp"
#include "core/Recents.hpp"
#include "core/Shortcuts.hpp"
#include "core/Tools.hpp"
#include "core/Stones.hpp"
#include "core/Undo.hpp"
#include "core/TextMap.hpp"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace core = klimt::core;

namespace {
int g_pass = 0, g_fail = 0;
void check(const std::string& name, bool ok, const std::string& detail = "") {
    if (ok) { ++g_pass; std::cout << "  PASS  " << name; }
    else    { ++g_fail; std::cout << "  FAIL  " << name; }
    if (!detail.empty()) std::cout << "  (" << detail << ")";
    std::cout << "\n";
}

// The FORWARD mapping, mirroring what a viewer's selection() would do: a
// (al,ac)-(cl,cc) mark -> V byte endpoints -> codepoint range + quote. Returns
// the byte endpoints too, so the round-trip through map_range can be exact.
struct Fwd { int cp_start, cp_end, bstart, bend; std::string quote, V; };
Fwd forward(const std::vector<core::FlatLine>& flat, int al, int ac, int cl, int cc) {
    std::vector<int> off;
    std::string V = core::visible_text(flat, off);
    int bstart = off[static_cast<std::size_t>(al)] +
                 std::min(ac, static_cast<int>(flat[static_cast<std::size_t>(al)].text.size()));
    int bend   = off[static_cast<std::size_t>(cl)] +
                 std::min(cc, static_cast<int>(flat[static_cast<std::size_t>(cl)].text.size()));
    std::string quote = V.substr(static_cast<std::size_t>(bstart),
                                 static_cast<std::size_t>(bend - bstart));
    int cps = core::utf8_length(V.substr(0, static_cast<std::size_t>(bstart)));
    int cpe = cps + core::utf8_length(quote);
    return {cps, cpe, bstart, bend, quote, V};
}

// Assert map_range inverts forward(): endpoints mapped back through byte_off
// reproduce the forward byte endpoints and reconstruct the same quote.
void roundtrip(const std::vector<core::FlatLine>& flat,
               int al, int ac, int cl, int cc, const std::string& what) {
    Fwd f = forward(flat, al, ac, cl, cc);
    std::vector<int> off;
    (void)core::visible_text(flat, off);
    core::LineCol s, e;
    const bool ok = core::map_range(flat, f.cp_start, f.cp_end, s, e);
    check("textmap: " + what + " maps", ok);
    if (!ok) return;
    const int rb0 = off[static_cast<std::size_t>(s.line)] + s.col;
    const int rb1 = off[static_cast<std::size_t>(e.line)] + e.col;
    check("textmap: " + what + " round-trips byte-exact",
          rb0 == f.bstart && rb1 == f.bend,
          std::to_string(rb0) + "," + std::to_string(rb1) +
          " vs " + std::to_string(f.bstart) + "," + std::to_string(f.bend));
    check("textmap: " + what + " reconstructs quote",
          f.V.substr(static_cast<std::size_t>(rb0),
                     static_cast<std::size_t>(rb1 - rb0)) == f.quote);
}
}  // namespace

// s007: the formats checks live in their own file (selftest_formats.cpp).
void selftest_formats(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_files(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_dirty(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_select(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_paint(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_move(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_canvas(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_pen(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_bounds(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_transform(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_modify(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_masks(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_gradient(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_adjust(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_filter(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_retouch(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_brush(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_dynamics(const std::function<void(const std::string&, bool, const std::string&)>& check);   // s028
void selftest_tooldefaults(const std::function<void(const std::string&, bool, const std::string&)>& check);   // s029
void selftest_filterlayers(const std::function<void(const std::string&, bool, const std::string&)>& check);   // s030
void selftest_sharpen(const std::function<void(const std::string&, bool, const std::string&)>& check);   // s034
void selftest_pixelate(const std::function<void(const std::string&, bool, const std::string&)>& check);   // s043
void selftest_paths(const std::function<void(const std::string&, bool, const std::string&)>& check);     // s035
void selftest_isolate(const std::function<void(const std::string&, bool, const std::string&)>& check);   // s035 m7
void selftest_rulers(const std::function<void(const std::string&, bool, const std::string&)>& check);   // s031
void selftest_styles(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_fade(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_partly(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_presets(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_transfer(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_session(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_groups(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_range(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_channels(const std::function<void(const std::string&, bool, const std::string&)>& check);
void selftest_tabs(const std::function<void(const std::string&, bool, const std::string&)>& check);

int main() {
    std::cout << "klimt_selftest -- undo, layers, jobs, recents, textmap, shortcuts\n";

    // ── Undo -- the one door, two pictures, gestures, merging (s046) ──────────
    {
        std::cout << "\n-- undo --\n";
        using core::Stones;
        using core::Stone;
        using Clock = core::StoneJournal::Clock;
        using SnapMap = std::vector<std::pair<std::string, Stone>>;
        const auto snap = [](const Stones& s) {
            SnapMap out;
            for (const auto& id : s.ids()) out.emplace_back(id, *s.get(id));
            return out;
        };
        const auto names = [](const Stones& s) {
            std::string out;
            for (const auto& id : s.ids()) out += s.get(id)->name + ",";
            return out;
        };
        // A fixture: three stones, LOADED (not a user choice -- not undoable).
        struct Fix {
            Stones s;
            core::StoneJournal j{s};
            std::vector<std::string> sel;
            Fix() {
                s.set_door([this] { return j.open(); });
                j.set_selection_source([this] { return sel; });
                Stones::Loading load(s);
                for (const char* n : {"Ruby", "Jade", "Onyx"}) {
                    const auto id = s.fresh_id();
                    s.put(id, Stone{n, 3, s.pos_after({})});
                }
            }
        };

        // THE GATE. Every verb the page offers, from a fresh fixture: nothing
        // written with the door shut; at least one step; undoing them all puts
        // the model back exactly; redoing them all reproduces the result.
        for (const auto& v : core::stones::verbs()) {
            Fix f;
            const auto first = f.s.ids().front();
            const auto before = snap(f.s);
            v.run(f.j, f.s, first);
            const auto after = snap(f.s);
            const auto steps = f.j.size();
            check("gate/" + v.name + ": through the one door", f.s.strays() == 0,
                  std::to_string(f.s.strays()) + " stray write(s)");
            check("gate/" + v.name + ": is a step", steps > 0 && !(after == before));
            for (std::size_t i = 0; i < steps; ++i) f.j.undo();
            check("gate/" + v.name + ": undo puts it back exactly", snap(f.s) == before);
            for (std::size_t i = 0; i < steps; ++i) f.j.redo();
            check("gate/" + v.name + ": redo reproduces it", snap(f.s) == after);
        }
        {   // ...and the gate bites: a write around the door is counted.
            Fix f;
            const auto id = f.s.ids().front();
            f.s.put(id, Stone{"sneaky", 3, 0.5});
            check("gate: a write around the door is a stray", f.s.strays() == 1);
        }

        {   // Delete comes back whole, in its place, under its id, selected.
            Fix f;
            const auto ids = f.s.ids();
            f.sel = {ids[1]};
            core::stones::remove(f.j, f.s, ids[1]);
            f.sel = {ids[2]};      // the UI selects the neighbour...
            f.j.reselect(f.sel);   // ...and tells the step
            check("delete: gone", names(f.s) == "Ruby,Onyx,", names(f.s));
            check("delete: the label names it", f.j.undo_label() == "Delete “Jade”", f.j.undo_label());
            const auto r = f.j.undo();
            check("delete: undo -- back in its place", names(f.s) == "Ruby,Jade,Onyx,", names(f.s));
            check("delete: undo -- the same id", f.s.ids()[1] == ids[1]);
            check("delete: undo hands back its selection", r.selection == std::vector<std::string>{ids[1]});
            const auto r2 = f.j.redo();
            check("delete: redo -- gone again, the neighbour selected",
                  names(f.s) == "Ruby,Onyx," && r2.selection == std::vector<std::string>{ids[2]});
        }

        {   // Created records: undo removes, redo brings back the same id.
            Fix f;
            const auto first = f.s.ids()[0];
            const auto made = core::stones::add(f.j, f.s, "Opal", first);
            check("add: just after the one given", names(f.s) == "Ruby,Opal,Jade,Onyx,", names(f.s));
            f.j.undo();
            check("add: undone", !f.s.get(made));
            f.j.redo();
            check("add: redone, same id, same place", f.s.get(made) && names(f.s) == "Ruby,Opal,Jade,Onyx,");
        }

        {   // A no-op is not a step.
            Fix f;
            const auto id = f.s.ids()[0];
            core::stones::rename(f.j, f.s, id, "Ruby");
            core::stones::remove(f.j, f.s, "nope");
            core::stones::move_up(f.j, f.s, id);   // already first
            check("no-op: nothing kept", f.j.size() == 0 && !f.j.can_undo());
        }

        {   // Merging: arrows on the slider fold into one step inside the window.
            Fix f;
            const auto id = f.s.ids()[0];
            const auto t0 = Clock::now();
            core::stones::resize(f.j, f.s, id, 4);
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 5; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::milliseconds(400));
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 6; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::milliseconds(800));
            check("merge: three presses, one step", f.j.size() == 1, std::to_string(f.j.size()));
            f.j.run("Size", {id}, [&] { auto v = *f.s.get(id); v.size = 7; f.s.put(id, v); }, "size:" + id,
                    t0 + std::chrono::seconds(10));
            check("merge: after the window, a new step", f.j.size() == 2);
            f.j.undo();
            f.j.undo();
            check("merge: two undos, back to the start", f.s.get(id)->size == 3);
            // Another stone's key never merges into this one's step.
            Fix g;
            const auto a = g.s.ids()[0], b = g.s.ids()[1];
            core::stones::resize(g.j, g.s, a, 5);
            core::stones::resize(g.j, g.s, b, 5);
            check("merge: a different key is a different step", g.j.size() == 2);
            // Nor into a step that was undone.
            core::stones::resize(g.j, g.s, b, 6);
            g.j.undo();
            core::stones::resize(g.j, g.s, b, 7);
            check("merge: never into an undone step", g.j.size() == 2 && g.j.can_undo() && !g.j.can_redo());
            // Back to where it started inside the window: the step goes away.
            Fix h;
            const auto c = h.s.ids()[0];
            core::stones::resize(h.j, h.s, c, 4);
            core::stones::resize(h.j, h.s, c, 3);
            check("merge: there and back again is no step", h.j.size() == 0);
        }

        {   // A gesture is one step, however many writes the drag made.
            Fix f;
            const auto id = f.s.ids()[0];
            core::stones::drag_begin(f.j, f.s, id);
            for (int n = 4; n <= 9; ++n) core::stones::drag_to(f.j, f.s, id, n);
            check("gesture: open while dragging", f.j.in_gesture() && f.j.size() == 0);
            core::stones::drag_end(f.j);
            check("gesture: a six-write drag is one step", f.j.size() == 1 && f.s.get(id)->size == 9);
            f.j.undo();
            check("gesture: one undo, back to the start", f.s.get(id)->size == 3);
            // Escape mid-drag: back, and nothing kept.
            core::stones::drag_begin(f.j, f.s, id);
            core::stones::drag_to(f.j, f.s, id, 7);
            f.j.cancel();
            check("gesture: cancel puts it back and keeps nothing",
                  f.s.get(id)->size == 3 && !f.j.in_gesture() && f.j.can_redo());
            // Ctrl+Z mid-drag: the drag becomes a step, then is undone.
            core::stones::drag_begin(f.j, f.s, id);
            core::stones::drag_to(f.j, f.s, id, 2);
            f.j.undo();
            check("gesture: undo mid-drag closes the drag, then undoes it",
                  !f.j.in_gesture() && f.s.get(id)->size == 3 && f.j.can_redo());
            check("gesture: no stray writes", f.s.strays() == 0);
        }

        {   // A new step forgets the redo branch; Move keeps both stones in one step.
            Fix f;
            const auto ids = f.s.ids();
            core::stones::move_down(f.j, f.s, ids[0]);
            check("move: one step for both stones", f.j.size() == 1 && names(f.s) == "Jade,Ruby,Onyx,", names(f.s));
            f.j.undo();
            check("move: undone", names(f.s) == "Ruby,Jade,Onyx,");
            core::stones::rename(f.j, f.s, ids[2], "Coal");
            check("redo branch: a new step forgets it", !f.j.can_redo() && f.j.size() == 1);
        }

        {   // The cap: the oldest step falls off; the newest still undoes.
            Fix f;
            const auto id = f.s.ids()[0];
            for (std::size_t i = 0; i < core::StoneJournal::kMax + 20; ++i)
                core::stones::rename(f.j, f.s, id, "n" + std::to_string(i));
            check("cap: kMax steps kept", f.j.size() == core::StoneJournal::kMax);
            f.j.undo();
            check("cap: the newest still undoes", f.s.get(id)->name == "n" + std::to_string(core::StoneJournal::kMax + 18));
        }

        {   // on_change: the UI hears every journal change (labels, greying).
            Fix f;
            int heard = 0;
            f.j.set_on_change([&] { ++heard; });
            core::stones::rename(f.j, f.s, f.s.ids()[0], "X");
            f.j.undo();
            f.j.redo();
            f.j.clear();
            check("on_change: step, undo, redo, clear -- four", heard == 4, std::to_string(heard));
        }
    }


    // ── The document: the layer tree (s002) ───────────────────────────────────
    {
        std::cout << "\n-- layers --\n";
        using core::Document;
        using core::Layer;
        namespace L = core::layers;
        using SnapMap = std::vector<std::pair<std::string, Layer>>;
        const auto snap = [](const Document& d) {
            SnapMap out;
            for (const auto& id : d.walk()) out.emplace_back(id, *d.get(id));
            // s008: the selection is a record too (apart from the tree).
            out.emplace_back(core::kSelectionId, d.get(core::kSelectionId).value_or(Layer{}));
            // s009: and so is the image (Crop, Canvas / Image Size).
            out.emplace_back(core::kImageId, *d.get(core::kImageId));
            // s016: and the saved selections.
            for (const auto& [id, c] : d.channels()) out.emplace_back(id, c);
            return out;
        };
        // The panel, flat: "Top,Group[A,B],Background" -- a group's contents in [].
        const auto tree = [](const Document& d) {
            std::function<std::string(const std::string&)> go = [&](const std::string& p) {
                std::string out;
                for (const auto& id : d.panel(p)) {
                    if (!out.empty()) out += ",";
                    out += d.get(id)->name;
                    if (d.is_group(id)) out += "[" + go(id) + "]";
                }
                return out;
            };
            return go({});
        };
        struct Fix {
            Document d;
            core::DocJournal j{d};
            std::vector<std::string> sel;
            Fix() {
                d.set_door([this] { return j.open(); });
                j.set_selection_source([this] { return sel; });
                L::load_fixture(d);
            }
        };

        {
            Fix f;
            check("layers: the fixture", tree(f.d) == "Top,Group[A,B],Background", tree(f.d));
            check("layers: the fixture holds its invariants", f.d.check().empty(), f.d.check());
            check("layers: fresh ids start after the loaded ones", f.d.fresh_id() == "L6");
        }

        // THE GATE, for the document: every verb the panel will offer, from a
        // fresh fixture -- through the door, a step, undo exact, redo exact, and
        // the tree's invariants after each of the three.
        for (const auto& v : L::verbs()) {
            Fix f;
            if (v.setup) v.setup(f.d);
            const auto before = snap(f.d);
            v.run(f.j, f.d);
            const auto after = snap(f.d);
            const auto steps = f.j.size();
            check("gate/" + v.name + ": through the one door", f.d.strays() == 0,
                  std::to_string(f.d.strays()) + " stray write(s)");
            check("gate/" + v.name + ": is ONE step", steps == 1 && !(after == before),
                  std::to_string(steps) + " step(s)");
            check("gate/" + v.name + ": the tree holds", f.d.check().empty(), f.d.check());
            for (std::size_t i = 0; i < steps; ++i) f.j.undo();
            check("gate/" + v.name + ": undo puts it back exactly", snap(f.d) == before);
            check("gate/" + v.name + ": the tree holds after undo", f.d.check().empty(), f.d.check());
            for (std::size_t i = 0; i < steps; ++i) f.j.redo();
            check("gate/" + v.name + ": redo reproduces it", snap(f.d) == after);
        }
        {
            Fix f;
            f.d.put("L1", Layer{});
            check("layers gate: a write around the door is a stray", f.d.strays() == 1);
        }

        {   // New layer / group: above the selection; inside a selected group.
            Fix f;
            L::add_layer(f.j, f.d, "N", "L1");
            check("add: above the layer given", tree(f.d) == "Top,Group[A,B],N,Background", tree(f.d));
            L::add_layer(f.j, f.d, "In", "L2");
            check("add: a group given -- inside it, at the top", tree(f.d) == "Top,Group[In,A,B],N,Background", tree(f.d));
            const auto g = L::add_group(f.j, f.d, "Set");
            check("add: nothing given -- top of the document", tree(f.d) == "Set[],Top,Group[In,A,B],N,Background", tree(f.d));
            check("add: a group starts Pass Through", f.d.get(g)->blend == core::Blend::PassThrough);
            check("add: the step names it", f.j.undo_label() == "New Group “Set”", f.j.undo_label());
        }

        {   // Group (Ctrl+G) and ungroup.
            Fix f;
            const auto g = L::group(f.j, f.d, {"L1", "L5"}, "Pair");
            check("group: where the topmost was, in panel order",
                  tree(f.d) == "Pair[Top,Background],Group[A,B]", tree(f.d));
            check("group: ONE step", f.j.size() == 1);
            L::ungroup(f.j, f.d, g);
            check("ungroup: the contents take its place", tree(f.d) == "Top,Background,Group[A,B]", tree(f.d));
            f.j.undo();
            f.j.undo();
            check("group + ungroup: two undos, the fixture again", tree(f.d) == "Top,Group[A,B],Background", tree(f.d));
            // A layer and its own group selected together: the group carries it.
            Fix h;
            L::group(h.j, h.d, {"L4", "L2"}, "Outer");
            check("group: a selected layer rides with its selected group",
                  tree(h.d) == "Top,Outer[Group[A,B]],Background", tree(h.d));
            // Grouping across groups pulls them to the first one's place.
            Fix k;
            L::group(k.j, k.d, {"L3", "L5"}, "Mix");
            check("group: across groups", tree(k.d) == "Mix[Top,B],Group[A],Background", tree(k.d));
        }

        {   // Delete: a group takes its contents; undo brings all of it back whole.
            Fix f;
            f.sel = {"L2"};
            L::remove(f.j, f.d, {"L2"});
            check("delete: the group and its contents", tree(f.d) == "Top,Background" && f.d.count() == 2, tree(f.d));
            const auto r = f.j.undo();
            check("delete: undo -- back in place, same ids",
                  tree(f.d) == "Top,Group[A,B],Background" && f.d.get("L3") && f.d.get("L4"), tree(f.d));
            check("delete: undo hands back its selection", r.selection == std::vector<std::string>{"L2"});
            check("delete: the label names it", f.j.redo_label() == "Delete “Group”", f.j.redo_label());
            check("delete: refused if it would empty the document",
                  !L::remove(f.j, f.d, {"L1", "L2", "L5"}) && f.d.count() == 5);
            check("delete: three selected, the label counts them",
                  L::remove(f.j, f.d, {"L5", "L4"}) && f.j.undo_label() == "Delete 2 Layers", f.j.undo_label());
        }

        {   // Duplicate: just above each original; a group with fresh ids throughout.
            Fix f;
            const auto made = L::duplicate(f.j, f.d, {"L2"});
            check("duplicate: just above, contents too",
                  tree(f.d) == "Top,Group copy[A,B],Group[A,B],Background", tree(f.d));
            const auto inside = f.d.panel(made.at(0));
            check("duplicate: fresh ids inside", inside.size() == 2 && inside[0] != "L4" && inside[1] != "L3");
            check("duplicate: the original untouched", f.d.panel("L2") == std::vector<std::string>({"L4", "L3"}));
        }

        {   // THE DROP -- the model under the Layers panel's drag and drop.
            Fix f;
            L::move(f.j, f.d, {"L1"}, "L4", L::Where::Above);
            check("drop: above a row inside a group", tree(f.d) == "Top,Group[Background,A,B]", tree(f.d));
            f.j.undo();
            L::move(f.j, f.d, {"L5"}, "L3", L::Where::Below);
            check("drop: below a row", tree(f.d) == "Group[A,B,Top],Background", tree(f.d));
            f.j.undo();
            L::move(f.j, f.d, {"L1"}, "L2", L::Where::Inside);
            check("drop: inside a group lands at its top", tree(f.d) == "Top,Group[Background,A,B]", tree(f.d));
            f.j.undo();
            L::move(f.j, f.d, {"L3"}, "L1", L::Where::Below);
            check("drop: out of a group, to the bottom", tree(f.d) == "Top,Group[A],Background,B", tree(f.d));
            f.j.undo();
            // Multi-select: contiguous, in panel order, whatever order they were picked.
            L::move(f.j, f.d, {"L1", "L5"}, "L4", L::Where::Below);
            check("drop: a multi-selection keeps its panel order", tree(f.d) == "Group[A,Top,Background,B]", tree(f.d));
            check("drop: one step for all of it", f.j.size() == 1 && f.j.undo_label() == "Move 2 Layers", f.j.undo_label());
            f.j.undo();
            check("drop: undo -- the fixture again", tree(f.d) == "Top,Group[A,B],Background", tree(f.d));
            // Refusals keep nothing.
            const auto n = f.j.size();
            const bool r1 = L::move(f.j, f.d, {"L2"}, "L4", L::Where::Above);   // into its own contents
            const bool r2 = L::move(f.j, f.d, {"L2"}, "L2", L::Where::Inside);  // into itself
            const bool r3 = L::move(f.j, f.d, {"L1"}, "L5", L::Where::Inside);  // inside a layer
            const bool r4 = L::move(f.j, f.d, {"L5"}, "L2", L::Where::Above);   // already there
            const bool r5 = L::move(f.j, f.d, {"L5", "L1"}, "L1", L::Where::Above); // onto itself
            check("drop: can_move agrees -- refusals and no-ops say no, writing nothing",
                  !L::can_move(f.d, {"L2"}, "L4", L::Where::Above) &&
                  !L::can_move(f.d, {"L2"}, "L2", L::Where::Inside) &&
                  !L::can_move(f.d, {"L1"}, "L5", L::Where::Inside) &&
                  !L::can_move(f.d, {"L5"}, "L2", L::Where::Above) &&
                  L::can_move(f.d, {"L1"}, "L2", L::Where::Inside) &&
                  L::can_move(f.d, {"L5", "L1"}, "L3", L::Where::Below) &&
                  f.j.size() == n && f.d.strays() == 0);
            check("drop: refusals and no-ops keep no step",
                  !r1 && !r2 && !r3 && !r4 && !r5 && f.j.size() == n && tree(f.d) == "Top,Group[A,B],Background");
            // Nested groups: drop a group into a group, then the outer into the
            // document's top level -- invariants all the way.
            const auto g = L::add_group(f.j, f.d, "Outer", "L1");
            L::move(f.j, f.d, {"L2"}, g, L::Where::Inside);
            check("drop: a group into a group", tree(f.d) == "Top,Outer[Group[A,B]],Background", tree(f.d));
            check("drop: depth follows", f.d.depth("L4") == 2 && f.d.check().empty(), f.d.check());
        }

        {   // Raise / lower step among siblings, never out of the group.
            Fix f;
            check("raise: already top of its group -- refused", !L::raise(f.j, f.d, "L4"));
            L::lower(f.j, f.d, "L4");
            check("lower: one place", tree(f.d) == "Top,Group[B,A],Background", tree(f.d));
            check("lower: the label", f.j.undo_label() == "Send Backward “A”", f.j.undo_label());
        }

        {   // Attributes: locks, visibility up the tree, blend, clipping.
            Fix f;
            L::set_visible(f.j, f.d, "L2", false);
            check("visible: a hidden group hides its contents", !f.d.shown("L4") && f.d.get("L4")->visible);
            check("blend: Pass Through refused on a layer", !L::set_blend(f.j, f.d, "L1", core::Blend::PassThrough));
            check("blend: the full set, keys round-trip", [] {
                for (const auto& b : core::blend_modes())
                    if (core::blend_from_key(b.key) != b.mode) return false;
                return core::blend_modes().size() == 28;
            }());
            L::set_locks(f.j, f.d, "L2", core::lock::All);
            const auto n = f.j.size();
            const bool o = L::set_opacity(f.j, f.d, "L4", 50);
            const bool b = L::set_blend(f.j, f.d, "L4", core::Blend::Screen);
            const bool c = L::set_clipped(f.j, f.d, "L3", true);
            L::opacity_begin(f.j, f.d, "L3");
            L::opacity_to(f.j, f.d, "L3", 10);
            L::opacity_end(f.j);
            check("lock: a locked group refuses opacity, blend, clip, drag inside it",
                  !o && !b && !c && f.j.size() == n && f.d.get("L3")->opacity == 100 && f.d.strays() == 0);
            check("lock: rename still allowed", L::rename(f.j, f.d, "L4", "Sky"));
            L::set_clipped(f.j, f.d, "L5", true);
            check("clip: the base is the unclipped layer below", f.d.clip_base("L5") == "L2");
            check("clip: an unclipped layer has no base", f.d.clip_base("L1").empty());
        }

        {   // Opacity: keys fold into one step; a drag is one step; clamped.
            Fix f;
            L::set_opacity(f.j, f.d, "L5", 90);
            L::set_opacity(f.j, f.d, "L5", 80);
            L::set_opacity(f.j, f.d, "L5", 70);
            check("opacity: presses fold into one step", f.j.size() == 1 && f.d.get("L5")->opacity == 70);
            L::set_fill(f.j, f.d, "L5", 150);
            check("fill: clamped, its own step", f.d.get("L5")->fill == 100 && f.j.size() == 1);
            L::set_fill(f.j, f.d, "L5", -5);
            check("fill: clamped low", f.d.get("L5")->fill == 0 && f.j.size() == 2);
            L::opacity_begin(f.j, f.d, "L1");
            for (int p = 95; p >= 20; p -= 5) L::opacity_to(f.j, f.d, "L1", p);
            L::opacity_end(f.j);
            check("opacity: a sixteen-write drag is one step", f.j.size() == 3 && f.d.get("L1")->opacity == 20);
            f.j.undo();
            check("opacity: one undo, back", f.d.get("L1")->opacity == 100 && f.d.strays() == 0);
        }

        {   // Sort by Name: the natural order, planned on a copy, landed by arrange.
            check("natural: Layer 2 before Layer 10", L::natural_less("Layer 2", "Layer 10") &&
                  !L::natural_less("Layer 10", "Layer 2"));
            check("natural: case-insensitive, prefix first",
                  L::natural_less("apple", "Banana") && L::natural_less("Sky", "sky 2") &&
                  L::natural_less("x007", "x8"));
            const auto order = L::sorted_by_name({{"a", "Layer 10"}, {"b", "layer 2"}, {"c", "Background"}, {"d", "Layer 1"}});
            check("sorted_by_name: A-Z, numbers as numbers",
                  order == std::vector<std::string>({"c", "d", "b", "a"}));
            Fix f;
            check("arrange: lands an order of exactly the contents",
                  L::arrange(f.j, f.d, "L2", {"L3", "L4"}, "Sort “Group” by Name") &&
                  tree(f.d) == "Top,Group[B,A],Background" && f.j.undo_label() == "Sort “Group” by Name", tree(f.d));
            const auto n = f.j.size();
            check("arrange: refused when the group changed (a layer more, or less)",
                  !L::arrange(f.j, f.d, "L2", {"L3"}, "x") && !L::arrange(f.j, f.d, "L2", {"L3", "L4", "L1"}, "x") &&
                  !L::arrange(f.j, f.d, "L4", {}, "x") && f.j.size() == n);
            check("arrange: the top level too", L::arrange(f.j, f.d, "", {"L1", "L2", "L5"}, "Sort") &&
                  tree(f.d) == "Background,Group[B,A],Top", tree(f.d));
        }

        {   // Many at once (s005): one step for the whole selection.
            Fix f;
            using V = std::vector<std::string>;
            check("many: opacity on three layers is ONE step",
                  L::set_opacity(f.j, f.d, V{"L5", "L4", "L1"}, 30) && f.j.size() == 1 &&
                  f.d.get("L5")->opacity == 30 && f.d.get("L4")->opacity == 30 && f.d.get("L1")->opacity == 30);
            check("many: the label counts them", f.j.undo_label() == "Opacity of 3 Layers", f.j.undo_label());
            L::set_opacity(f.j, f.d, V{"L5", "L4", "L1"}, 20);
            check("many: presses on the same selection merge", f.j.size() == 1 && f.d.get("L4")->opacity == 20);
            L::set_opacity(f.j, f.d, V{"L5", "L4"}, 10);
            check("many: a different selection is a new step", f.j.size() == 2);
            f.j.undo();
            f.j.undo();
            check("many: two undos, all back", f.d.get("L5")->opacity == 100 && f.d.get("L1")->opacity == 100);
            L::set_locks(f.j, f.d, "L2", core::lock::All);
            const auto n = f.j.size();
            check("many: layers under an All lock are skipped",
                  L::set_fill(f.j, f.d, V{"L4", "L5"}, 50) && f.d.get("L4")->fill == 100 && f.d.get("L5")->fill == 50 &&
                  f.j.size() == n + 1 && f.j.undo_label() == "Fill of “Top”", f.j.undo_label());
            check("many: refused when every one is locked",
                  !L::set_fill(f.j, f.d, V{"L3", "L4"}, 50) && f.j.size() == n + 1);
            check("many: Pass Through only reaches the groups",
                  L::set_blend(f.j, f.d, V{"L5", "L1"}, core::Blend::Multiply) &&
                  !L::set_blend(f.j, f.d, V{"L5", "L1"}, core::Blend::PassThrough));
            Fix g;
            L::opacity_begin(g.j, g.d, V{"L5", "L1"});
            for (int p = 90; p >= 40; p -= 10) L::opacity_to(g.j, g.d, V{"L5", "L1"}, p);
            L::opacity_end(g.j);
            check("many: a drag over two layers is ONE step",
                  g.j.size() == 1 && g.d.get("L5")->opacity == 40 && g.d.get("L1")->opacity == 40 && g.d.strays() == 0);
        }

        {   // History's view of the journal (s005): position and labels.
            Fix f;
            L::rename(f.j, f.d, "L5", "One");
            L::rename(f.j, f.d, "L5", "Two");
            L::rename(f.j, f.d, "L5", "Three");
            f.j.undo();
            check("history: position after an undo", f.j.position() == 2 && f.j.size() == 3);
            check("history: labels by index", f.j.label(0) == "Rename “Top”" && f.j.label(2) == "Rename “Two”",
                  f.j.label(0) + " / " + f.j.label(2));
            while (f.j.position() > 0) f.j.undo();
            check("history: back to the start", f.d.get("L5")->name == "Top" && f.j.can_redo());
            while (f.j.position() < 2) f.j.redo();
            check("history: forward to step 2", f.d.get("L5")->name == "Two");
        }

        {   // Pixels in the document (s006): every write a step, by tile pointer.
            namespace P = klimt::pixel;
            Fix f;
            check("pixels: the fixture's image and pixels hold the invariants",
                  f.d.image().width == 600 && f.d.check().empty(), f.d.check());
            const auto before = f.d.get("L4")->pixels;
            check("pixels: fill is a step", L::fill(f.j, f.d, "L4", {0, 0, 50, 50}, P::rgba(0, 0, 1, 1)) &&
                  f.j.size() == 1 && f.j.undo_label() == "Fill “A”", f.j.undo_label());
            const auto after = f.d.get("L4")->pixels;
            check("pixels: the new map shares the tiles the fill didn't touch",
                  after != before && after->same_tile(*before, 1, 0) && !after->same_tile(*before, 0, 0));
            f.j.undo();
            check("pixels: undo puts back the very same map (a pointer, not a copy)",
                  f.d.get("L4")->pixels == before);
            f.j.redo();
            check("pixels: redo the very same new one", f.d.get("L4")->pixels == after &&
                  f.d.get("L4")->pixels->get(10, 10).c[2] == 1.f);
            const auto n = f.j.size();
            check("pixels: refused on a group, wholly outside, or a no-op",
                  !L::fill(f.j, f.d, "L2", {0, 0, 10, 10}, P::rgba(1, 1, 1, 1)) &&
                  !L::fill(f.j, f.d, "L4", {700, 0, 10, 10}, P::rgba(1, 1, 1, 1)) &&
                  !L::fill(f.j, f.d, "L4", {0, 0, 10, 10}, P::rgba(0, 0, 1, 1)) && f.j.size() == n);
            L::set_locks(f.j, f.d, "L4", core::lock::Pixels);
            check("pixels: a Pixels lock refuses", !L::fill(f.j, f.d, "L4", {0, 0, 10, 10}, P::rgba(1, 1, 1, 1)));
            L::set_locks(f.j, f.d, "L4", core::lock::Alpha);
            check("pixels: an Alpha lock fills colour only where there is something",
                  L::fill(f.j, f.d, "L4", {0, 0, 600, 400}, P::rgba(0, 1, 0, 1)) &&
                  f.d.get("L4")->pixels->get(150, 150).c[1] == 1.f &&      // was red: now green
                  f.d.get("L4")->pixels->get(500, 350).c[3] == 0.f);       // was empty: stays empty
            check("pixels: an Alpha lock refuses a clear", !L::clear(f.j, f.d, "L4", {0, 0, 600, 400}));
            const auto dup = L::duplicate(f.j, f.d, {"L4"});
            check("pixels: duplicate shares the pixels -- free until painted",
                  dup.size() == 1 && f.d.get(dup[0])->pixels == f.d.get("L4")->pixels);
            check("pixels: the tree holds", f.d.check().empty() && f.d.strays() == 0, f.d.check());
            // A clear that empties the layer leaves no map at all.
            Fix g;
            L::clear(g.j, g.d, "L4", {0, 0, 600, 400});
            check("pixels: cleared to nothing -- no map", g.d.get("L4")->pixels == nullptr);
            // The invariant catches pixels at the wrong size.
            Fix h;
            {
                core::Document::Loading load(h.d);
                auto bad = *h.d.get("L5");
                bad.pixels = std::make_shared<const P::TileMap>(P::TileMap(h.d.image().format, 10, 10));
                h.d.put("L5", bad);
            }
            check("pixels: check() catches a map not at the image's size", !h.d.check().empty());
            // set_image outside a load is a stray (not a user choice).
            h.d.set_image(h.d.image());
            check("pixels: set_image outside a load counts as a stray", h.d.strays() == 1);
        }

        {   // Blend math (libs/pixel) and the compositor (s006).
            namespace P = klimt::pixel;
            using B = P::Blend;
            const auto one = [](B m, std::array<float, 4> src, std::array<float, 4> dst, float op = 1.f) {
                P::blend_span(m, src.data(), dst.data(), 1, op, 0, 0);
                return dst;
            };
            const auto near = [](std::array<float, 4> a, std::array<float, 4> b) {
                for (int i = 0; i < 4; ++i) if (std::abs(a[static_cast<std::size_t>(i)] - b[static_cast<std::size_t>(i)]) > 1e-4f) return false;
                return true;
            };
            const std::array<float, 4> red{1, 0, 0, 1}, white{1, 1, 1, 1}, black{0, 0, 0, 1}, clear{0, 0, 0, 0};
            const std::array<float, 4> c{0.2f, 0.5f, 0.7f, 1};
            check("blend: Normal, opaque, replaces", near(one(B::Normal, red, white), red));
            check("blend: Multiply by white is the backdrop", near(one(B::Multiply, white, c), c));
            check("blend: Screen by black is the backdrop", near(one(B::Screen, black, c), c));
            check("blend: Difference with itself is black", near(one(B::Difference, c, c), black));
            check("blend: opacity 0 / a transparent source change nothing",
                  near(one(B::Multiply, red, c, 0.f), c) && near(one(B::Screen, clear, c), c));
            check("blend: half opacity, Normal, is halfway", near(one(B::Normal, red, white, 0.5f), {1, 0.5f, 0.5f, 1}));
            const auto g = one(B::Color, {0.5f, 0.5f, 0.5f, 1}, {0.9f, 0.1f, 0.1f, 1});
            check("blend: Color with a grey source greys the backdrop", std::abs(g[0] - g[1]) < 1e-4f && std::abs(g[1] - g[2]) < 1e-4f);
            check("blend: over transparent, any mode shows the source", near(one(B::Multiply, red, clear), red));
            int kept = 0;
            for (int x = 0; x < 1000; ++x) {
                std::array<float, 4> d = white;
                P::blend_span(B::Dissolve, red.data(), d.data(), 1, 0.5f, x, 7);
                kept += d[1] < 0.5f ? 1 : 0;
            }
            check("blend: Dissolve at 50% keeps about half, all or nothing", kept > 400 && kept < 600, std::to_string(kept));

            // The compositor, on the fixture: white Background; group "Group"
            // (Pass Through) holding A (a red square 100..300 x 100..250) over
            // B (empty); Top (empty).
            Fix f;
            const auto at = [&](int x, int y) {
                std::vector<std::uint32_t> img;
                core::composite_argb(f.d.image(), f.d.snapshot(), img);
                return img[static_cast<std::size_t>(y) * 600 + static_cast<std::size_t>(x)];
            };
            check("composite: red over white, white elsewhere", at(150, 150) == 0xffff0000u && at(500, 350) == 0xffffffffu);
            L::set_visible(f.j, f.d, "L2", false);
            check("composite: a hidden group hides its contents", at(150, 150) == 0xffffffffu);
            L::set_visible(f.j, f.d, "L2", true);
            L::set_opacity(f.j, f.d, "L2", 50);
            const auto half = at(150, 150);
            check("composite: a Pass Through group at 50% -- pink", ((half >> 8) & 0xff) > 120 && ((half >> 8) & 0xff) < 135 &&
                  ((half >> 16) & 0xff) == 0xff, std::to_string(half));
            L::set_opacity(f.j, f.d, "L2", 100);
            L::set_clipped(f.j, f.d, "L4", true);   // clipped to B -- which is empty
            check("composite: clipped to an empty base shows nothing", at(150, 150) == 0xffffffffu);
            L::set_clipped(f.j, f.d, "L4", false);
            L::set_blend(f.j, f.d, "L4", core::Blend::Screen);
            check("composite: red Screen over white is white", at(150, 150) == 0xffffffffu);
            L::set_blend(f.j, f.d, "L4", core::Blend::Multiply);
            check("composite: red Multiply over white is red", at(150, 150) == 0xffff0000u);
            std::vector<std::uint32_t> img;
            int asked = 0;
            const bool done = core::composite_argb(f.d.image(), f.d.snapshot(), img, [&] { return ++asked > 1; });
            check("composite: cancelled between tiles", !done);
        }

        {   // Who takes the selection after a delete.
            Fix f;
            check("neighbour: the row below", L::neighbour(f.d, "L5") == "L2");
            check("neighbour: last in a group -- the row above", L::neighbour(f.d, "L3") == "L4");
            Fix g;
            L::remove(g.j, g.d, {"L3"});
            check("neighbour: alone in a group -- the group", L::neighbour(g.d, "L4") == "L2");
        }

        {   // Many structural steps, then all undone: exactly the fixture.
            Fix f;
            const auto start = snap(f.d);
            const auto n1 = L::add_layer(f.j, f.d, "One", "L3");
            const auto g = L::group(f.j, f.d, {n1, "L1"}, "G");
            L::duplicate(f.j, f.d, {g});
            L::move(f.j, f.d, {"L5"}, g, L::Where::Inside);
            L::ungroup(f.j, f.d, "L2");
            L::remove(f.j, f.d, {g});
            L::set_blend(f.j, f.d, "L4", core::Blend::Overlay);
            check("churn: the tree holds", f.d.check().empty(), f.d.check());
            const auto end = snap(f.d);
            const auto steps = f.j.size();
            for (std::size_t i = 0; i < steps; ++i) f.j.undo();
            check("churn: seven steps undone -- the fixture exactly", steps == 7 && snap(f.d) == start,
                  std::to_string(steps));
            for (std::size_t i = 0; i < steps; ++i) f.j.redo();
            check("churn: and redone -- the end exactly", snap(f.d) == end && f.d.check().empty());
            check("churn: no stray writes", f.d.strays() == 0);
        }
    }


    // ── Jobs: the UI never waits -- work off-thread, land as ONE step (s002) ──
    {
        std::cout << "\n-- jobs --\n";
        using core::Jobs;
        namespace L = core::layers;
        struct Fix {
            core::Document d;
            core::DocJournal j{d};
            Fix() { d.set_door([this] { return j.open(); }); L::load_fixture(d); }
        };
        const auto spin_until = [](auto pred) {
            for (int i = 0; i < 2000 && !pred(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return pred();
        };

        {   // A job computes off-thread; nothing changes until the UI pumps it;
            // then it lands as ONE step, through the door.
            Fix f;
            Jobs jobs(2);
            std::atomic<int> woken{0};
            jobs.set_wake([&] { ++woken; });
            const auto main_id = std::this_thread::get_id();
            std::thread::id worker_id;
            const auto id = jobs.submit("Auto Name", [&](Jobs::Token& t) -> Jobs::Landing {
                worker_id = std::this_thread::get_id();
                long sum = 0;
                for (int i = 1; i <= 1000; ++i) { sum += i; if (i % 100 == 0) t.progress(i / 1000.0); }
                const std::string name = "Sum " + std::to_string(sum);
                return [&f, name] { L::rename(f.j, f.d, "L5", name); };
            });
            jobs.wait_idle();
            check("jobs: the work ran on a worker", worker_id != main_id && worker_id != std::thread::id{});
            check("jobs: finished, not landed -- the document untouched",
                  jobs.state(id) == Jobs::State::Done && f.d.get("L5")->name == "Top" && f.j.size() == 0);
            check("jobs: the wake was called", spin_until([&] { return woken.load() == 1; }));
            check("jobs: progress reads 1 when done", jobs.progress(id) == 1.0);
            check("jobs: pump lands it", jobs.pump() == 1 && jobs.state(id) == Jobs::State::Landed);
            check("jobs: ONE step, through the door",
                  f.d.get("L5")->name == "Sum 500500" && f.j.size() == 1 && f.d.strays() == 0);
            check("jobs: a second pump lands nothing", jobs.pump() == 0);
            f.j.undo();
            check("jobs: undo takes the whole job back", f.d.get("L5")->name == "Top");
        }

        {   // Cancel while running: the landing is dropped -- nothing left behind.
            Fix f;
            Jobs jobs(1);
            const auto id = jobs.submit("Long", [&](Jobs::Token& t) -> Jobs::Landing {
                for (int i = 0; !t.cancelled() && i < 100000; ++i) {
                    t.progress(0.01 + (i % 100) / 1000.0);
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
                return [&f] { L::rename(f.j, f.d, "L5", "SHOULD NOT LAND"); };
            });
            check("cancel: it started and reports progress", spin_until([&] { return jobs.progress(id) > 0.0; }));
            jobs.cancel(id);
            jobs.wait_idle();
            check("cancel: nothing lands", jobs.pump() == 0 && jobs.state(id) == Jobs::State::Cancelled);
            check("cancel: the document and journal untouched", f.d.get("L5")->name == "Top" && f.j.size() == 0);
        }

        {   // Cancel while queued: it never runs. Cancel after done, before the
            // pump: still dropped.
            Jobs jobs(1);
            std::atomic<bool> release{false}, second_ran{false};
            const auto a = jobs.submit("Blocker", [&](Jobs::Token&) -> Jobs::Landing {
                while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                return [] {};
            });
            const auto b = jobs.submit("Queued", [&](Jobs::Token&) -> Jobs::Landing {
                second_ran = true;
                return [] {};
            });
            check("queue: the second waits its turn",
                  spin_until([&] { return jobs.state(a) == Jobs::State::Running; }) &&
                  jobs.state(b) == Jobs::State::Queued && jobs.active() == 2);
            jobs.cancel(b);
            release = true;
            jobs.wait_idle();
            check("queue: a cancelled job never runs", !second_ran && jobs.state(b) == Jobs::State::Cancelled);
            jobs.cancel(a);   // done, not yet landed
            check("queue: cancel after done, before the pump -- dropped", jobs.pump() == 0);
        }

        {   // A job that throws lands nothing and keeps its error.
            Jobs jobs(1);
            const auto id = jobs.submit("Broken", [](Jobs::Token&) -> Jobs::Landing {
                throw std::runtime_error("model file missing");
            });
            jobs.wait_idle();
            check("fail: no landing, the error kept",
                  jobs.pump() == 0 && jobs.state(id) == Jobs::State::Failed &&
                  jobs.error(id) == "model file missing", jobs.error(id));
        }

        {   // Many in parallel; each lands its own result, one step each.
            Fix f;
            Jobs jobs(4);
            const std::vector<std::string> ids = {"L1", "L3", "L4", "L5"};
            for (const auto& lid : ids)
                jobs.submit("Name " + lid, [&f, lid](Jobs::Token&) -> Jobs::Landing {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    return [&f, lid] { L::rename(f.j, f.d, lid, "done " + lid); };
                });
            jobs.wait_idle();
            const auto landed = jobs.pump();
            bool all = true;
            for (const auto& lid : ids) all = all && f.d.get(lid)->name == "done " + lid;
            check("parallel: four jobs, four landings, four steps",
                  landed == 4 && all && f.j.size() == 4 && f.d.strays() == 0, std::to_string(landed));
            check("parallel: the pool has its threads", jobs.threads() == 4);
        }

        {   // Destroying the pool with work in flight cancels and joins cleanly.
            std::atomic<bool> stopped{false};
            {
                Jobs jobs(2);
                const auto id = jobs.submit("Endless", [&](Jobs::Token& t) -> Jobs::Landing {
                    while (!t.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    stopped = true;
                    return [] {};
                });
                spin_until([&] { return jobs.state(id) == Jobs::State::Running; });
            }
            check("shutdown: a running job is cancelled and joined", stopped.load());
        }
    }


    // ── Tools: the registry the rail, the keys and the reference read (s005) ──
    {
        std::cout << "\n-- tools --\n";
        const auto& ts = core::tools();
        std::set<std::string> ids;
        std::set<char> keys;
        bool ordered = true, named = true;
        int group = -1;
        for (const auto& t : ts) {
            ids.insert(t.id);
            keys.insert(t.key);
            if (t.group < group) ordered = false;
            group = t.group;
            if (t.name.empty() || t.note.empty() || t.key < 'a' || t.key > 'z') named = false;
        }
        check("tools: ids unique", ids.size() == ts.size());
        check("tools: keys unique", keys.size() == ts.size());
        check("tools: groups in rail order", ordered);
        check("tools: each has a name, a note and a lower-case letter", named);
        check("tools: a key finds its tool, either case",
              core::tool_for_key('m') && core::tool_for_key('M') && core::tool_for_key('M')->id == "select" &&
              !core::tool_for_key('q'));
        check("tools: the scope wall -- no shape tool",
              !core::find_tool("shape") && !core::find_tool("rectangle") && !core::find_tool("ellipse"));
        // s035: the Pen is allowed through the wall as a PATH tool -- paths for
        // selections and masks (REQUIREMENTS) -- never as art: its note
        // speaks of the selection, and no command strokes or fills a path.
        {
            const auto* pen = core::find_tool("pen");
            bool art = false;
            for (const auto& c : core::commands())
                art = art || c.action.find("stroke-path") != std::string::npos || c.action.find("fill-path") != std::string::npos;
            check("tools: the scope wall -- the Pen makes paths for selections, never art",
                  pen && pen->note.find("selection") != std::string::npos && !art);
        }
        // Every icon is a file in resources/ AND listed in the gresource bundle.
        const std::string root = KLIMT_SOURCE_DIR;
        std::ifstream xml(root + "/resources/klimt.gresource.xml");
        std::stringstream buf;
        buf << xml.rdbuf();
        const std::string bundle = buf.str();
        std::string missing;
        for (const auto& t : ts) {
            const auto file = "icons/scalable/apps/" + t.icon + ".svg";
            if (!std::filesystem::exists(root + "/resources/" + file) || bundle.find(file) == std::string::npos)
                missing += t.icon + " ";
        }
        check("tools: every icon is in the resource bundle", missing.empty(), missing);
        // The shortcut reference carries a row per tool, from the registry.
        std::size_t rows = 0;
        for (const auto& r : core::shortcut_registry()) if (r.section == "Tools") ++rows;
        check("tools: the shortcut reference has a row per tool", rows == ts.size(), std::to_string(rows));
        std::string prev;
        bool az = true;
        for (const auto& r : core::shortcut_registry()) {
            if (r.section != prev && r.section < prev) az = false;
            prev = r.section;
        }
        check("shortcuts: sections authored A-Z", az);
    }

    // ── Commands: the palette's catalog and matcher (s005) ─────────────────────
    {
        std::cout << "\n-- commands --\n";
        const auto& cs = core::commands();
        std::set<std::string> titles;
        bool shaped = true;
        for (const auto& c : cs) {
            titles.insert(c.title);
            if (c.action.rfind("win.", 0) != 0 || c.title.empty() || c.section.empty()) shaped = false;
        }
        check("commands: titles unique", titles.size() == cs.size());
        check("commands: every one names a win.* action, a title and a section", shaped);
        std::size_t tool_rows = 0;
        for (const auto& c : cs) if (c.action == "win.tool") ++tool_rows;
        check("commands: a row per tool, from the tool registry", tool_rows == core::tools().size());
        // Keys come from the shortcut registry: Duplicate's is Ctrl+J.
        bool dup_key = false;
        for (const auto& c : cs) if (c.title == "Duplicate") dup_key = c.keys == "Ctrl+J";
        check("commands: keys come from the shortcut registry", dup_key);
        // Every wired registry action the window offers is in the catalog.
        std::string missing;
        for (const auto& r : core::shortcut_registry()) {
            if (r.action.empty() || r.action == "win.command-palette") continue;
            bool found = false;
            for (const auto& c : cs) found = found || c.action == r.action;
            if (!found) missing += r.action + " ";
        }
        check("commands: every keyed action is findable", missing.empty(), missing);
        const auto top = [](const std::string& q) {
            const auto r = core::search(q);
            return r.empty() ? std::string("(none)") : r.front()->title;
        };
        check("search: word starts win -- 'sbn' -> Sort by Name", top("sbn") == "Sort by Name", top("sbn"));
        check("search: case doesn't matter -- 'DUP' -> Duplicate", top("DUP") == "Duplicate", top("DUP"));
        check("search: keywords find it -- 'alphabetical' -> Sort by Name",
              top("alphabetical") == "Sort by Name", top("alphabetical"));
        check("search: 'brush' -> the Brush tool", top("brush") == "Brush Tool", top("brush"));
        check("search: nothing for nonsense", core::search("zzqx").empty());
        check("search: empty query lists everything", core::search("").size() == cs.size());
    }


    // ── libs/pixel: formats, immutable tiles, sparse maps (s006) ──────────────
    {
        std::cout << "\n-- pixel --\n";
        namespace P = klimt::pixel;
        const P::Format rgba8{P::Depth::U8, P::Mode::RGB, true};
        const P::Format rgba16{P::Depth::U16, P::Mode::RGB, true};
        const P::Format cmyk32{P::Depth::F32, P::Mode::CMYK, true};
        check("pixel: format sizes", rgba8.bytes_per_pixel() == 4 && rgba16.bytes_per_pixel() == 8 &&
              cmyk32.channels() == 5 && cmyk32.bytes_per_pixel() == 20 &&
              P::Format{P::Depth::U8, P::Mode::Gray, false}.bytes_per_pixel() == 1);

        // Round trips: every 8-bit value, a sweep of 16-bit values, float exact.
        bool exact8 = true;
        for (int v = 0; v < 256; ++v) {
            P::TileWriter w(rgba8, P::transparent());
            w.set(3, 4, P::rgba(v / 255.f, 0, 0, 1));
            if (std::lround(w.freeze()->get(3, 4).c[0] * 255.f) != v) exact8 = false;
        }
        check("pixel: 8-bit stores every value exactly", exact8);
        bool exact16 = true;
        for (int v = 0; v < 65536; v += 257) {
            P::TileWriter w(rgba16, P::transparent());
            w.set(0, 0, P::rgba(v / 65535.f, 0, 0, 1));
            if (std::lround(w.freeze()->get(0, 0).c[0] * 65535.f) != v) exact16 = false;
        }
        check("pixel: 16-bit stores its values exactly", exact16);
        {
            P::TileWriter w(cmyk32, P::transparent());
            P::Px p;
            p.c = {0.1f, 1.7f, -0.25f, 0.5f, 0.75f};   // float may leave 0..1
            w.set(255, 255, p);
            check("pixel: 32-bit float keeps out-of-range values", w.freeze()->get(255, 255) == p);
        }

        // Uniform tiles: one pixel's memory for 65,536.
        const auto u = P::Tile::uniform(rgba8, P::rgba(1, 0, 0, 1));
        check("pixel: a uniform tile holds one pixel", u->is_uniform() && u->bytes() == 4 &&
              u->get(200, 17) == u->get(0, 0));
        {
            P::TileWriter w(u);
            const auto same = w.freeze();
            check("pixel: an untouched writer hands back the same tile", same == u);
            P::TileWriter w2(u);
            w2.set(1, 1, P::rgba(0, 1, 0, 1));
            const auto t = w2.freeze();
            check("pixel: writing a uniform tile expands a copy; the original stands",
                  !t->is_uniform() && t->bytes() == 256u * 256u * 4u && t->get(1, 1).c[1] == 1.f &&
                  t->get(0, 0).c[0] == 1.f && u->get(1, 1).c[0] == 1.f);
        }

        // Straight storage, premultiplied for compositing, and back.
        {
            P::TileWriter w(rgba8, P::transparent());
            w.set(0, 0, P::rgba(1.f, 0.5f, 0.f, 0.5f));
            w.set(1, 0, P::rgba(0.2f, 0.4f, 0.6f, 0.f));   // invisible -- colour kept anyway
            const auto t = w.freeze();
            check("pixel: straight storage keeps colour under zero alpha",
                  std::abs(t->get(1, 0).c[2] - 0.6f) < 1.f / 255.f);
            std::vector<float> pm;
            t->premultiplied(pm);
            check("pixel: premultiplied for compositing",
                  pm.size() == 256u * 256u * 4u && std::abs(pm[0] - 0.502f) < 0.01f &&
                  std::abs(pm[1] - 0.251f) < 0.01f && std::abs(pm[3] - 0.502f) < 0.01f);
            P::TileWriter back(rgba8, P::transparent());
            back.set_premultiplied(pm);
            const auto b = back.freeze();
            check("pixel: premultiplied back to straight, within a step",
                  std::abs(b->get(0, 0).c[1] - t->get(0, 0).c[1]) <= 1.f / 255.f + 1e-6f);
        }

        // Sparse maps: absent = transparent; edits share what they don't touch.
        {
            const P::TileMap empty(rgba8, 6600, 10200);   // Scott's 600 dpi spread
            check("pixel: a 600 dpi spread holds nothing until painted",
                  empty.cols() == 26 && empty.rows() == 40 && empty.tile_count() == 0 && empty.bytes() == 0 &&
                  empty.get(5000, 9000) == P::transparent());
            const auto a = empty.fill({0, 0, 6600, 10200}, P::rgba(1, 1, 1, 1));
            check("pixel: a full fill is uniform tiles -- 1,040 tiles, 4 KB",
                  a.tile_count() == 26u * 40u && a.bytes() == 26u * 40u * 4u, std::to_string(a.bytes()));
            const auto b = a.fill({300, 300, 10, 10}, P::rgba(1, 0, 0, 1));
            check("pixel: a small fill touches one tile; the edge pixels are right",
                  b.get(300, 300).c[1] == 0.f && b.get(309, 309).c[1] == 0.f && b.get(310, 310).c[1] == 1.f &&
                  b.get(299, 300).c[1] == 1.f);
            int shared = 0;
            for (int r = 0; r < b.rows(); ++r)
                for (int c = 0; c < b.cols(); ++c) shared += b.same_tile(a, c, r) ? 1 : 0;
            check("pixel: ...and shares the other 1,039 with the map before (undo by tile pointer)",
                  shared == 26 * 40 - 1, std::to_string(shared));
            check("pixel: the map before is untouched (immutable)", a.get(300, 300).c[1] == 1.f);
            const auto c = b.fill({256, 256, 256, 256}, P::transparent());
            check("pixel: clearing a whole tile drops it", c.tile(1, 1) == nullptr && c.tile_count() == 26u * 40u - 1);
            const auto d = c.fill({-50, -50, 100, 100}, P::rgba(0, 0, 1, 1));
            check("pixel: fills clip to the map (blue in, white beyond)",
                  d.get(0, 0).c[0] == 0.f && d.get(49, 49).c[0] == 0.f && d.get(49, 49).c[2] == 1.f &&
                  d.get(50, 50).c[0] == 1.f);
            const auto e = d.fill({7000, 0, 10, 10}, P::rgba(0, 0, 0, 1));
            check("pixel: a fill wholly outside changes nothing", e.same_tile(d, 0, 0) && e.tile_count() == d.tile_count());
        }
    }

    std::cout << "-----------------------------------------------\n";

    // -- Recents: list ops + JSON round-trip with prune -----------------------
    {
        std::vector<std::string> l;
        core::recents_add(l, "/a");
        core::recents_add(l, "/b");
        core::recents_add(l, "/c");
        check("recents: newest first", l.size() == 3 && l.front() == "/c");
        core::recents_add(l, "/a");
        check("recents: re-add moves to front (no dup)",
              l.size() == 3 && l.front() == "/a");
        core::recents_add(l, "/x/");
        check("recents: trailing slash normalised",
              std::find(l.begin(), l.end(), "/x") != l.end() &&
              std::find(l.begin(), l.end(), "/x/") == l.end());
        std::vector<std::string> cap;
        for (int i = 0; i < 20; ++i)
            core::recents_add(cap, "/p" + std::to_string(i), 8);
        check("recents: trim to max (8)", cap.size() == 8 && cap.front() == "/p19");

        const std::string base =
            (std::filesystem::temp_directory_path() / "klimt_selftest_recents").string();
        std::error_code ec;
        std::filesystem::remove_all(base, ec);
        const std::string live = base + "/live";
        std::filesystem::create_directories(live, ec);
        const std::string file = base + "/recent.json";

        std::vector<std::string> to_save = {live, base + "/GONE"};
        check("recents: save ok", core::save_recents(file, to_save));
        auto loaded = core::load_recents(file);
        check("recents: load prunes the missing path",
              loaded.size() == 1 && loaded.front() == live);
        check("recents: missing file -> empty",
              core::load_recents(base + "/nope.json").empty());
    }

    // -- TextMap: forward/inverse round-trip ----------------------------------
    {
        // A scene as a viewer flattens it:
        //   0 "The Cellar"     title  (non-prose)
        //   1 ""               gap    (non-prose)
        //   2 "She opened the" prose, para_start   <- para 1, wrap 1
        //   3 "old iron door." prose               <- para 1, wrap 2
        //   4 "Caf\u00e9 dust." prose, para_start  <- para 2 (UTF-8: é)
        std::vector<core::FlatLine> flat = {
            {"The Cellar",     false, false},
            {"",               false, false},
            {"She opened the", true,  true },
            {"old iron door.", true,  false},
            {"Caf\xC3\xA9 dust.", true, true },
        };
        roundtrip(flat, 2, 0, 2, 14, "single line");
        roundtrip(flat, 2, 4, 3, 8,  "multi-line (soft wrap)");
        roundtrip(flat, 2, 0, 4, 10, "cross-paragraph");
        roundtrip(flat, 4, 0, 4, 6,  "UTF-8 span (accented)");

        // Chrome is never landed on: a range covering all prose starts on line 2.
        std::vector<int> off;
        (void)core::visible_text(flat, off);
        core::LineCol s, e;
        core::map_range(flat, 0, 3, s, e);
        check("textmap: start snaps to prose, never chrome", s.line == 2);

        // Degenerate / empty inputs fail cleanly.
        check("textmap: empty range rejected", !core::map_range(flat, 5, 5, s, e));
        std::vector<core::FlatLine> no_prose = {{"Title", false, false}};
        check("textmap: no prose -> false", !core::map_range(no_prose, 0, 3, s, e));
    }
    // -- Shortcuts: the registry paradigm, exercised ---------------------------
    // A seed's claim isn't a fact until a consumer exercises it. The registry's
    // whole promise is that a key and its advertised row can't drift, because
    // both read one list -- so the guard that matters is pure and lives here:
    // no two chords may collide. (App wires from this same list; ShortcutsDialog
    // renders from it.)
    {
        namespace sc = klimt::core;

        check("shortcuts: format_accel renders modifiers + key",
              sc::format_accel("<Ctrl><Shift>n") == "Ctrl+Shift+N",
              sc::format_accel("<Ctrl><Shift>n"));
        check("shortcuts: format_accel maps a named key to its glyph",
              sc::format_accel("<Ctrl>question") == "Ctrl+?",
              sc::format_accel("<Ctrl>question"));

        {
            sc::ShortcutSpec derived; derived.accels = {"<Ctrl>question", "<Ctrl>slash"};
            check("shortcuts: display_keys derives + joins accels",
                  derived.display_keys() == "Ctrl+?  /  Ctrl+/",
                  derived.display_keys());
            sc::ShortcutSpec doc; doc.keys = "Click a recent";
            check("shortcuts: display_keys prefers an explicit key string",
                  doc.display_keys() == "Click a recent");
        }

        check("shortcuts: registry is populated",
              !sc::shortcut_registry().empty(),
              std::to_string(sc::shortcut_registry().size()) + " specs");
        check("shortcuts: registry is accel-collision-free",
              sc::find_accel_collisions().empty(),
              sc::find_accel_collisions().empty()
                  ? "clean" : sc::find_accel_collisions().front());

        // Sections authored A-Z + contiguous: the dialog walks linearly and
        // starts a heading on change, so a stray out-of-order row would split a
        // section into two headings.
        {
            std::vector<std::string> secs;
            for (const auto& r : sc::shortcut_registry())
                if (secs.empty() || secs.back() != r.section) secs.push_back(r.section);
            std::vector<std::string> sorted = secs;
            std::sort(sorted.begin(), sorted.end());
            check("shortcuts: sections are contiguous and A-Z ordered", secs == sorted);
        }

        // The doc-only half of the paradigm: a gesture row documents without
        // binding (no accels), but must still show keys.
        {
            bool ok = true, saw_doc_only = false;
            for (const auto& r : sc::shortcut_registry())
                if (r.action.empty()) {
                    saw_doc_only = true;
                    if (!r.accels.empty() || r.keys.empty()) ok = false;
                }
            check("shortcuts: a doc-only row binds nothing but still displays keys",
                  ok && saw_doc_only);
        }
    }



    selftest_formats([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_files([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_dirty([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_select([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_paint([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_move([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_canvas([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_pen([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_bounds([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_transform([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_modify([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_masks([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_gradient([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_adjust([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_filter([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_retouch([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_brush([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_dynamics([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_tooldefaults([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_filterlayers([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_sharpen([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_pixelate([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_paths([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_isolate([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_rulers([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_styles([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_tabs([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_fade([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_partly([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_presets([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_transfer([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_session([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_groups([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_range([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });
    selftest_channels([](const std::string& n, bool ok, const std::string& d) { check(n, ok, d); });

    std::cout << "-----------------------------------------------\n";
    std::cout << g_pass << " pass / " << g_fail << " fail\n";
    return g_fail == 0 ? 0 : 1;
}
