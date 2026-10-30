#pragma once
// core::matte -- A CUT-OUT'S EDGE (s036; Scott: "on masks a defringe and
// edge softness feature could be useful"). docs/masks.md.
//
// Where a mask cuts a figure from its picture, the edge pixels are a MIX:
// part figure, part the old background (a green fringe round a figure cut
// from a lawn). Two things here:
//
//   * colour ESTIMATES -- for every pixel, the figure's colour nearby and the
//     background's colour nearby, filled in from the pixels sure to be one or
//     the other (push-pull: averaged down a pyramid, then pulled back up into
//     the gaps), at a quarter of the resolution (they vary slowly).
//   * DEFRINGE -- within `width` px of the mask's edge, each pixel's colour is
//     split into figure + background along the line between the two
//     estimates (I = (1 - t) F + t B), and the background's share swapped
//     for the figure's colour: I + t (F - B). A pixel that isn't
//     background-coloured (off that line) keeps its colour and its texture;
//     full strength for the first half of `width`, fading to 0 at it.
//
// Pure, GTK-free, any mode and depth. The mask is the layer mask's format:
// how much it HIDES.
#include "pixel/Pixel.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace klimt::core::matte {

// A colour field: `ch` straight colour channels per cell, W x H cells, each
// covering `cell` x `cell` image pixels. at() samples bilinearly at an image
// pixel's centre.
struct Field {
    int W = 0, H = 0, ch = 0, cell = 4;
    std::vector<float> v;   // W * H * ch
    void at(double x, double y, float* out) const;
    bool empty() const { return v.empty(); }
};

// Fill a field from samples: `add(cx, cy, colour, weight)` feeds it while
// `feed` runs (cx, cy: image pixels); cells no sample reached are pulled in
// from the coarser levels (push-pull). Empty when nothing was fed.
Field filled(int width, int height, int ch, int cell,
             const std::function<void(const std::function<void(int, int, const float*, float)>&)>& feed);

// `pixels` (a layer's) with the fringe taken out at the edge of `hide` (the
// layer mask as it acts; same size). `width`: px into the figure. nullopt
// when cancelled; `pixels` unchanged (shared tiles) where nothing changes.
std::optional<pixel::TileMap> defringed(const pixel::TileMap& pixels, const pixel::TileMap& hide, double width,
                                        const std::function<bool()>& cancelled = {});

// s036 m3: a SOFT EDGE for a hard cut (Isolate's next step -- hair, fur,
// soft focus). Within `radius` px of `cover`'s edge (cover: how much is
// SELECTED, the selection's format), each pixel's coverage comes from its
// colour: where it lies between the figure's colour and the background's
// near it (the estimates, from pixels beyond the band on each side), alpha =
// 1 - t. A colour that is neither (off the line) keeps the hard edge's
// value. `picture`: what the eye sees (any mode, same size). nullopt when
// cancelled.
std::optional<pixel::TileMap> soft_edge(const pixel::TileMap& picture, const pixel::TileMap& cover, double radius,
                                        const std::function<bool()>& cancelled = {});

}  // namespace klimt::core::matte
