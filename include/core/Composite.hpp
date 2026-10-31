#pragma once
// core::Composite -- the layer tree flattened to what the eye sees (s006).
// Runs on a WORKER from a snapshot (Document::snapshot() + image(): values
// and shared, immutable tile maps -- nothing the UI can change under it).
// Per 256 x 256 tile, bottom to top: visibility; groups (Pass Through blends
// its contents straight into what's below; any other mode composites them
// alone, then blends the result); opacity x fill; clipping (a clipped layer
// shows only where its base has pixels, and not at all if the base is
// hidden); the blend modes of libs/pixel. RGB documents; other modes wait on
// colour management. s012: layer masks; s013: adjustment layers (applied
// to what is below them, in their group). Layer styles come later.
#include "core/Document.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

// s008: an sRGB colour (the colour pair's) as a pixel of a document's mode
// -- unmanaged, the inverse of how the compositor shows each mode (gray =
// luminance; CMYK = naive with full black generation; Lab via D50). Alpha 1.
pixel::Px from_srgb(const pixel::Format& f, float r, float g, float b);

// s008 m3: the colour the eye sees at (x, y) -- the composite, unpremultiplied,
// as encoded sRGB 0..1 (what the Eyedropper picks). Outside / transparent:
// alpha 0. `radius` 1 = 3 x 3, 2 = 5 x 5 (an average, premultiplied).
struct Sampled { float r = 0, g = 0, b = 0, a = 0; };
Sampled sample_composite(const Image& im, const Layers& layers, int x, int y, int radius = 0);

// ── s007 m4: dirty tiles ─────────────────────────────────────────────────────
// Which tiles of the composite can differ between two snapshots of a
// document -- so a change re-composites those tiles, not the whole image.
// Conservative (a tile it names may turn out unchanged; a tile it doesn't
// name is CERTAIN not to have changed -- the selftest holds it to that):
//   * a layer whose pixels alone changed: the tiles whose tile pointers
//     differ (immutable tiles: same pointer, same pixels);
//   * a rename or a lock: nothing (the eye can't see it);
//   * anything else (visible, opacity, fill, blend, clip, place, kind, a
//     layer made or deleted): every tile the layer held before or holds now,
//     its contents' (a group), and its clipped siblings' (a base that moves
//     changes what they clip to);
//   * a different image size or format: everything.
struct TileSet {
    int               cols = 0, rows = 0;
    bool              all = false;
    std::vector<char> on;                 // cols * rows, row-major
    bool has(int col, int row) const {
        return all || on[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(col)];
    }
    std::size_t count() const;
    void add(int col, int row);
    void add_all() { all = true; }
    void merge(const TileSet& o);         // same grid; else: all
};
TileSet tiles_of(const Image& im);       // the empty set on the image's grid
TileSet changed_tiles(const Image& before_im, const Layers& before, const Image& after_im, const Layers& after);

// s013: the layers as they are BELOW `id` -- `id` and everything painted
// after it (its later siblings, and its groups' later siblings, up to the
// top) hidden. What an adjustment layer's histogram is made from.
Layers below(const Layers& all, const Document::Id& id);
// s013: the histogram of the composite of `layers` -- every tile when there
// are few, else an even spread of `max_tiles` of them.
adjust::Histogram histogram_of(const Image& im, const Layers& layers, int max_tiles = 64,
                               const std::function<bool()>& cancelled = {});

// One tile of the composite for the screen: Cairo ARGB32 (premultiplied,
// native-endian), `w` x `h` = the tile's part inside the image (an edge
// tile is smaller), rows of w.
// s010 m4: a tile's picture at half size -- each output pixel the mean of a
// 2 x 2 block (Cairo's ARGB32 is premultiplied, so a plain mean is right).
// w x h in, ceil(w / 2) x ceil(h / 2) out. The canvas's mip levels.
void argb_half(const std::vector<std::uint32_t>& in, int w, int h, std::vector<std::uint32_t>& out, int& ow, int& oh);
// s012: a layer mask as a picture -- white where it shows, black where it
// hides (it is stored as what it hides). Opaque ARGB, like composite_tile_argb.
void mask_tile_argb(const pixel::TileMap& mask, int col, int row, std::vector<std::uint32_t>& out, int& w, int& h);
void composite_tile_argb(const Image& im, const Layers& layers, int col, int row,
                         std::vector<std::uint32_t>& out, int& w, int& h);

// ── s046: THE VIEW PYRAMID (WEAKNESSES 13; docs/speed.md) ───────────────────
// Zoomed out, a whole document is owed before anything shows: every tile
// composited at full size. A PREVIEW goes first -- the document at 1/2^k,
// each layer's pixels (masks too) box-averaged to that size, composited
// whole, cut into the canvas's tiles at mip level k -- and the full tiles
// replace it as they land. A preview is a likeness, not the picture: layer
// styles, live filters and filter layers work in pixels and are drawn at
// their full-size settings on the reduced pixels.
//
// A map at 1/2^k (k 1..6): each output pixel the PREMULTIPLIED mean of the
// f x f block it covers (f = 2^k), counting only pixels inside the map --
// so an edge is as solid as its pixels. ceil(w / f) x ceil(h / f), the same
// format; nothing beyond the image (outside tiles) is kept.
pixel::TileMap reduced_map(const pixel::TileMap& m, int k);
// Reductions already made, by the map they came from (immutable maps: the
// same pointer is the same pixels). Thread-safe; prune() drops what `keep`
// no longer holds. The Shell keeps one, so an unchanged layer is reduced once.
class ReduceCache {
public:
    std::shared_ptr<const pixel::TileMap> get(const std::shared_ptr<const pixel::TileMap>& m, int k);
    void prune(const Layers& keep);
    std::size_t size() const;
private:
    struct Entry { std::shared_ptr<const pixel::TileMap> from, to; };
    mutable std::mutex m_mu;
    std::map<std::pair<const pixel::TileMap*, int>, Entry> m_map;
};
// The document at 1/2^k: the image's size divided (rounded up), every
// layer's pixels, mask and filter mask reduced (through `cache` when given),
// path layers' outlines scaled. `cancelled` asked between layers; nullopt
// when it says so.
struct Reduced { Image image; Layers layers; };
std::optional<Reduced> reduced(const Image& im, const Layers& layers, int k, ReduceCache* cache = nullptr,
                               const std::function<bool()>& cancelled = {});
// The reduced composite as Cairo ARGB32 (premultiplied), rows of
// r.image.width -- what the canvas cuts into preview tiles.
bool composite_reduced_argb(const Reduced& r, std::vector<std::uint32_t>& out, const std::function<bool()>& cancelled = {});

}  // namespace klimt::core
