#pragma once
// core::path -- PATHS (s035): Bezier outlines for SELECTIONS and MASKS.
// docs/paths.md.
//
// Inside the scope wall (RULES "Pixels only"): a path here is never artwork
// -- it is never stroked or filled as a picture. It becomes a selection
// (Ctrl+Enter), a layer mask, or comes FROM a selection (Make Work Path).
//
// A path is SUBPATHS of ANCHORS. Each anchor has two handles, stored as
// absolute image positions: `in` (the curve arriving) and `out` (leaving). A
// handle sitting on its anchor is "no handle" -- a straight segment there.
// A SMOOTH anchor keeps its handles in line (dragging one turns the other).
// Open subpaths fill as if closed (Photoshop's way); subpaths combine
// even-odd -- a ring inside a shape is a hole.
//
// Pure value types, GTK-free, no pixels: the geometry here; the masks and
// the verbs in core/Paths.hpp.
#include <functional>
#include <string>
#include <vector>

namespace klimt::core::path {

struct P {
    double x = 0, y = 0;   // image pixels; (0, 0) the top-left CORNER
    bool operator==(const P&) const = default;
};

struct Node {
    P    at, in, out;
    bool smooth = false;
    bool operator==(const Node&) const = default;
    static Node corner(P p) { return {p, p, p, false}; }
};

struct Sub {
    std::vector<Node> nodes;
    bool closed = false;
    bool operator==(const Sub&) const = default;
};

struct Path {
    std::vector<Sub> subs;
    bool empty() const;                      // no subpath with an anchor
    std::size_t anchors() const;
    bool operator==(const Path&) const = default;
};

// A cubic: the curve from a to b, through a.out and b.in.
P bezier(const Node& a, const Node& b, double t);
// The polygons the path fills: every subpath with 2+ anchors, closed,
// flattened so no chord strays more than `tol` px from its curve.
std::vector<std::vector<P>> flatten(const Path& p, double tol = 0.1);
// The same, but an OPEN subpath stays open (what the canvas draws).
std::vector<std::vector<P>> outline(const Path& p, double tol = 0.25);
// Every point (anchors and handles) through `f` -- an affine map takes a
// Bezier exactly (crop, canvas size, image size, rotate / flip the canvas).
Path mapped(const Path& p, const std::function<P(P)>& f);
// The anchors' and handles' bounding box: x0, y0, x1, y1. All 0 when empty.
struct Box { double x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
Box box(const Path& p);

// ── hit testing, for the Pen ────────────────────────────────────────────────
struct Hit {
    enum What { None, Anchor, In, Out, Segment } what = None;
    int sub = -1, node = -1;    // Segment: from node to node + 1 (the closing one wraps)
    double t = 0;               // Segment: where on it
};
// The nearest thing within `tol` px of q: anchors first, then handles, then
// segments.
Hit hit(const Path& p, P q, double tol);

// ── editing (pure; the Pen's gestures land through core/Paths.hpp) ──────────
// An anchor split into the segment at t (de Casteljau: the curve keeps its
// shape). The new anchor's index is node + 1.
Path inserted(const Path& p, int sub, int node, double t);
// An anchor out; a subpath left with none goes too.
Path removed(const Path& p, int sub, int node);
// Move an anchor (its handles with it), a handle (a smooth anchor's other
// handle turns to stay in line, keeping its length), by `d`.
Path moved(const Path& p, int sub, int node, Hit::What what, P d);
// Corner <-> smooth: a corner loses its handles; a smooth point gets handles
// along its neighbours' chord, a third of the way to each.
Path toggled(const Path& p, int sub, int node);

// ── fitting: polylines to Beziers (Schneider, Graphics Gems 1990) ───────────
// A CLOSED loop of points (no repeat of the first at the end) fitted with
// cubics no further than `tol` px from it; sharp turns become corner anchors
// (their place the meeting of the straight runs either side, so a hard
// rectangle comes back with its corners), the rest smooth.
Sub fit(const std::vector<P>& loop, double tol);

}  // namespace klimt::core::path
