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
//
// s021: a new look whose pixels share most tiles with a cached one of the
// same stack (a stroke's next dab) is made IN PART: only the cells the change
// can reach are worked (in a window wide enough that they come out as the
// whole run makes them); the rest are the cached look's tiles.
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
// s024: with the image, a GROUP with layer styles too: its contents
// composited (isolated), its styles drawn round that, and the group then
// stands in the result as one pixel layer (its mask, opacity, mode kept).
Layers effective(const Layers& ls, const Image* im = nullptr);
// s024: a styled group's contents composited, flat (cached, keyed by its
// whole subtree); s025: look() draws its styles over that. nullptr when the
// image isn't RGB with alpha (styles draw in RGB).
std::shared_ptr<const pixel::TileMap> group_look(const Image& im, const Layers& ls, const Document::Id& gid);
// m3: the filtered pixels with the raw ones back where `hide` hides the filters.
pixel::TileMap masked(const pixel::TileMap& raw, const pixel::TileMap& filtered, const pixel::TileMap& hide);
// How far an active layer's look can reach past its pixels (the stack's reach).
int reach(const Layer& l);
// s036: a layer mask as it acts -- its feather and density applied (cached;
// `m` itself when they do nothing). Apply Layer Mask bakes this.
std::shared_ptr<const pixel::TileMap> edged_mask(const std::shared_ptr<const pixel::TileMap>& m, const MaskEdges& e);
// s036: a pixel layer's colours with its mask's fringe taken out (Defringe;
// cached). Its pixels themselves when there's nothing to do.
std::shared_ptr<const pixel::TileMap> defringed_pixels(const Layer& l);
void clear_cache();                       // tests
// s021: how many looks were made in part (the change and its reach only) --
// painting a live / styled layer. For the selftest.
int partly_count();

}  // namespace klimt::core::live

namespace klimt::core::layers {
// s018 -- the stack's verbs, each ONE step; refused on a group / an
// adjustment layer, under an All or Pixels lock.
bool add_live(DocJournal& j, Document& d, const Id& id, const Filter& f);
bool set_live(DocJournal& j, Document& d, const Id& id, std::size_t i, const Filter& f);
bool toggle_live(DocJournal& j, Document& d, const Id& id, std::size_t i);
bool remove_live(DocJournal& j, Document& d, const Id& id, std::size_t i);
bool move_live(DocJournal& j, Document& d, const Id& id, std::size_t from, std::size_t to);
// s023: filter i's blending (opacity %, mode) -- one step; quick presses (a
// slider's) fold into one.
bool set_live_blend(DocJournal& j, Document& d, const Id& id, std::size_t i, int opacity, pixel::Blend mode);
// The stack baked into the pixels (Photoshop's Rasterize): the look becomes
// the pixels, the stack goes. s019: with styles, they go into the pixels too
// (Rasterize Layer Style) and the fill returns to 100 (it is in the look).
bool bake_live(DocJournal& j, Document& d, const Id& id);
// m3: the FILTER MASK set (nullptr: none) -- one step; refused without live
// filters, or a map not the selection's format and the image's size.
bool set_filter_mask(DocJournal& j, Document& d, const Id& id, std::shared_ptr<const pixel::TileMap> m,
                     const std::string& label = "Filter Mask");
// s019 -- LAYER STYLES: set (presses fold into one step while they come
// quickly, merge key "styles:<id>"); a drag on a slider is ONE step. Refused
// on a group / adjustment layer, under an All lock.
bool set_styles(DocJournal& j, Document& d, const Id& id, const Styles& s, const std::string& label = "Layer Style");
void styles_begin(DocJournal& j, Document& d, const Id& id);
void styles_to(DocJournal& j, Document& d, const Id& id, const Styles& s);
bool styles_end(DocJournal& j);
// Paste / Clear Layer Style on several layers as ONE step (those it can't
// take -- groups, adjustment layers, All-locked -- are skipped).
bool set_styles_on(DocJournal& j, Document& d, const std::vector<Id>& ids, const Styles& s, const std::string& label);
}  // namespace klimt::core::layers
