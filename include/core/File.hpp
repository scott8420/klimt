#pragma once
// core::File -- documents in and out of files (s007). GTK-free; runs on a
// worker (every function here is pure over its arguments: a snapshot in, a
// file or a Contents out), so the UI never waits on a disk.
//
// THE NATIVE FORMAT, `.klimt` (Scott, s006 -> s007, question 1: a zip, like
// OpenRaster). docs/file-format.md is the page; in short:
//
//   mimetype         "image/x-klimt", STORED and FIRST (sniffable at byte 38)
//   document.json    the image (size, depth, mode, alpha, dpi) and the layer
//                    tree -- every field of every layer, by id -- and each
//                    layer's tiles: [col, row, "tiles/N"] or [col, row, "u:<hex>"]
//   tiles/N          one tile's stored bytes (256 x 256 x bytes-per-pixel,
//                    little-endian), deflated. Tiles SHARED between layers
//                    (a duplicate) are written once and named twice.
//   profile.icc      the colour profile, when there is one
//   mergedimage.png  s049: the picture, 8-bit, at most 1024 a side (a preview)
//   Thumbnails/thumbnail.png   s049: the picture at 256 (the file manager's)
//
// A uniform tile (one colour) is its pixel in hex in the JSON -- no file. An
// absent tile is transparent and not mentioned. LZ4 comes with the memory
// budget (docs/pixels.md 7).
#include "core/Document.hpp"
#include "formats/Formats.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::file {

constexpr int kVersion = 1;   // document.json's "klimt": a newer one is refused
// s049: the preview a .klimt carries -- `mergedimage.png`, the composite at
// most this many px a side (the file manager's largest thumbnail is 1024),
// and `Thumbnails/thumbnail.png` at 256. Written on every save; never read.
constexpr int kPreviewSide = 1024;

// A whole document: what Document::replace takes, what a save writes.
struct Contents {
    Image                     image;
    std::map<Document::Id, Layer> layers;
    std::map<Document::Id, Layer> channels;   // s016: the saved selections
    std::shared_ptr<const pixel::TileMap> selection;   // s026: the selection as it was (nullptr: none)
    int                       next_id = 1;
    formats::Kind             kind = formats::Kind::Unknown;   // what it was opened from
    std::vector<std::string>  notes;                           // "first page of 3"
};

// Open anything Klimt reads: a .klimt, an OpenRaster (s049, core/Ora.hpp),
// or a PNG / JPEG / TIFF / WebP as one layer named "Background". The tree is checked (Document::check) before it is
// handed back; a file that breaks it is refused, in words.
std::optional<Contents> open(const std::string& path, std::string& error, const formats::Progress& p = {});

// Write a .klimt. To `path + ".partial"` first, renamed over `path` only when
// complete -- a failure or a cancel leaves the old file as it was.
bool save_klimt(const std::string& path, const Contents& c, std::string& error, const formats::Progress& p = {});

// File > New.
enum class Background { White, Black, Transparent, Colour };
struct NewSpec {
    int           width = 1920, height = 1080;
    pixel::Format format{pixel::Depth::U8, pixel::Mode::RGB, true};
    double        dpi = 72;
    Background    background = Background::White;
    pixel::Px     colour = pixel::rgba(1, 1, 1);   // Colour: straight, in the mode's channels
};
// "" when the spec is fine, else why not (for the dialog).
std::string check(const NewSpec& s);
Contents make_new(const NewSpec& s);

// Export: the composite (what the eye sees) as one flat image, written as a
// PNG, JPEG, TIFF or WebP (s049). RGB only for now (the compositor is RGB); the depth is
// the document's (PNG / JPEG cap it); alpha kept unless the file can't.
std::optional<pixel::TileMap> flatten(const Image& im, const std::map<Document::Id, Layer>& layers,
                                      const formats::Progress& p = {});
bool export_image(const std::string& path, formats::Kind kind, const Image& im,
                  const std::map<Document::Id, Layer>& layers, std::string& error,
                  const formats::Progress& p = {}, const formats::WriteOptions& o = {});

}  // namespace klimt::core::file
