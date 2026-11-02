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

// s073: a PROJECTIVE map (a homography) -- Free Transform's Skew, Distort and
// Perspective. Row-major 3 x 3:  X = h0 x + h1 y + h2,  Y = h3 x + h4 y + h5,
// W = h6 x + h7 y + h8;  (x', y') = (X / W, Y / W). An affine map is the one
// with h6 = h7 = 0 (and h8 = 1): every affine path (type kept as type, the
// lossless flips) still takes it.
struct Quad { Pt p[4]; };   // corners: top-left, top-right, bottom-right, bottom-left
struct Projective {
    double h[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    Projective() = default;
    Projective(const Affine& m) : h{m.a, m.c, m.e, m.b, m.d, m.f, 0, 0, 1} {}   // NOLINT: implicit on purpose
    Pt apply(Pt p) const;
    double w_at(Pt p) const { return h[6] * p.x + h[7] * p.y + h[8]; }   // > 0 where the map is valid
    Projective inverse() const;
    bool affine() const;           // h6, h7 ~ 0 (within 1e-12 of h8's scale)
    Affine to_affine() const;      // its affine part (exact when affine())
    bool identity() const { return affine() && to_affine().identity(); }
    // The map taking the box's corners to q's (tl, tr, br, bl). nullopt: q is
    // not a convex, non-degenerate quad (a bow-tie or a fold has no map that
    // keeps the picture in one piece).
    static std::optional<Projective> box_to_quad(const pixel::Rect& box, const Quad& q);
    static std::optional<Projective> box_to_quad(double x, double y, double w, double h, const Quad& q);
};
Projective operator*(const Projective& l, const Projective& r);   // l after r
// q's corners are a convex quad, turning one way, its area at least min_area.
bool convex(const Quad& q, double min_area = 1e-6);

enum class Quality { Fast, Best };

// s074: WARP -- a 4 x 4 mesh of control points over the box: one bicubic
// Bezier patch (Photoshop's Warp). p[row * 4 + col], row 0 the top; the
// corners are p[0], p[3], p[12], p[15]; the rest are their handles (the
// sides') and the inner four. Evenly spaced points are the identity.
struct Mesh {
    Pt p[16];
    static Mesh of(const pixel::Rect& box);                          // the identity over the box
    static Mesh of(const pixel::Rect& box, const Projective& m);     // the grid mapped by m (exact when m is affine)
    Pt at(double u, double v) const;                                 // the patch at (u, v), 0..1 across the box
    bool identity_for(const pixel::Rect& box) const;
};
pixel::Rect mapped(const pixel::Rect& box, const Mesh& m);   // the control points' box (the patch lies inside them)

// The box a rectangle lands in under m (rounded out to whole pixels).
pixel::Rect mapped(const pixel::Rect& r, const Affine& m);
pixel::Rect mapped(const pixel::Rect& r, const Projective& m);   // s073: empty when a corner crosses the horizon

// THE RESAMPLE. `base`, with `src`'s pixels inside `box` (times `mask`'s
// coverage when given) mapped by m laid over it, premultiplied. `src` and
// `base` may hold pixels beyond the image; so does the result where they
// land. Outside `box`, src contributes nothing (a format with no alpha: the
// box's own edge is its edge). nullopt when cancelled.
// s073: m is projective -- the filter is widened per pixel, by how much the
// map shrinks there (a far side of a perspective shrinks more than a near).
std::optional<pixel::TileMap> over_transformed(const pixel::TileMap& base, const pixel::TileMap& src,
                                               const pixel::TileMap* mask, const pixel::Rect& box,
                                               const Projective& m, Quality q,
                                               const std::function<bool()>& cancelled = {});

// s074: the same over a warp's mesh (defined over `box`): the patch is cut
// into small triangles, each landing pixel takes its source point from the
// one it falls in (a fold: the one drawn last), and the filter is widened by
// that triangle's shrink.
std::optional<pixel::TileMap> over_warped(const pixel::TileMap& base, const pixel::TileMap& src,
                                          const pixel::TileMap* mask, const pixel::Rect& box,
                                          const Mesh& m, Quality q,
                                          const std::function<bool()>& cancelled = {});

// What a transform starts from.
struct Plan {
    move::Plan  p;     // the targets, their originals, the lifted bases, the selection
    pixel::Rect box;   // what is transformed: the selection's bounds, or all the targets hold
};
// "" yes, else why not (nothing to transform; a Position / All lock).
std::string refusal(const Document& d, const std::vector<Document::Id>& selected, const Document::Id& mask_target = {});
std::optional<Plan> plan(const Document& d, const std::vector<Document::Id>& selected, const pixel::Px& background,
                         const Document::Id& mask_target = {});   // s059: see move::plan
// The box alone, from a Move plan: the selection's bounds, or everything the
// targets hold. Pure (it scans pixels -- a worker's job); empty: nothing.
pixel::Rect box_of(const move::Plan& p);
// The layers (and the selection) under m. Pure. s073: m projective -- a type
// layer can only be mapped affinely (type stays type): with a non-affine m
// it is left as it was; projective_refusal says so first.
std::optional<move::Result> compute(const Plan& p, const Projective& m, Quality q,
                                    const std::function<bool()>& cancelled = {});
// s073: "" a non-affine map is fine for this plan, else why not (a type layer).
std::string projective_refusal(const Plan& p);
// s074: the layers under a warp (the mesh over p.box). Type is left as it was
// (projective_refusal says so first). A whole layer's mask is warped over the
// box (beyond it the mask shows all). Pure.
std::optional<move::Result> compute_warp(const Plan& p, const Mesh& m, Quality q,
                                         const std::function<bool()>& cancelled = {});

// ── verbs (synchronous; the app runs the same on a worker) ──────────────────
bool apply(DocJournal& j, Document& d, const std::vector<Document::Id>& selected, const Projective& m,
           const std::string& label, const pixel::Px& background);
// s073: the box (what plan() would transform) to the quad -- Distort /
// Perspective as one verb (the selftest's door). False: no plan, a bad quad,
// or a type layer.
bool distort(DocJournal& j, Document& d, const std::vector<Document::Id>& selected, const Quad& q,
             const std::string& label, const pixel::Px& background);
// s074: Warp as one verb. `bend(box)` gives the mesh for the plan's box (the
// selftest's door). False: no plan, a type layer, or the identity.
bool warp(DocJournal& j, Document& d, const std::vector<Document::Id>& selected,
          const std::function<Mesh(const pixel::Rect&)>& bend, const std::string& label, const pixel::Px& background);
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
