#pragma once
// core::ora -- OpenRaster in and out (s049; WEAKNESSES 20; docs/file-format.md
// "OpenRaster"). Krita's, GIMP's and MyPaint's layered exchange format: a zip
// whose first entry is "mimetype" = "image/openraster" (stored), a
// `stack.xml` naming the layers top first, one PNG per layer under `data/`,
// a full-size `mergedimage.png` and a `Thumbnails/thumbnail.png` (256 px).
//
// What crosses: pixel and text layers (as pixels), groups (nested stacks),
// names, visibility, opacity, blend modes, a layer's place (x, y). What ORA
// can't hold is BAKED where the look survives -- a layer's mask, live
// filters, styles and Fill go into its PNG -- and otherwise LEFT OUT with a
// note: adjustment and filter layers, path layers, clipping, a group's mask
// and styles. Non-RGB documents are written as RGB; 16-bit and 32f as
// 16-bit PNGs. Pure over its arguments: runs on a worker.
#include "core/File.hpp"

#include <string>
#include <vector>

namespace klimt::core::ora {

// Open an .ora as a document (RGB, alpha; 8-bit, or 16-bit when any layer's
// PNG is). Layers past the canvas are cut at its edge.
std::optional<file::Contents> open(const std::string& path, std::string& error, const formats::Progress& p = {});

// Write an .ora (atomically, as save_klimt). `notes` hears what didn't
// cross ("2 adjustment layers left out").
bool save(const std::string& path, const Image& im, const std::map<Document::Id, Layer>& layers,
          std::string& error, std::vector<std::string>& notes, const formats::Progress& p = {});

// s050: what an Export as .ora will do, said BEFORE it writes (the Export
// dialog): `baked` -- looks drawn into a layer's pixels ("2 masks"),
// `left_out` -- what doesn't cross at all (the same lines save() puts in
// `notes`, so the dialog and the toast never disagree).
struct Plan {
    std::vector<std::string> baked, left_out;
};
Plan plan(const Image& im, const std::map<Document::Id, Layer>& layers);

// ORA's composite-op for a Klimt blend mode ("svg:multiply"; a mode SVG
// lacks as Krita writes it, "krita:linear_burn"), and back -- any "svg:" /
// "krita:" / "klimt:" name, spaces and underscores as hyphens. Unknown:
// nullopt (the reader then uses Normal and says so).
std::string             composite_op(Blend b);
std::optional<Blend>    blend_of_op(const std::string& op);

// The picture at most `side` px on its longer side, 8-bit RGBA -- a
// document's thumbnail (ORA's, and the .klimt's for the file manager).
pixel::TileMap thumbnail(const pixel::TileMap& flat, int side = 256);

// A whole map as one PNG in memory, at its own depth (32f as 16; zlib `level`).
bool png_bytes(const pixel::TileMap& m, std::vector<std::uint8_t>& out, std::string& error,
               const formats::Progress& p = {}, int level = 6, double dpi = 0);

}  // namespace klimt::core::ora
