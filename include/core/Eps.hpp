#pragma once
// core::eps -- Export as EPS (s080; REQUIREMENTS "Clipping paths and EPS";
// docs/file-format.md "EPS"). Our own PostScript writer.
//
// What a page-layout program (InDesign, QuarkXPress, Composix) places: the
// picture as one image -- what the eye sees, flattened, matted with white
// (PostScript has no transparency) -- 8-bit RGB, or CMYK through the
// document's CMYK profile (else Colour Settings' working CMYK, else the plain
// conversion), cut out by the CLIPPING PATH (core/Clip.hpp) when the
// document has one: `eoclip` round the image, the path's flatness `setflat`.
//
// The file: a DOS EPS binary header when it carries a preview (the TIFF a
// layout program shows on screen: 8-bit RGB, uncompressed, at 72 ppi, its
// long side at most 1024), then EPSF-3.0 PostScript, Clean7Bit -- the image
// data ASCII85 over run-length (LanguageLevel 2) or over Flate (3, "ZIP",
// smaller). %BeginPhotoshop carries Photoshop's path resources, so a reader
// that looks for the paths (InDesign's "Photoshop Path") finds them too.
// The page is the picture at its resolution (a 3000 px picture at 300 dpi:
// 10 in, 720 pt). Pure over its arguments: runs on a worker.
#include "core/File.hpp"
#include "core/Ora.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace klimt::core::eps {

// What will cross, said before it writes (the Export dialog): `kept` -- the
// clipping path; `baked` -- the layers flattened; `left_out` -- transparency
// (white behind), the depth.
ora::Plan plan(const Image& im, const std::map<Document::Id, Layer>& layers, const formats::WriteOptions& o = {});

// Write the EPS (atomically). `title`: its %%Title (the file's name).
// `notes` hears what it did for the toast ("CMYK", "clipping path “Path 1”").
bool save(const std::string& path, const Image& im, const std::map<Document::Id, Layer>& layers,
          const std::string& title, const formats::WriteOptions& o, std::string& error,
          std::vector<std::string>& notes, const formats::Progress& p = {});

// ── the pieces (tests) ──────────────────────────────────────────────────────
// PostScript's RunLengthEncode (literal runs 0..127 +1, repeats 129..255 as
// 257 - n, 128 the end).
std::vector<std::uint8_t> run_length(const std::vector<std::uint8_t>& in);
// ASCII85 with its "~>" end, lines at most 76 characters, none starting with
// '%' (a DSC reader takes those for comments) -- a space is put first.
std::string ascii85(const std::vector<std::uint8_t>& in);
// And their decoders -- the selftest reads the files back.
std::vector<std::uint8_t> un_run_length(const std::vector<std::uint8_t>& in);
std::vector<std::uint8_t> un_ascii85(const std::string& in);
// The clipping path as PostScript: moveto / curveto / closepath in points,
// y up from the picture's bottom ("newpath ... eoclip newpath").
std::string clip_ps(const path::Path& p, int height, double dpi);
// An uncompressed little-endian TIFF of 8-bit RGB (the preview).
std::vector<std::uint8_t> tiff_rgb(const std::vector<std::uint8_t>& rgb, int width, int height, double dpi);

}  // namespace klimt::core::eps
