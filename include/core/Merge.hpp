#pragma once
// core::merge -- MERGE AND FLATTEN (s096; docs/merge.md). Photoshop's four:
//
//   Merge Down     the picked layer into the one just below it, in its group
//                  -- ONE pixel layer in the lower one's place, with its name,
//                  opacity, blend mode and clipping; what the two drew
//                  (masks, styles, live filters, fill, an adjustment or filter
//                  layer's effect on the lower one) baked into its pixels.
//   Merge Visible  every SHOWN layer into one pixel layer "Merged"; hidden
//                  layers (and a group still holding one) stay.
//   Flatten Image  everything into one pixel layer "Background" -- hidden
//                  layers dropped (said). Transparency stays transparent.
//   Stamp Visible  what you see as a NEW pixel layer "Merged" above the
//                  picked one; nothing else changes.
//
// The pixels are the compositor's -- what the eye sees -- in the document's
// own format (RGB and Gray documents; CMYK and Lab refused for now). The
// compositing is long, so it splits in two like every job: render() on a
// worker from a snapshot, land() as ONE step on the UI thread (refused when
// the tree no longer allows it). run() does both, for the tests and actions.
// GTK-free.
#include "core/Document.hpp"
#include "formats/Formats.hpp"

#include <memory>
#include <optional>
#include <string>

namespace klimt::core::merge {

using Id = Document::Id;
enum class How { Down, Visible, Flatten, Stamp };
const char* label(How h);   // "Merge Down", "Merge Visible", "Flatten Image", "Stamp Visible"

// "" when `how` can run on `d` (picked layer `picked`), else why not, in words.
std::string refusal(const Document& d, How how, const Id& picked);
// Merge Down: the layer it merges into ("" none).
Id below(const Document& d, const Id& picked);
// What to composite -- a snapshot's worth of records (worker-safe values).
std::map<Id, Layer> source(const Document& d, How how, const Id& picked);
// The composite of `src` in `im`'s format. nullopt: cancelled.
std::optional<std::shared_ptr<const pixel::TileMap>> render(const Image& im, const std::map<Id, Layer>& src,
                                                            const formats::Progress& p = {});
// ONE step. The layer that holds the result ("" refused).
Id land(DocJournal& j, Document& d, How how, const Id& picked, std::shared_ptr<const pixel::TileMap> px);
// render + land.
Id run(DocJournal& j, Document& d, How how, const Id& picked);
// Flatten Image: how many hidden layers it would drop (for the toast).
int hidden_dropped(const Document& d);

}  // namespace klimt::core::merge
