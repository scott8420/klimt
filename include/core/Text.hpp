#pragma once
// core::Text -- TEXT LAYERS (s046; docs/text.md). A text layer is Kind::Text:
// its `text` is a text::Spec (libs/text) and its `pixels` are that Spec set at
// the image's size, in the layer's colour. Every change to the Spec sets the
// pixels again IN THE SAME STEP, so everything that reads pixels -- the
// compositor, layer styles, masks, clipping, export, the Layers thumbnail --
// sees type exactly as it sees a pixel layer, and nothing needs a second path.
// Painting, filters and adjustments refuse it (not_pixels says why): the user
// rasterizes first, by choice -- never forced (RULES, the scope wall).
//
// Every verb goes through the journal (the one door) and is in
// layers::verbs(). Typing on the canvas is a GESTURE: text_begin, any number
// of set_text, text_end -- ONE step ("Type"); a new layer that ends empty
// leaves nothing.
#include "core/Document.hpp"

#include <memory>
#include <string>

namespace klimt::core {

// The Spec set as pixels at the image's size and format (nullptr: nothing on
// the image -- empty text, or all of it off the canvas). Pure; any thread.
std::shared_ptr<const pixel::TileMap> text_pixels(const Image& im, const text::Spec& s);

// The name a text layer takes from its text: the first line, trimmed, at most
// 30 characters ("Text" when empty) -- Photoshop's way. A layer still carrying
// its text's name follows the text; one the user renamed keeps its name.
std::string text_name(const std::string& text);

namespace layers {

// A new text layer above `above` (panel order; the selected layer) -- set,
// named from its text. Empty text is allowed (a layer the user is about to
// type into). Returns its id, or "" when the Spec is refused.
Id   add_text(DocJournal& j, Document& d, const text::Spec& s, const Id& above = {}, const std::string& label = "New Text Layer");
// The Spec replaced, pixels set again. Refused: not a text layer, an All
// lock, a Spec text::refusal refuses, or no change. Presses inside the merge
// window with the same key fold (the strip's fields).
bool set_text(DocJournal& j, Document& d, const Id& id, const text::Spec& s,
              const std::string& label = "Edit Type", const std::string& merge_key = {});
// The same change to several text layers' style at once (the strip with many
// selected): `f` edits each Spec; layers it leaves unchanged are skipped.
bool set_text_on(DocJournal& j, Document& d, const std::vector<Id>& ids,
                 const std::function<void(text::Spec&)>& f, const std::string& label, const std::string& merge_key = {});
// Layer > Rasterize Type: a pixel layer of what it shows (its pixels, mask,
// styles, blend, opacity kept). Refused on anything but a text layer.
bool rasterize_text(DocJournal& j, Document& d, const Id& id);
// The canvas editor's gesture: one step for a whole editing visit.
void text_begin(DocJournal& j, const Document& d, const Id& id);
// Ends it. A layer made in this visit whose text is still empty is removed
// (the whole visit leaves nothing). Returns whether a step was kept.
bool text_end(DocJournal& j, Document& d, const Id& id, bool made_here);

}  // namespace layers
}  // namespace klimt::core
