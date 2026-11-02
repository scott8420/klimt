#pragma once
// core::roundtrip -- THE COMPOSIX ROUND-TRIP (s083; REQUIREMENTS "Added" 9;
// docs/round-trip.md). "Edit Original" in Composix opens a placed picture
// here; Save writes it back, in the format Composix placed, and Composix sees
// the file change.
//
// A PDF carries the KLIMT DOCUMENT inside it (Illustrator's trick): Klimt
// draws the page as ever (core/Pdf.hpp -- bands and live type), then appends
// ONE incremental update holding the .klimt whole, as a private stream named
// from the update's trailer (/KlimtDocument N 0 R). A PDF reader, Composix's
// poppler and qpdf see the page and nothing else (the stream isn't on the
// page, so a placed page never carries it into Composix's output); Klimt
// opening that PDF opens the document inside -- every layer, live type, smart
// layers, paths -- not a picture of it. A PDF Klimt didn't write (or one
// rewritten without the stream) is refused in words: Klimt doesn't read PDF
// pages as pictures.
//
// The other formats are written as Export writes them: PSD and OpenRaster
// with their layers, TIFF / PNG / JPEG / WebP as the one picture (said, when
// there was more than one layer). Every write is atomic: a failure or a
// cancel leaves the file Composix placed as it was. GTK-free; on a worker.
#include "core/File.hpp"
#include "core/SmartSpec.hpp"
#include "formats/Formats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::roundtrip {

// ── The Klimt document inside a PDF ─────────────────────────────────────────
// Append the .klimt bytes to the PDF at `pdf` as an incremental update (its
// cross-reference in the file's own style -- a stream after a stream, a
// table after a table). False, with `error`: not a PDF whose trailer can be
// found, or the write failed (the file is then as it was).
bool embed(const std::string& pdf, const std::vector<std::uint8_t>& klimt, std::string& error);
// The .klimt bytes a PDF carries (nullopt: none -- not Klimt's, or rewritten
// without them). Reads the last trailer's /KlimtDocument, else the last
// stream that says it is one; a FlateDecode'd copy (qpdf compresses streams
// when it rewrites) is inflated.
std::optional<std::vector<std::uint8_t>> embedded(const std::string& pdf);
// Opening a PDF: the document it carries, `kind` PDF, the note "Klimt's
// layers, kept in the PDF". nullopt with `error` in words otherwise.
std::optional<file::Contents> open_pdf(const std::string& path, std::string& error, const formats::Progress& p = {});

// ── Writing back ───────────────────────────────────────────────────────────
// What Save writes for a file of `kind`: nullptr when Klimt can't write it
// (then the tab saves as a .klimt). A label for the toast / tab ("PDF").
const char* writes_back(formats::Kind kind);
// The kind a file is (by its bytes; a PDF / EPS by its first line).
formats::Kind kind_of(const std::string& path);
// Write the document to `path` as `kind`. `notes` hears what it lost, in
// words ("flattened -- a TIFF holds one picture"). `o`: the format's options
// (JPEG quality ...). PDF: the page and the document inside it.
bool save_back(const std::string& path, formats::Kind kind, const file::Contents& c, std::string& error,
               std::vector<std::string>& notes, const formats::Progress& p = {}, const formats::WriteOptions& o = {});

// ── Edit Contents (a smart layer's picture in a tab of its own) ────────────
// The picture as a document: its own size, the owner's format, profile and
// the picture's resolution; one layer named for it ("beach.jpg"). Saving
// puts what it draws back into the smart layer (Layer > Smart Layer > Edit
// Contents; the step "Edit Contents" there).
file::Contents contents_of(const smart::Smart& s, const Image& owner);
// What a document draws, as a picture in `f` -- what goes back into the
// smart layer. nullopt: cancelled.
std::optional<pixel::TileMap> picture_of(const file::Contents& c, const pixel::Format& f, const formats::Progress& p = {});

// ── Where a tab came from ──────────────────────────────────────────────────
// The open request's hint (the D-Bus Open's, or `klimt --composix FILE`).
inline constexpr const char* kComposixHint = "composix";

}  // namespace klimt::core::roundtrip
