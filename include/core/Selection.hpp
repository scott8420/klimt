#pragma once
// core::Selection -- the selection (s008). GTK-free. docs/selection.md.
//
// A selection is a MASK (Scott, s007 -> s008, answer 1): one channel at the
// document's depth on the same sparse 256-tile grid as the pixels -- 0 not
// selected, 1 selected, between = partly (an anti-aliased or feathered
// edge). Absent tiles are unselected; a wholly selected tile is one uniform
// pixel. So "Select All" on a 600 dpi spread costs a few KB, and Select
// Subject / Refine Edge (later) are just mask pixels.
//
// Two halves, like Composite:
//   * PURE functions over masks (rect / ellipse / polygon shapes, combine,
//     invert, feather; Fill / Delete through a mask; the outline the canvas
//     draws as marching ants) -- a worker may run any of them;
//   * VERBS that put a mask in the document through the journal (every
//     selection change is a step -- answer 2): select::set is the one door,
//     and each named verb is set() of a pure result.
#include "core/Document.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::select {

// How a new shape meets the selection already there (Photoshop's modifier
// keys at the press: none Replace, Shift Add, Alt Subtract, Shift+Alt
// Intersect). Set arithmetic on masks: max, min(a, 1-b), min.
enum class Op { Replace, Add, Subtract, Intersect };
const char* op_name(Op op);                // "Add to Selection" ...

struct Pt { double x = 0, y = 0; };        // image pixels; (0, 0) the top-left CORNER

// ── pure: shapes, at a mask format and image size ───────────────────────────
// A rectangle: hard, pixel-aligned (Photoshop's marquee). Clipped to the image.
pixel::TileMap rect_mask(const pixel::Format& mf, int w, int h, const pixel::Rect& r);
// An ellipse inscribed in [x0, x1) x [y0, y1), anti-aliased (4 x 4 samples
// on the edge only; tiles wholly in or out are decided whole).
pixel::TileMap ellipse_mask(const pixel::Format& mf, int w, int h, double x0, double y0, double x1, double y1);
// A closed polygon (the lasso's points), non-zero winding, anti-aliased
// (4 sub-rows a pixel, exact horizontal coverage).
pixel::TileMap polygon_mask(const pixel::Format& mf, int w, int h, const std::vector<Pt>& pts);

// `shape` into `old` (nullptr: nothing selected) by `op`.
pixel::TileMap combine(const pixel::TileMap* old, const pixel::TileMap& shape, Op op);
pixel::TileMap all_mask(const pixel::Format& mf, int w, int h);
pixel::TileMap inverted(const pixel::TileMap* old, const pixel::Format& mf, int w, int h);
// A Gaussian-like blur of the mask (three box passes), radius in pixels.
// nullopt when cancelled.
std::optional<pixel::TileMap> feathered(const pixel::TileMap& m, double radius,
                                        const std::function<bool()>& cancelled = {});
bool empty(const pixel::TileMap& m);       // nothing selected anywhere

// s011 m4 -- Select > Modify. The selection's edge (coverage >= 1/2), moved
// by an exact Euclidean distance, with a one-pixel soft edge. The canvas's
// edge is not "unselected" (Photoshop's default: a selection touching the
// edge doesn't shrink from it). nullopt when cancelled.
//   Expand   -- out by r;   Contract -- in by r;
//   Border   -- a band r wide, centred on the edge;
//   Smooth   -- each pixel takes the majority of its (2r + 1)^2 square (hard).
enum class Modify { Expand, Contract, Border, Smooth };
const char* modify_name(Modify k);   // "Expand"
std::optional<pixel::TileMap> modified(const pixel::TileMap& m, Modify k, double r,
                                       const std::function<bool()>& cancelled = {});
// The tiles' extent (coarse: whole tiles), empty Rect when nothing.
pixel::Rect extent(const pixel::TileMap& m);
float coverage(const pixel::TileMap& m, int x, int y);   // 0..1; outside: 0

// The Magic Wand (s008 m3): the pixels of `src` like the one at (x, y) --
// every channel, alpha included, within `tolerance` (0..1 of full scale;
// Photoshop's 32 is 32 / 255). Contiguous: only those joined to (x, y)
// edge to edge; else all of them. Hard-edged. nullopt when cancelled.
std::optional<pixel::TileMap> wand_mask(const pixel::TileMap& src, const pixel::Format& mf, int x, int y,
                                        float tolerance, bool contiguous,
                                        const std::function<bool()>& cancelled = {});

// The marching ants: the boundary between selected (>= half) and not, as
// axis-aligned segments on pixel edges, merged into runs. Image coords.
struct Seg { int x0, y0, x1, y1; };
std::vector<Seg> outline(const pixel::TileMap& m);

// ── pure: pixels through a mask ─────────────────────────────────────────────
// Fill `colour` (straight, the layer's format) where the mask says (nullptr:
// everywhere). Alpha lock: colour only, where there is something, alpha kept.
pixel::TileMap filled(const pixel::TileMap& layer, const pixel::TileMap* mask, const pixel::Px& colour, bool alpha_lock);
// Delete what the mask says: alpha *= (1 - m). A format with no alpha (a
// JPEG's Background) can't go transparent: it takes `background` instead.
pixel::TileMap cleared(const pixel::TileMap& layer, const pixel::TileMap* mask, const pixel::Px& background);

// ── verbs: through the journal ──────────────────────────────────────────────
// THE door for the selection. `mask` nullopt or empty: nothing selected.
// With `expect`, refused unless the selection is still that one (a job that
// planned on a copy lands through this; if the selection moved meanwhile,
// nothing happens). A change that changes nothing keeps no step.
bool set(DocJournal& j, Document& d, std::optional<pixel::TileMap> mask, const std::string& label);
bool set_if(DocJournal& j, Document& d, const std::shared_ptr<const pixel::TileMap>& expect,
            std::optional<pixel::TileMap> mask, const std::string& label);
bool rect(DocJournal& j, Document& d, const pixel::Rect& r, Op op = Op::Replace);
bool ellipse(DocJournal& j, Document& d, double x0, double y0, double x1, double y1, Op op = Op::Replace);
bool polygon(DocJournal& j, Document& d, const std::vector<Pt>& pts, Op op = Op::Replace);
bool all(DocJournal& j, Document& d);        // Ctrl+A
bool none(DocJournal& j, Document& d);       // Ctrl+D -- Deselect
bool invert(DocJournal& j, Document& d);     // Ctrl+Shift+I
bool feather(DocJournal& j, Document& d, double radius);
bool modify(DocJournal& j, Document& d, Modify k, double r);   // s011 m4: "Expand 8 px" ...
// s016 -- SAVED SELECTIONS. Save: the selection as a new channel ("Alpha 1",
// "Alpha 2" ... unless named), its id back ("" refused: nothing selected).
// Load: the channel into the selection by `op`. Remove: the channel gone.
// Rename: a new name (not empty). Each ONE step.
Document::Id save_channel(DocJournal& j, Document& d, const std::string& name = {});
// s026: a mask as a new saved channel (the same naming), `step` the step's label.
Document::Id save_channel_mask(DocJournal& j, Document& d, std::shared_ptr<const pixel::TileMap> mask,
                               const std::string& name, const std::string& step);
bool load_channel(DocJournal& j, Document& d, const Document::Id& id, Op op = Op::Replace);
bool remove_channel(DocJournal& j, Document& d, const Document::Id& id);
bool rename_channel(DocJournal& j, Document& d, const Document::Id& id, const std::string& name);

}  // namespace klimt::core::select

namespace klimt::core::layers {
// s008 -- pixels through the selection, as steps (refused as fill / clear are:
// a group, an All or Pixels lock; Delete also under an Alpha lock when the
// layer has alpha). No selection: the whole layer.
bool fill_selection(DocJournal& j, Document& d, const Id& id, const pixel::Px& colour);
bool clear_selection(DocJournal& j, Document& d, const Id& id, const pixel::Px& background);
// A job that computed a layer's new pixels on a copy lands through this:
// refused unless the layer's pixels are still `expect` (and writable).
bool set_pixels_if(DocJournal& j, Document& d, const Id& id, const std::shared_ptr<const pixel::TileMap>& expect,
                   pixel::TileMap after, const std::string& label);
}  // namespace klimt::core::layers
