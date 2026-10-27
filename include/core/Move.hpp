#pragma once
// core::Move -- the Move tool's pixel work (s009 m1). GTK-free; docs/move.md.
//
// Two ways to move, Photoshop's:
//   * NO selection: the selected layers' pixels slide by (dx, dy) -- a group
//     carries every pixel layer inside it;
//   * a selection: the selected pixels are LIFTED (the hole they leave is
//     transparent -- or the background colour on a layer with no alpha) and
//     land (dx, dy) away, over what is there; the selection travels with them.
//
// Whole pixels only (dx, dy are ints: the Move tool doesn't resample). The
// Position lock bites here at last, with All (on the layer or a group above).
//
// THE MODEL, the brush's: a move is always computed from the ORIGINAL (what
// the layers held at the press), so a drag that goes off the edge and comes
// back loses nothing until it is let go. The work is pure (compute() on a
// worker); put() writes the result inside the door. A layer is the size of
// the canvas, so what is off the canvas when a move LANDS is gone
// (WEAKNESSES: layers bigger than the canvas).
#include "core/Document.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::move {

using Id = Document::Id;
using MapPtr = std::shared_ptr<const pixel::TileMap>;

// The pixel layers a move of `selected` acts on: each selected pixel layer,
// and every pixel layer inside a selected group, once each, in walk order.
std::vector<Id> targets(const Document& d, const std::vector<Id>& selected);
// May they move? "" yes, else why not (for a toast): nothing to move, a
// Position or All lock (on the layer or a group above it).
std::string refusal(const Document& d, const std::vector<Id>& selected);

// What a move starts from: the targets' pixels at the press, the selection
// then, and (with a selection) each layer with its selected pixels lifted out.
struct Plan {
    std::vector<Id>     ids;
    std::map<Id, MapPtr> original;   // nullptr: the layer was empty
    std::map<Id, MapPtr> base;       // with a selection: original minus the lifted pixels
    MapPtr              mask;        // the selection at the press; nullptr: none
    pixel::Px           fill;        // what a layer with no alpha shows where pixels left
    int                 width = 0, height = 0;
    pixel::Format       format;
};
// nullopt when refused.
std::optional<Plan> plan(const Document& d, const std::vector<Id>& selected, const pixel::Px& background);

// The result of moving by (dx, dy): each target's new pixels (nullptr:
// empty) and the selection's new mask (nullptr: it went wholly off the
// canvas). nullopt when cancelled. Pure -- a worker runs it.
struct Result {
    std::map<Id, MapPtr> pixels;
    MapPtr              mask;
};
std::optional<Result> compute(const Plan& p, int dx, int dy, const std::function<bool()>& cancelled = {});

// Write a result: the layers (and with a selection, the selection). INSIDE
// the door (a gesture, or run's op). Layers gone since the plan are skipped.
void put(Document& d, const Plan& p, const Result& r);
// The ids a step of this move touches (the targets, and @selection with one).
std::vector<Id> step_ids(const Plan& p);

// ── pure pieces ─────────────────────────────────────────────────────────────
// `src` slid by (dx, dy): what comes in from beyond the edge is `fill`.
// Bytes are copied, not decoded; a whole tile of one value stays uniform.
pixel::TileMap shifted(const pixel::TileMap& src, int dx, int dy, const pixel::Px& fill);
// `top` over `under`, `top`'s alpha (no alpha: 1) scaled by `cov` (a mask
// map at the same size), in the rectangle `r` only.
pixel::TileMap over(const pixel::TileMap& under, const pixel::TileMap& top, const pixel::TileMap& cov, const pixel::Rect& r);

// ── the verb (synchronous: the verb table, tests). The app runs the same in
// pieces on a worker (Shell_move.cpp). One step "Move"; with merge_key,
// presses inside the merge window fold into one (the arrow keys).
bool by(DocJournal& j, Document& d, const std::vector<Id>& selected, int dx, int dy,
        const pixel::Px& background, const std::string& merge_key = {});

}  // namespace klimt::core::move
