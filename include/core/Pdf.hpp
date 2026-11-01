#pragma once
// core::pdf -- Export as PDF: the picture, with its text layers kept as TYPE
// (s051; Scott, s051: "PDF as our go-to format" -- Composix places it, and so
// does anything else that takes a PDF). docs/file-format.md "PDF".
//
// One page the image's size at its resolution (a 3000 px picture at 300 dpi:
// a 10 in page). The layers are cut into BANDS at each text layer that can
// stay type: the picture below it, the type (glyphs, the fonts embedded as
// subsets), then the picture of what's above it, and so on -- each band
// composited by Klimt (every blend, style, filter, mask) and laid in as an
// image (lossless, 8-bit, its transparency a soft mask).
//
// A text layer stays TYPE only where cutting the stack there leaves the look
// exactly as it is: at the top level of the stack -- or inside plain Pass
// Through groups, opened in place (s055) -- visible, Normal, no
// styles, no mask, not clipped and not a clipping base; and everything drawn
// above it Normal too, with no adjustment / filter layer above it reaching
// down (an isolated group above is fine whatever is inside it). Any other
// text layer is BAKED into its band's picture -- said, with why, in the plan.
// A non-RGB document: one picture (the compositor is RGB). Pure over its
// arguments: runs on a worker.
#include "core/File.hpp"
#include "core/Ora.hpp"

#include <map>
#include <string>
#include <vector>

namespace klimt::core::pdf {

// What will cross, as the Export dialog lists it (ORA's Plan: `kept` --
// type kept as type; `baked` -- type drawn as pixels, and why; `left_out`),
// and the text layers kept as type, bottom first.
struct Plan {
    ora::Plan list;
    std::vector<Document::Id> live;
};
Plan plan(const Image& im, const std::map<Document::Id, Layer>& layers);

// Write the PDF (atomically). `title`: its document title (the file's name).
// `notes` hears the plan's lines for the toast ("2 text layers kept as type").
bool save(const std::string& path, const Image& im, const std::map<Document::Id, Layer>& layers,
          const std::string& title, std::string& error, std::vector<std::string>& notes,
          const formats::Progress& p = {});

}  // namespace klimt::core::pdf
