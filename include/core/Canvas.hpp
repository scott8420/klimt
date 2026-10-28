#pragma once
// core::Canvas -- Crop, Canvas Size and Image Size (s009 m2). GTK-free;
// docs/canvas.md.
//
// All three change the IMAGE (kImageId) and every layer's pixels, and (Canvas
// Size, Image Size) the selection, as ONE step -- so they undo like anything
// else. Crop deselects (Photoshop's).
//
// Two halves, like Selection:
//   * PURE: reframed (a map on a bigger / smaller / shifted canvas -- Crop and
//     Canvas Size are this), resampled (Image Size), and the plans that apply
//     them to a snapshot -- a worker runs these;
//   * land(): writes a plan through the journal, refused if the document
//     changed while the worker ran. The synchronous verbs (crop, canvas_size,
//     image_size) are plan + land, for the verb table and tests.
#include "core/Document.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::canvas {

using Id = Document::Id;
using MapPtr = std::shared_ptr<const pixel::TileMap>;

// `src` on a w x h canvas, its (0, 0) at (ox, oy): pixel (x, y) of the result
// is src(x - ox, y - oy) -- src's pixels beyond its image included (s010);
// where src holds nothing there, `fill` (transparent outside an image with
// alpha). With keep_outside, whatever lands beyond the new image is KEPT as
// outside tiles; without, it is gone (a mask; "Delete Cropped Pixels").
// Bytes copied, not decoded; a whole tile of one value stays uniform.
pixel::TileMap reframed(const pixel::TileMap& src, int w, int h, int ox, int oy, const pixel::Px& fill,
                        bool keep_outside = true);

// How Image Size resamples. Smooth: bicubic (Mitchell), area-correct when it
// shrinks; Nearest: hard pixels (pixel art, masks you want hard).
enum class Filter { Smooth, Nearest };
const char* filter_name(Filter f);
// `src` at w x h. Premultiplied (no dark fringes at transparent edges).
// nullopt when cancelled; `progress` gets 0..1.
std::optional<pixel::TileMap> resampled(const pixel::TileMap& src, int w, int h, Filter f,
                                        const std::function<bool()>& cancelled = {},
                                        const std::function<void(double)>& progress = {});

// s010: a LAYER resampled -- its pixels beyond the image too, scaled with
// it and kept beyond the new image.
std::optional<pixel::TileMap> resampled_layer(const pixel::TileMap& src, int w, int h, Filter f,
                                              const std::function<bool()>& cancelled = {},
                                              const std::function<void(double)>& progress = {});
// s010: the exact bounds of everything a layer holds that isn't transparent,
// in image pixels (may reach past the image either way). Empty: nothing.
pixel::Rect content_bounds(const pixel::TileMap& m);

// Where Canvas Size keeps the old image: 0 left / top, 1 centre, 2 right / bottom.
int anchor_offset(int old_size, int new_size, int anchor);

// What a document looked like when a plan was made (land() checks it).
// s018 m3: a layer's FILTER MASK rides in Before / Reshape::masks under the
// layer's id with this suffix (its layer mask under the bare id).
inline constexpr const char* kFilterMaskKey = "\x1f" "fx";
inline bool filter_mask_key(const std::string& k) {
    const std::size_t n = std::char_traits<char>::length(kFilterMaskKey);
    return k.size() > n && k.compare(k.size() - n, n, kFilterMaskKey) == 0;
}

struct Before {
    std::shared_ptr<const Image> image;
    std::map<Id, MapPtr>         pixels;   // every pixel layer's
    MapPtr                       mask;
    std::map<Id, MapPtr>         masks;    // s012: every layer mask (the layers that have one)
};
Before before(const Document& d);

// A plan: the new image, every pixel layer's new pixels, and the selection's.
struct Reshape {
    std::shared_ptr<const Image> image;
    std::map<Id, MapPtr>         pixels;
    MapPtr                       mask;     // nullptr: nothing selected after
    std::map<Id, MapPtr>         masks;    // s012: each layer mask, reshaped like its layer
};
// Crop to `r` (image pixels; may reach past the canvas -- that adds canvas).
// s010: what falls outside is KEPT beyond the new image unless delete_cropped.
std::optional<Reshape> crop_plan(const Before& b, const pixel::Rect& r, const pixel::Px& background,
                                 bool delete_cropped = false);
// A w x h canvas, the old image placed by the anchors (0..2 each).
std::optional<Reshape> canvas_plan(const Before& b, int w, int h, int anchor_x, int anchor_y, const pixel::Px& background);
// Resample to w x h at `dpi` (w, h the same: only the resolution changes).
std::optional<Reshape> size_plan(const Before& b, int w, int h, double dpi, Filter f,
                                 const std::function<bool()>& cancelled = {},
                                 const std::function<void(double)>& progress = {});
// The exact bounds of a mask's selected pixels (coverage > 0): Crop to
// Selection's rectangle. Empty Rect when nothing is selected.
pixel::Rect bounds(const pixel::TileMap& mask);

// s010: Image > Reveal All -- the canvas grows to show every pixel the
// layers hold beyond it (nullopt: nothing is beyond). The selection stays on
// its pixels.
std::optional<Reshape> reveal_plan(const Before& b);

// Is this a size we'll make? "" yes, else why not.
std::string size_refusal(int w, int h);

// Write a plan as ONE step `label`. Refused (false) if the document is no
// longer `expect` (a layer's pixels, the image or the selection changed).
bool land(DocJournal& j, Document& d, const Before& expect, const Reshape& r, const std::string& label);

// ── the verbs, synchronous ──────────────────────────────────────────────────
bool crop(DocJournal& j, Document& d, const pixel::Rect& r, const pixel::Px& background, bool delete_cropped = false);
bool canvas_size(DocJournal& j, Document& d, int w, int h, int anchor_x, int anchor_y, const pixel::Px& background);
bool image_size(DocJournal& j, Document& d, int w, int h, double dpi, Filter f = Filter::Smooth);
bool reveal_all(DocJournal& j, Document& d);

}  // namespace klimt::core::canvas
