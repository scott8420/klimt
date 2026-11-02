#pragma once
// core::smart::Smart -- what a SMART LAYER holds (s082; docs/smart.md): the
// placed picture whole, at its own size, and the map that lays it on the
// canvas. The layer's pixels are always this, drawn fresh -- so scaling it
// down and back up loses nothing, and its live filters run on it as on any
// layer. Pure value, shared and compared by pointer like a layer's pixels.
#include "pixel/Pixel.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace klimt::core::smart {

struct Smart {
    // The picture, in the DOCUMENT's format (converted when placed), at its
    // own width and height.
    std::shared_ptr<const pixel::TileMap> source;
    std::string   name;            // "beach.jpg" -- what it came from
    // LINKED: the file it was read from (absolute), its size and its last
    // change (ns since the epoch) when read -- Update reads it again when
    // they differ. "" : EMBEDDED (the .klimt carries it).
    std::string   link;
    std::int64_t  stamp = 0;
    std::uint64_t bytes = 0;
    double        dpi = 72;        // the source's own resolution (said, not used to draw)
    // Source pixels -> image pixels: a projective map, row-major 3 x 3 (as
    // xform::Projective) -- Free Transform's scale, turn, skew, distort and
    // perspective all compose into it.
    std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
    bool operator==(const Smart&) const = default;
};

}  // namespace klimt::core::smart
