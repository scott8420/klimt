#pragma once
// core::isolate -- "LOOK HERE" (s035 m7; Scott: "many times a path is
// following the outer bounds of a figure ... by looking at the intent we
// might be able to isolate the image cleaner than the path itself").
// docs/paths.md.
//
// The path is taken as a HINT: the figure lies inside it, give or take
// `band` pixels (a path may cut into it a little). Beyond path + band is
// background, sure; everything inside is decided from the picture -- a path's
// corners may be far from the figure -- by GrabCut (Rother, Kolmogorov &
// Blake 2004): a colour model of each side (a mixture of five Gaussians; the
// figure's first from the path's inside, the background's from a ring just
// outside), and a minimum cut (core/MaxFlow.hpp) that pays for every pixel
// given to the less likely side and for every neighbour pair split across a
// weak colour edge. Three rounds, the models re-learnt from each cut. Much
// to decide: COARSE first (cells averaged, ~300k at most), then the full
// resolution in a strip along the coarse edge. The result: a mask on the
// figure's real edge, anti-aliased.
//
// Pure, GTK-free; a worker runs it (cancellable).
#include "core/Path.hpp"
#include "pixel/Pixel.hpp"

#include <functional>
#include <optional>

namespace klimt::core::isolate {

struct Options {
    double band = 12;     // how far the path may cut into the figure, px (2 .. 64)
    int    rounds = 3;    // cut, re-learn, cut ...
    double soft = 0;      // s036: a soft edge this wide (px, 0: off) -- hair, fur (core/Matte.hpp)
};

// `picture`: what the eye sees (RGB / RGBA, any depth). The mask: the
// selection's format (`mf`), the picture's size. nullopt when cancelled or
// when the path encloses nothing.
std::optional<pixel::TileMap> isolated(const pixel::TileMap& picture, const path::Path& p, const pixel::Format& mf,
                                       const Options& o = {}, const std::function<bool()>& cancelled = {},
                                       const std::function<void(double)>& progress = {});

}  // namespace klimt::core::isolate
