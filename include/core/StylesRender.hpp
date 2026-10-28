#pragma once
// core::styles -- drawing the layer styles (s019). See core/Styles.hpp.
#include "core/Styles.hpp"
#include "pixel/Pixel.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace klimt::core::styles {

// `content` (a layer's pixels as they look, straight, with alpha) with its
// styles drawn: shadows / glow under it, the content at `fill` %, the inner
// effects and the stroke over it. The result grows past the content by the
// effects' reach. A format without alpha comes back as it was.
pixel::TileMap drawn(const pixel::TileMap& content, const Styles& s, int fill);

// How far the styles reach past the content, in pixels.
int reach(const Styles& s);

// Exact Euclidean distance, per pixel, to the nearest `seed` (true) of a w x h
// grid (Felzenszwalb & Huttenlocher) -- the Stroke's band. For the selftest.
std::vector<float> distance(const std::vector<char>& seed, int w, int h);

}  // namespace klimt::core::styles
