#pragma once
// core::clipboard -- CUT, COPY, COPY MERGED, PASTE, PASTE IN PLACE (s096;
// docs/clipboard.md). GTK-free: the Shell carries a Piece to and from the
// system clipboard (as straight 8-bit RGBA, what other apps read).
//
//   Copy         the picked layer's pixels inside the selection (the whole
//                layer when nothing is selected), a soft edge's coverage
//                kept, cut to what isn't clear.
//   Copy Merged  the same from what you see (every shown layer).
//   Cut          Copy, then the selected pixels cleared (a selection needed).
//   Paste        a NEW pixel layer "Pasted" above the picked one -- centred
//                on the canvas; Paste in Place: where it was copied from.
//
// A piece keeps its depth and mode while it stays in Klimt; one from another
// app comes in as 8-bit RGB. Pasted into a picture of another format it is
// converted (depth; RGB <-> Gray) -- CMYK and Lab pictures take their own only.
#include "core/Document.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::clipboard {

using Id = Document::Id;

struct Piece {
    std::shared_ptr<const pixel::TileMap> pixels;   // w x h, with alpha
    int x = 0, y = 0;                               // its top-left where it was copied from
    int width() const { return pixels ? pixels->width() : 0; }
    int height() const { return pixels ? pixels->height() : 0; }
};

// What Copy reads: the selection's bounds, else the whole picture.
pixel::Rect area(const Document& d);
// `src`'s pixels in `r` through the selection `sel` (nullptr: all), cut to
// what isn't clear. nullopt: nothing there, or cancelled. Worker-safe.
std::optional<Piece> cut_out(const pixel::TileMap& src, const pixel::TileMap* sel, const pixel::Rect& r,
                             const std::function<bool()>& cancelled = {});
// As straight 8-bit RGBA rows (width * 4 bytes each) for other apps. Empty
// for CMYK / Lab (kept inside Klimt only).
std::vector<std::uint8_t> rgba8(const Piece& p);
// A picture from another app: straight 8-bit RGBA rows.
Piece from_rgba8(const std::uint8_t* data, int w, int h, int stride);

// "" when `p` can be pasted into `d`, else why not.
std::string refusal(const Document& d, const Piece& p);
// The new layer's pixels -- the piece in the picture's format, placed
// (centred, or in place). Worker-safe. nullopt: refused.
std::optional<std::shared_ptr<const pixel::TileMap>> placed(const Image& im, const Piece& p, bool in_place);
// ONE step "Paste" / "Paste in Place": a new pixel layer "Pasted" above
// `above`, holding `px` (from placed()). The new layer ("" refused).
Id land(DocJournal& j, Document& d, std::shared_ptr<const pixel::TileMap> px, const Id& above, bool in_place);
// placed + land, for the tests.
Id paste(DocJournal& j, Document& d, const Piece& p, const Id& above, bool in_place);

}  // namespace klimt::core::clipboard
