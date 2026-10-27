#pragma once
// core::Transform -- Free Transform, flips and rotations (s011 m1). GTK-free;
// docs/transform.md.
//
// Scott, s010 -> s011: "transform selection if any else the whole layer".
//   * a selection: its pixels are lifted (as the Move tool lifts them) and
//     land transformed over what is left; the selection is transformed with
//     them -- one step;
//   * no selection: every selected layer (a group's pixel layers) is
//     transformed whole, about the box around everything they hold -- the
//     pixels beyond the canvas included, and kept where they land beyond it.
//
// The work is the Move tool's shape: a Plan made at the start (the originals,
// the lifted bases, the box), compute(plan, M) pure on a worker, move::put to
// land. Fast (bilinear) while the handles are dragged, Best (bicubic,
// widened when it shrinks) for the step that lands.
#include "core/Canvas.hpp"
#include "core/Document.hpp"
#include "core/Move.hpp"
#include "core/Selection.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::xform {

using Pt = select::Pt;

// x' = a x + c y + e,  y' = b x + d y + f   (Cairo's order)
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    Pt apply(Pt p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
    Affine inverse() const;
    bool identity() const { return a == 1 && b == 0 && c == 0 && d == 1 && e == 0 && f == 0; }
    static Affine translate(double x, double y) { return {1, 0, 0, 1, x, y}; }
    static Affine scale(double sx, double sy) { return {sx, 0, 0, sy, 0, 0}; }
    static Affine rotate(double radians);
    // About a point: translate(p) * m * translate(-p).
    static Affine about(Pt p, const Affine& m);
};
Affine operator*(const Affine& l, const Affine& r);   // l after r

enum class Quality { Fast, Best };

// The box a rectangle lands in under m (rounded out to whole pixels).
pixel::Rect mapped(const pixel::Rect& r, const Affine& m);

// THE RESAMPLE. `base`, with `src`'s pixels inside `box` (times `mask`'s
// coverage when given) mapped by m laid over it, premultiplied. `src` and
// `base` may hold pixels beyond the image; so does the result where they
// land. Outside `box`, src contributes nothing (a format with no alpha: the
// box's own edge is its edge). nullopt when cancelled.
std::optional<pixel::TileMap> over_transformed(const pixel::TileMap& base, const pixel::TileMap& src,
                                               const pixel::TileMap* mask, const pixel::Rect& box,
                                               const Affine& m, Quality q,
                                               const std::function<bool()>& cancelled = {});

// What a transform starts from.
struct Plan {
    move::Plan  p;     // the targets, their originals, the lifted bases, the selection
    pixel::Rect box;   // what is transformed: the selection's bounds, or all the targets hold
};
// "" yes, else why not (nothing to transform; a Position / All lock).
std::string refusal(const Document& d, const std::vector<Document::Id>& selected);
std::optional<Plan> plan(const Document& d, const std::vector<Document::Id>& selected, const pixel::Px& background);
// The box alone, from a Move plan: the selection's bounds, or everything the
// targets hold. Pure (it scans pixels -- a worker's job); empty: nothing.
pixel::Rect box_of(const move::Plan& p);
// The layers (and the selection) under m. Pure.
std::optional<move::Result> compute(const Plan& p, const Affine& m, Quality q,
                                    const std::function<bool()>& cancelled = {});

// ── verbs (synchronous; the app runs the same on a worker) ──────────────────
bool apply(DocJournal& j, Document& d, const std::vector<Document::Id>& selected, const Affine& m,
           const std::string& label, const pixel::Px& background);
// About the box's centre: Flip Horizontal / Vertical, Rotate 90 CW (+90), CCW
// (-90), 180.
Affine flip_about(const pixel::Rect& box, bool horizontal);
Affine rotate_about(const pixel::Rect& box, double degrees);
bool flip(DocJournal& j, Document& d, const std::vector<Document::Id>& selected, bool horizontal, const pixel::Px& background);
bool rotate(DocJournal& j, Document& d, const std::vector<Document::Id>& selected, double degrees, const pixel::Px& background);

// ── the whole canvas (s011 m3): Image > Image Rotation, Crop's Straighten ──
// Plans over a canvas::Before (a worker makes them; canvas::land lands them).
enum class Turn { Clockwise, CounterClockwise, Half, FlipHorizontal, FlipVertical };
const char* turn_label(Turn t);
// Exact: every pixel lands on a pixel; the selection turns too.
std::optional<canvas::Reshape> turn_plan(const canvas::Before& b, Turn t, const std::function<bool()>& cancelled = {});
// Any angle (degrees, clockwise): the canvas grows to hold the turned image;
// the new corners are transparent (no alpha: the background colour).
std::optional<canvas::Reshape> rotate_plan(const canvas::Before& b, double degrees, const pixel::Px& background,
                                           const std::function<bool()>& cancelled = {});
// Straighten: the line from p to q (image coords) becomes level (or upright,
// when it is nearer vertical); the canvas is cropped to the largest rectangle
// of its own proportions inside the turned image. What falls outside is kept
// beyond the canvas. The selection is dropped (a crop's way). nullopt: the
// line is too short, or already level.
std::optional<canvas::Reshape> straighten_plan(const canvas::Before& b, Pt p, Pt q, const pixel::Px& background,
                                               const std::function<bool()>& cancelled = {});
double straighten_angle(Pt p, Pt q);
// The verbs (plan + canvas::land), one step each.
bool turn(DocJournal& j, Document& d, Turn t);
bool rotate_canvas(DocJournal& j, Document& d, double degrees, const pixel::Px& background);
bool straighten(DocJournal& j, Document& d, Pt p, Pt q, const pixel::Px& background);   // degrees to turn (clockwise +) so p-q is level / upright

}  // namespace klimt::core::xform
