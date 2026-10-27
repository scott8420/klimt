#pragma once
// core::Composite -- the layer tree flattened to what the eye sees (s006).
// Runs on a WORKER from a snapshot (Document::snapshot() + image(): values
// and shared, immutable tile maps -- nothing the UI can change under it).
// Per 256 x 256 tile, bottom to top: visibility; groups (Pass Through blends
// its contents straight into what's below; any other mode composites them
// alone, then blends the result); opacity x fill; clipping (a clipped layer
// shows only where its base has pixels, and not at all if the base is
// hidden); the blend modes of libs/pixel. RGB documents; other modes wait on
// colour management. Layer styles, masks and adjustments come later.
#include "core/Document.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace klimt::core {

using Layers = std::map<Document::Id, Layer>;

// One tile of the composite: PREMULTIPLIED float RGBA, kTile * kTile * 4.
void composite_tile(const Image& im, const Layers& layers, int col, int row, std::vector<float>& out);

// The whole image for the screen: Cairo's ARGB32 (premultiplied, one
// native-endian uint32 per pixel, rows of width). `cancelled` is asked
// between tiles; `progress` told after each. Returns false if cancelled.
bool composite_argb(const Image& im, const Layers& layers, std::vector<std::uint32_t>& out,
                    const std::function<bool()>& cancelled = {},
                    const std::function<void(double)>& progress = {});

}  // namespace klimt::core
