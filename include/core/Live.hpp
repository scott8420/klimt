#pragma once
// core::live -- LIVE FILTERS (s018), Photoshop's smart filters: a stack of
// filters under a pixel layer, each on or off, editable later, in an order
// you choose; the layer's pixels never change. docs/filters.md.
//
// THE CACHE. A filter reads its neighbours, so it can't run per tile inside
// the compositor. Instead composite_tile asks effective() for the layers as
// they LOOK: each layer with live filters gets its pixels through its stack,
// worked out ONCE for the whole layer and kept (keyed by the pixels' pointer
// and the stack) -- tiles that ask at once wait for the one computation. So
// the canvas, export, the histogram, the eyedropper and "All layers" sampling
// all see the filtered layer with no change of their own.
#include "core/Composite.hpp"   // Layers
#include "core/Document.hpp"

#include <functional>
#include <optional>

namespace klimt::core::live {

// The stack runs here: `src` through each filter that is on, in order. With
// `only`, just enough for that rectangle to be exact (each stage widened by
// the reach of the stages after it) -- a dialog's preview.
std::optional<pixel::TileMap> run(const pixel::TileMap& src, const std::vector<LiveFilter>& stack,
                                  const pixel::Rect* only = nullptr, const std::function<bool()>& cancelled = {});

bool active(const Layer& l);              // a pixel layer with pixels and a filter on
bool any(const Layers& ls);               // any layer active
// The layer's pixels as they look (cached; computed here when not yet).
std::shared_ptr<const pixel::TileMap> look(const Layer& l);
// The layers as they look: each active layer's pixels through its stack, its
// stack then empty (so nothing runs twice).
Layers effective(const Layers& ls);
// m3: the filtered pixels with the raw ones back where `hide` hides the filters.
pixel::TileMap masked(const pixel::TileMap& raw, const pixel::TileMap& filtered, const pixel::TileMap& hide);
// How far an active layer's look can reach past its pixels (the stack's reach).
int reach(const Layer& l);
void clear_cache();                       // tests

}  // namespace klimt::core::live

namespace klimt::core::layers {
// s018 -- the stack's verbs, each ONE step; refused on a group / an
// adjustment layer, under an All or Pixels lock.
bool add_live(DocJournal& j, Document& d, const Id& id, const Filter& f);
bool set_live(DocJournal& j, Document& d, const Id& id, std::size_t i, const Filter& f);
bool toggle_live(DocJournal& j, Document& d, const Id& id, std::size_t i);
bool remove_live(DocJournal& j, Document& d, const Id& id, std::size_t i);
bool move_live(DocJournal& j, Document& d, const Id& id, std::size_t from, std::size_t to);
// The stack baked into the pixels (Photoshop's Rasterize): the look becomes
// the pixels, the stack goes.
bool bake_live(DocJournal& j, Document& d, const Id& id);
// m3: the FILTER MASK set (nullptr: none) -- one step; refused without live
// filters, or a map not the selection's format and the image's size.
bool set_filter_mask(DocJournal& j, Document& d, const Id& id, std::shared_ptr<const pixel::TileMap> m,
                     const std::string& label = "Filter Mask");
}  // namespace klimt::core::layers
