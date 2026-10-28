#pragma once
// core::heal -- THE HEALING BRUSHES (s015 m3). GTK-free; docs/retouch.md.
//
// Photoshop's Healing Brush takes the TEXTURE from a source and the COLOUR
// and LIGHT from around where you paint (it solves a Poisson equation). Ours
// is the cheap, close relative of that:
//
//     healed = source + (smooth(destination) - smooth(source))
//
// where smooth() is a Gaussian blur that LEAVES THE STROKE OUT (each pixel
// weighted by 1 - coverage, then divided by the blurred weights) -- so the
// smooth colour under the stroke is filled in from the stroke's surroundings,
// not from the blemish being covered. The source's own detail (its pixels
// minus their smooth part) rides on top. The blur's reach follows the brush
// (0.75 x its diameter): wide enough to bridge the stroke.
//
// The healed colours become a clone source with no offset, so the stroke
// lands through paint::apply as any clone does (opacity, selection, locks).
//
// SPOT HEALING has no source point: spot_offset() tries eight directions just
// clear of the stroke and keeps the one whose pixels best match what the
// surroundings predict there (so it doesn't borrow another blemish or an edge).
#include "core/Document.hpp"
#include "core/Paint.hpp"

#include <functional>
#include <optional>

namespace klimt::core::heal {

// The healing source over the stroke: `ref` (the layer as the stroke began,
// or the flattened picture -- RGB / Gray) sampled at (x + dx, y + dy), its
// smooth part swapped for the destination's; transparent where `cov` (the
// stroke's coverage, paint::new_coverage's format) is 0. nullopt: cancelled,
// or a format it can't heal (CMYK / Lab).
std::optional<pixel::TileMap> healing_source(const pixel::TileMap& ref, int dx, int dy, const pixel::TileMap& cov,
                                             double diameter, const std::function<bool()>& cancelled = {});

// Spot healing: where to take from (an offset), or nullopt when nothing fits
// (a stroke too big for the picture).
struct Offset { int dx = 0, dy = 0; };
std::optional<Offset> spot_offset(const pixel::TileMap& ref, const pixel::TileMap& cov, double diameter);

// The tiles a coverage map holds (what a heal recomputes).
TileSet covered(const Image& im, const pixel::TileMap& cov);

// A whole Healing Brush stroke (source offset dx, dy) or Spot Healing stroke
// (spot = true: the offset found), synchronously, as ONE step -- the verb
// table and tests; the app does the same on workers (Shell_paint.cpp).
// `all`: sample what the eye sees (the flattened picture). False when refused
// or nothing fits.
bool stroke(DocJournal& j, Document& d, const Document::Id& id, const std::vector<paint::Dab>& path, paint::Brush b,
            bool spot, int dx, int dy, bool all = false);

}  // namespace klimt::core::heal
