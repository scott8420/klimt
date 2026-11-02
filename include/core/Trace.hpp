#pragma once
// core::trace -- THE TRACER (s081; REQUIREMENTS "A Streamline-style tracer";
// docs/paths.md "The tracer"). A closed outline of points (a mask's edge,
// from core/Paths.hpp's marching squares) -> clean Beziers with the FEWEST
// anchors: corners where the shape turns, smooth anchors only where a curve
// needs them, straight runs without handles.
//
// Our own code (Potrace is GPL), in three stages:
//   1. the POLYGON: the fewest straight edges whose lines pass within
//      `straight` px of every point between their ends (a cone of allowed
//      directions narrowed point by point; the shortest cyclic cover);
//      each edge's line then refitted to its points (least squares) and the
//      vertices put where neighbouring lines meet -- a staircase of hard
//      pixels becomes the straight edge it stands for;
//   2. CORNERS: a vertex is a corner when it turns more than a smooth curve
//      would at that edge length (a curve's chord of length l, kept within
//      e of it, turns ~8e / l; a corner turns `corner` times more than even
//      a loose curve's 16e / l, and 10 degrees at least);
//   3. CURVES: between corners, the fewest cubics -- each reaching as far
//      along the smooth vertices as one cubic can stay within `tolerance`
//      px of the points (Schneider's least-squares handles, tangents fixed
//      by the polygon); a run within reach of its chord is a straight
//      segment. A loop with no corner: the start that needs the fewest.
// Pure value code, GTK-free and pixel-free: one tracer meant to be shared
// with Curvz and Granjon.
#include "core/Path.hpp"

#include <vector>

namespace klimt::core::trace {

struct Options {
    double tolerance = 2.0;   // px the curves may stray from the outline (Make Path's Tolerance)
    double straight = 1.2;    // px the polygon's lines may stray (a hard pixel edge's staircase is ~0.35)
    double corner = 1.6;      // how much sharper than a curve a corner must turn
};

// Stage 1 + 2, for the tests and the curious: the vertices (where they are
// on the loop -- point indices, ascending -- and where they were put), and
// which are corners.
struct Polygon {
    std::vector<int>     at;
    std::vector<path::P> v;
    std::vector<bool>    corner;
};
Polygon polygon(const std::vector<path::P>& loop, const Options& o = {});

// The whole tracer: a closed subpath. Fewer than 3 points: those points.
path::Sub fitted(const std::vector<path::P>& loop, const Options& o = {});

}  // namespace klimt::core::trace
