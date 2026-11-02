#pragma once
// core::clip -- the CLIPPING PATH (s080; REQUIREMENTS "Clipping paths and
// EPS"; docs/paths.md "Clipping path").
//
// One path layer of the document may be marked as its clipping path: where
// the picture is PLACED (InDesign, Composix, QuarkXPress), only what's inside
// the path shows. Klimt itself draws nothing differently -- it is a word to
// the page-layout program, carried three ways:
//   - EPS (core/Eps.hpp): the path as PostScript's clip, round the image;
//   - TIFF and PSD: Photoshop's PATH RESOURCES (an 8BIM block 2000 + n for
//     each path layer, named by it, and 2999 naming the clipping path and its
//     flatness) -- InDesign's "Photoshop Path" / "Use Photoshop clipping
//     path" read them; Klimt reads them back (open: path layers).
// The mark lives on the image record (Image::clip_path, clip_flatness), so
// it is ONE step and the .klimt keeps it. A deleted path layer leaves the
// mark inert (of() says ""); undoing the delete brings it back.
//
// FLATNESS: how far (device pixels) the printer may cut a curve's corner
// with straight lines -- 0 lets the printer pick; 8 - 10 for a high-res
// press, 1 - 3 for a desktop printer (Photoshop's advice). Pure, GTK-free.
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Path.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::clip {

inline constexpr double kMaxFlatness = 100;

// The clipping path's layer: Image::clip_path when it names a path layer with
// an outline; "" otherwise.
Document::Id of(const Image& im, const std::map<Document::Id, Layer>& layers);
Document::Id of(const Document& d);

// Mark `id` as the clipping path (or "" for none) at `flatness` (clamped to
// 0 .. kMaxFlatness, a tenth of a pixel) -- ONE step, "Clipping Path" / "No
// Clipping Path". Refused: `id` not a path layer, or an empty one; no change.
bool set(DocJournal& j, Document& d, const Document::Id& id, double flatness);

// ── Photoshop's path resources (Photoshop File Formats, "Path resource
//    format") ──────────────────────────────────────────────────────────────
// A path is 26-byte records: a fill-rule record (6), the initial fill rule
// (8), then each subpath -- a length record (0 closed, 3 open) and its knots
// (1 / 4 smooth -- "linked", 2 / 5 corner): the handle arriving, the anchor,
// the handle leaving, each (vertical, horizontal) as 8.24 fixed fractions of
// the image's height and width.
std::vector<std::uint8_t> encode(const path::Path& p, int width, int height);
// Back; nullopt: not a path resource.
std::optional<path::Path> decode(const std::uint8_t* data, std::size_t n, int width, int height);

struct Named {
    std::string name;
    path::Path  path;
};
struct Resources {
    std::vector<Named> paths;    // in the file's order
    std::string        clip;     // the clipping path's name; "" none
    double             flatness = 0;
};
// The document's path layers (panel order, top first; a hidden one too --
// a path is a word, not a picture) as image-resource blocks ("8BIM", id,
// name, size, data), names made unique ("Path 1", "Path 1 (2)"), and 2999
// for the clipping path. Empty when there's no path layer.
std::vector<std::uint8_t> resources(const Image& im, const std::map<Document::Id, Layer>& layers);
// The paths and the clip in an image-resource section (PSD's, a TIFF's tag
// 34377, EPS's %BeginPhotoshop) -- every other block skipped. Truncated or
// broken blocks end the walk; what came before stands.
Resources parse(const std::uint8_t* data, std::size_t n, int width, int height);
// What parse() found, into an opened file: each path a path layer at the top
// of the stack (above everything, file order kept), the clip marked. Notes
// "2 paths" for the open's toast.
void adopt(file::Contents& c, const Resources& r);

}  // namespace klimt::core::clip
