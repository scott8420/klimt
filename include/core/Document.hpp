#pragma once
// core::Document -- Klimt's document as the core sees it: the LAYER TREE.
// GTK-free; the Layers panel, its drag and drop, and (later) the compositor
// sit on it. Pixels are not here yet -- a layer's pixels will live in
// libs/pixel keyed by the layer's id (docs/pixels.md is the proposal).
//
// The shape is Cairn's undo shape (docs/undo.md), the Stones fixture made real:
//
//   * the store meets the journal's contract: get / put by id, BY VALUE;
//   * a layer's PLACE is in its value (`parent`, `pos`), so putting an old
//     value back puts the layer back where it was -- delete + undo lands in
//     the same slot, inside the same group, under the same id;
//   * every user verb is a function in `layers::` that goes through
//     Journal::run (or a gesture) -- the ONE DOOR -- and is listed in
//     layers::verbs(), which the selftest walks;
//   * put() counts a write made with the door shut (`strays()`); a load says
//     so by scope (`Loading`);
//   * check() states the tree's invariants; the selftest asserts it after
//     every verb, every undo and every redo.
//
// ORDER. Siblings sort by `pos` ascending = PAINT order (bottom first, the
// way the compositor walks them). The panel shows the reverse (top first);
// panel() and walk() hand back that order so the UI never flips it itself.
// A structural verb renumbers the siblings it touches to 1..n, so positions
// never run out of room between two neighbours.
#include "core/Adjust.hpp"
#include "core/Filter.hpp"
#include "core/Path.hpp"     // s035
#include "core/Styles.hpp"   // s019
#include "core/Undo.hpp"
#include "pixel/Blend.hpp"
#include "pixel/Pixel.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace klimt::core {

// ── blend modes: the full Photoshop set, plus Pass Through for groups ───────
// Blend If is not a mode -- it lands with the compositor (a per-layer range).
using Blend = pixel::Blend;   // s006: the enum lives in libs/pixel/Blend.hpp
struct BlendInfo {
    Blend       mode;
    const char* key;      // stable, for files and scripts: "linear-burn"
    const char* label;    // what the menu says: "Linear Burn"
    int         section;  // the menu's separator groups, 0..6
};
const std::vector<BlendInfo>& blend_modes();   // in menu order
std::string_view     blend_key(Blend b);
std::string_view     blend_label(Blend b);
std::optional<Blend> blend_from_key(std::string_view key);

enum class Kind : std::uint8_t { Pixel, Group, Adjustment, Filter, Path };   // s013: Adjustment; s030: Filter; s035: Path; Text later

// Photoshop's four locks, as bits. Only All bites in the tree today: it
// refuses opacity, fill, blend and clip on the layer and on everything inside
// a locked group. Rename, show / hide, reorder and delete stay allowed. The
// other three are pixel locks -- they bite when there are pixels.
namespace lock {
constexpr std::uint8_t Alpha    = 1;   // transparent pixels
constexpr std::uint8_t Pixels   = 2;   // image pixels
constexpr std::uint8_t Position = 4;   // moving the content
constexpr std::uint8_t All      = 8;
constexpr std::uint8_t Mask     = 15;
}  // namespace lock

struct Image;

struct Layer {
    Kind          kind    = Kind::Pixel;
    std::string   name;
    std::string   parent;              // "" = the document's top level
    double        pos     = 0;         // place among siblings, ascending = bottom -> top
    bool          visible = true;
    std::uint8_t  locks   = 0;         // lock:: bits
    int           opacity = 100;       // 0..100, the whole layer (styles too)
    int           fill    = 100;       // 0..100, the content only (not styles)
    Blend         blend   = Blend::Normal;   // a group starts PassThrough
    bool          clipped = false;     // clipped to the base below it
    // s006: the layer's PIXELS -- an immutable tile map (libs/pixel), shared
    // by pointer. nullptr = fully transparent (a new layer, a group). An edit
    // makes a new map that shares every untouched tile, so == compares the
    // POINTERS: a step's two pictures differ exactly when the pixels changed,
    // and they cost only the tiles the edit touched (docs/pixels.md 6).
    std::shared_ptr<const pixel::TileMap> pixels;
    // s009: only on the "@image" record -- the image's size, format and
    // resolution, shared and compared by pointer like the pixels.
    std::shared_ptr<const Image> image;
    // s012: the LAYER MASK -- stored as how much is HIDDEN (0 shows, 1 hides;
    // the UI paints it the usual way: white reveals, black hides), one Gray
    // channel at the document's depth like the selection, so an absent tile
    // shows everything and "Reveal All" costs nothing. nullptr: no mask; a
    // map with no tiles: a mask that hides nothing. Pixel layers only.
    std::shared_ptr<const pixel::TileMap> mask;
    bool          mask_on = true;      // Shift-click the mask: off (it stays, shows nothing)
    // s013: an ADJUSTMENT LAYER's settings (core/Adjust.hpp); kind None on
    // every other layer. No pixels; it may have a mask.
    Adjust        adjust;
    // s018: LIVE FILTERS (core/Live.hpp) -- pixel layers only; the compositor
    // draws the pixels through them. Empty: none.
    std::vector<LiveFilter> filters;
    // s018 m3: the FILTER MASK -- where the live filters show, stored as how
    // much they are HIDDEN (as a layer mask: 0 shows, 1 hides; painted white
    // reveals, black hides). nullptr: they show everywhere.
    std::shared_ptr<const pixel::TileMap> filter_mask;
    // s019: LAYER STYLES (core/Styles.hpp) -- pixel layers only; drawn live.
    Styles        styles;
    // s035: a PATH LAYER's outline (Kind::Path; docs/paths.md) -- shared and
    // compared by pointer like the pixels. It draws nothing itself. Two
    // switches: `path_mask` -- the layer just below it shows only inside the
    // path (live, through live::effective); `path_select` -- the selection IS
    // the path's area, following each edit.
    std::shared_ptr<const path::Path> path;
    bool          path_mask = false;
    bool          path_select = false;
    bool operator==(const Layer&) const = default;
};

// The image every layer's pixels share: its size and format. Set when a
// document is made or opened (a load -- not a step). s009: it is a record of
// the store too, under kImageId, so Crop / Image Size / Canvas Size are steps
// like any other (core/Canvas.hpp).
// s032: a GUIDE -- a line across the picture at `at` image pixels: a
// vertical one (x = at) or a horizontal one (y = at).
struct Guide {
    bool   vertical = true;
    double at = 0;
    bool operator==(const Guide&) const = default;
};

struct Image {
    int           width = 0, height = 0;
    pixel::Format format;
    // s007: what a file said of itself -- its resolution, and its colour
    // profile (kept and saved again; used once libs/tint lands). Shared:
    // a profile is a few KB and never changes.
    double        dpi = 72;
    std::shared_ptr<const std::vector<std::uint8_t>> icc{};
    // s031: the rulers' ZERO POINT, image pixels (0, 0: the top-left
    // corner). Crop, Canvas Size and Image Size carry it with the picture.
    double        origin_x = 0, origin_y = 0;
    // s032: the GUIDES (docs/guides.md) -- carried like the zero point.
    std::vector<Guide> guides;
    bool operator==(const Image&) const = default;
};

// s008: THE SELECTION is a record of the store too -- under this reserved id,
// held APART from the layer tree (walk / panel / snapshot / count / check's
// tree never see it). Its value is a Layer whose `pixels` is the MASK (nullptr
// = nothing selected), so a selection change goes through the one door and
// undoes like any step, with no second journal (Scott, s007 -> s008, answer
// 2). docs/selection.md.
inline constexpr const char* kSelectionId = "@selection";
// s016: SAVED SELECTIONS (Photoshop's alpha channels): records apart from the
// tree, like the selection -- ids "@channel/<n>"; each a Layer whose `name`
// is the channel's and whose `mask` is the saved selection (the selection's
// format). The compositor and the Layers panel never see them; the canvas
// verbs carry them as they carry layer masks; .klimt keeps them.
inline constexpr const char* kChannelPrefix = "@channel/";
inline bool is_channel(const std::string& id) { return id.rfind(kChannelPrefix, 0) == 0; }
// s009: THE IMAGE (size, format, resolution) under this reserved id: a Layer
// whose `image` carries it. Apart from the tree like the selection; it can be
// replaced, never removed.
inline constexpr const char* kImageId = "@image";

class Document {
public:
    using Id    = std::string;
    using Value = Layer;

    // ── the journal's contract ──────────────────────────────────────────────
    std::optional<Layer> get(const Id& id) const;
    void put(const Id& id, const std::optional<Layer>& v);

    // ── reading the tree ────────────────────────────────────────────────────
    std::size_t     count() const { return m_items.size(); }
    const Image&    image() const { return m_image; }
    std::shared_ptr<const Image> image_ptr() const { return m_image_ptr; }
    void            set_image(const Image& im);   // only under Loading (a new / opened document)
    // An empty tile map at the image's size and format (a layer's first paint).
    pixel::TileMap  blank() const { return pixel::TileMap(m_image.format, m_image.width, m_image.height); }
    // s008: the selection. A MASK: one channel (Gray, no alpha) at the
    // document's depth, on the same sparse grid; 0 = not selected, 1 =
    // selected, between = partly (a feathered or anti-aliased edge). An
    // absent tile is unselected. nullptr: nothing selected.
    const pixel::TileMap* selection() const { return m_selection && m_selection->pixels ? m_selection->pixels.get() : nullptr; }
    std::shared_ptr<const pixel::TileMap> selection_ptr() const { return m_selection ? m_selection->pixels : nullptr; }
    pixel::Format   mask_format() const { return {m_image.format.depth, pixel::Mode::Gray, false}; }
    pixel::TileMap  blank_mask() const { return pixel::TileMap(mask_format(), m_image.width, m_image.height); }
    // A copy of the records -- what a worker composites from (s006). Cheap:
    // values, and the pixels are shared pointers to immutable maps.
    std::map<Id, Layer> snapshot() const { return m_items; }
    // s016: the saved selections, by id ("@channel/<n>"); in their order (pos).
    const std::map<Id, Layer>& channels() const { return m_channels; }
    std::vector<Id> channel_order() const;
    void replace_channels(std::map<Id, Layer> c);   // only under Loading (an opened document)
    std::vector<Id> children(const Id& parent) const;   // paint order (bottom first)
    std::vector<Id> panel(const Id& parent) const;      // panel order (top first)
    std::vector<Id> walk() const;          // whole tree, panel order, a group before its contents
    std::vector<Id> subtree(const Id& id) const;        // id + descendants, parent first
    bool  is_group(const Id& id) const;
    bool  within(const Id& id, const Id& ancestor) const;   // id == ancestor, or inside it
    int   depth(const Id& id) const;                    // 0 at the top level
    bool  locked_all(const Id& id) const;               // its own All lock, or a group's above it
    bool  shown(const Id& id) const;                    // visible, and every group above it too
    // The layer a clipped layer clips to: the nearest unclipped sibling below
    // it. "" when it isn't clipped or nothing is below (then the clip is inert).
    Id    clip_base(const Id& id) const;
    Id    fresh_id();                                    // "L1", "L2", ... never reused
    int   next_id() const { return m_next; }             // what fresh_id() hands out next (a file keeps it)

    // s007 -- a whole new document in place of this one: New, Open, Revert.
    // Only under Loading (not a user choice, not a step: the Shell clears the
    // journal with it). `next` keeps fresh ids ahead of a file's own.
    void  replace(const Image& im, std::map<Id, Layer> layers, int next = 1);
    // The tree's invariants; "" when they all hold, else the first one broken.
    // Every parent exists and is a group; no cycles; no two siblings share a
    // place; opacity / fill in 0..100; locks within the mask; Pass Through on
    // groups only.
    std::string check() const;

    // s020 -- DOCUMENT TABS. A document not on screen is PARKED: its records
    // moved out whole (O(1): maps and shared pixels). swap_parked() trades
    // this document's contents with a parked one -- the window's other tab
    // comes in, this one goes out. Only under Loading (not a step: each tab
    // keeps its own journal history, swapped alongside). The door and the
    // change callback stay: they belong to the window, not the document.
    struct Parked {
        std::map<Id, Layer>          items;
        std::optional<Layer>         selection;
        std::map<Id, Layer>          channels;
        Image                        image;
        std::shared_ptr<const Image> image_ptr = std::make_shared<const Image>();
        int                          next = 1;
    };
    void swap_parked(Parked& p);

    // ── the one-door guard ──────────────────────────────────────────────────
    void set_door(std::function<bool()> open) { m_door = std::move(open); }
    int  strays() const { return m_strays; }
    struct Loading {   // opening a file is not a user choice, and not undoable
        explicit Loading(Document& d) : m_d(d) { ++m_d.m_loading; }
        ~Loading() { --m_d.m_loading; }
        Document& m_d;
    };
    void set_on_change(std::function<void()> f) { m_changed = std::move(f); }

private:
    std::map<Id, Layer>    m_items;
    std::optional<Layer>   m_selection;    // kSelectionId's record (s008)
    std::map<Id, Layer>    m_channels;     // s016: the saved selections
    Image                  m_image;
    std::shared_ptr<const Image> m_image_ptr = std::make_shared<const Image>();   // kImageId's value (s009)
    int                    m_next = 1;
    int                    m_strays = 0;
    int                    m_loading = 0;
    std::function<bool()>  m_door;
    std::function<void()>  m_changed;
};

using DocJournal = Journal<Document>;

// ── the verbs: every user choice on the layer tree ──────────────────────────
// Each goes through the journal; the UI calls these and nothing else writes.
// A verb that would change nothing (or is refused) keeps no step and returns
// false / empty.
namespace layers {
using Id = Document::Id;

// Where a drop lands, relative to the row under the pointer: the drop line
// above it, below it, or (a group) inside it, at the top.
enum class Where { Above, Below, Inside };

// New layer / group just above `above`, in the same group; above a GROUP that
// is selected means inside it, at the top. Nothing given: top of the document.
Id   add_layer(DocJournal& j, Document& d, const std::string& name, const Id& above = {});
Id   add_group(DocJournal& j, Document& d, const std::string& name, const Id& above = {});
// Ctrl+G: a new group where the topmost of `ids` was, holding them all in
// their panel order. Returns the group.
Id   group(DocJournal& j, Document& d, const std::vector<Id>& ids, const std::string& name = "Group");
// The group's contents take its place in its parent; the group goes.
bool ungroup(DocJournal& j, Document& d, const Id& group);
// Delete (a group takes its contents). Refused if it would empty the document.
bool remove(DocJournal& j, Document& d, const std::vector<Id>& ids);
// Copies (a group with its contents, fresh ids) each just above its original.
std::vector<Id> duplicate(DocJournal& j, Document& d, const std::vector<Id>& ids);
// s022: layers from elsewhere (core/Transfer.hpp makes them fit): values with
// their OLD ids and parents, `roots` the tops (panel order); fresh ids, the
// roots just above `above`; ONE step. The new roots' ids.
std::vector<Id> insert(DocJournal& j, Document& d, const std::string& label,
                       const std::vector<std::pair<Id, Layer>>& layers, const std::vector<Id>& roots, const Id& above = {});
// THE DROP. `ids` (a multi-selection; a layer whose group is also selected
// rides along with it) land contiguously, in their panel order, at `where`
// relative to `target`. Refused: a group into itself or its own contents; a
// target that is itself being moved; Inside on a layer that isn't a group.
bool move(DocJournal& j, Document& d, const std::vector<Id>& ids, const Id& target, Where where);
// s022: Alt- / Ctrl-drag in the panel -- copies landing where the drop says;
// ONE step. The copies' ids (roots).
std::vector<Id> copy_to(DocJournal& j, Document& d, const std::vector<Id>& ids, const Id& target, Where where);
// Would that drop do anything? (Refused, or lands where it already is: false.)
// Writes nothing -- the panel asks on every pointer motion.
bool can_move(const Document& d, const std::vector<Id>& ids, const Id& target, Where where);
// Put a group's contents (or the top level's, parent "") in this panel order
// (top first). Refused unless `order` is exactly its current contents -- a job
// that planned an order on a copy lands through this, and if the group changed
// while it worked, nothing happens.
bool arrange(DocJournal& j, Document& d, const Id& parent, const std::vector<Id>& order,
             const std::string& label);
// "Layer 2" before "Layer 10", case-insensitive: the order Sort by Name uses.
bool natural_less(const std::string& a, const std::string& b);
// The sort's plan, on a copy (a worker may run it): names in panel order in,
// ids in A-Z panel order out. Pure.
struct Named { Id id; std::string name; };
std::vector<Id> sorted_by_name(std::vector<Named> rows);
// ── pixels (s006) ── every pixel write is a layer value with a new tile map,
// through the journal like any other verb. Refused on a group, under an All
// or a Pixels lock, or for a rectangle wholly outside the image. Under an
// Alpha lock ("lock transparent pixels") only colour changes, where there is
// already something: transparent stays transparent, alpha stays as it was.
bool fill(DocJournal& j, Document& d, const Id& id, const pixel::Rect& r, const pixel::Px& colour);
bool clear(DocJournal& j, Document& d, const Id& id, const pixel::Rect& r);   // to transparent
// Ctrl+] / Ctrl+[ : one place up / down among its siblings.
bool raise(DocJournal& j, Document& d, const Id& id);
bool lower(DocJournal& j, Document& d, const Id& id);

bool rename(DocJournal& j, Document& d, const Id& id, const std::string& name);
bool set_visible(DocJournal& j, Document& d, const Id& id, bool visible);
bool set_blend(DocJournal& j, Document& d, const Id& id, Blend mode);
bool set_clipped(DocJournal& j, Document& d, const Id& id, bool clipped);
bool set_locks(DocJournal& j, Document& d, const Id& id, std::uint8_t locks);
// Keys / scroll / typing on the opacity and fill fields: presses inside the
// merge window fold into one step (merge key "opacity:<id>", "fill:<id>").
bool set_opacity(DocJournal& j, Document& d, const Id& id, int percent);
bool set_fill(DocJournal& j, Document& d, const Id& id, int percent);
// The same on a whole SELECTION, as ONE step (s005; WEAKNESSES 5 -- Photoshop
// sets them on every selected layer). Layers inside an All lock are skipped
// (and so are layers Pass Through can't apply to); refused when none is left.
// Merge keys name the whole set, so presses on the same selection fold.
bool set_blend(DocJournal& j, Document& d, const std::vector<Id>& ids, Blend mode);
bool set_locks(DocJournal& j, Document& d, const std::vector<Id>& ids, std::uint8_t locks);
bool set_opacity(DocJournal& j, Document& d, const std::vector<Id>& ids, int percent);
bool set_fill(DocJournal& j, Document& d, const std::vector<Id>& ids, int percent);
void opacity_begin(DocJournal& j, Document& d, const std::vector<Id>& ids);
void opacity_to(DocJournal& j, Document& d, const std::vector<Id>& ids, int percent);
// Dragging the opacity slider: ONE step for the whole drag.
void opacity_begin(DocJournal& j, Document& d, const Id& id);
void opacity_to(DocJournal& j, Document& d, const Id& id, int percent);
bool opacity_end(DocJournal& j);

// s012 -- LAYER MASKS. Refused on a group, under an All lock (a mask edits
// what shows, not the pixels: the Pixels lock doesn't stop it).
enum class MaskFrom { RevealAll, HideAll, RevealSelection, HideSelection };
bool add_mask(DocJournal& j, Document& d, const Id& id, MaskFrom from);
bool delete_mask(DocJournal& j, Document& d, const Id& id);
// Bake the mask into the pixels' alpha and drop it. Refused with no alpha,
// under a Pixels or Alpha lock.
bool apply_mask(DocJournal& j, Document& d, const Id& id);
bool set_mask_on(DocJournal& j, Document& d, const Id& id, bool on);
bool invert_mask(DocJournal& j, Document& d, const Id& id);
// A job that computed a mask on a copy lands through this (refused unless
// the mask is still `expect`).
bool set_mask_if(DocJournal& j, Document& d, const Id& id, const std::shared_ptr<const pixel::TileMap>& expect,
                 pixel::TileMap after, const std::string& label);

// s013 -- ADJUSTMENT LAYERS (core/Adjust.hpp, docs/adjustments.md). A new
// one sits above `above` like add_layer, named "Levels 1", "Curves 2" ...,
// starting as an adjustment that changes nothing, with a mask: the selection
// (revealed) when there is one, else Reveal All -- Photoshop's way. One step.
Id   add_adjustment(DocJournal& j, Document& d, AdjustKind k, const Id& above = {});
// Its settings (normalised). Refused on any other layer, for a different
// kind, or under an All lock. Presses inside the merge window fold
// (merge key "adjust:<id>").
bool set_adjust(DocJournal& j, Document& d, const Id& id, const Adjust& a);
// Dragging a slider or a curve point: ONE step for the whole drag.
void adjust_begin(DocJournal& j, Document& d, const Id& id);
void adjust_to(DocJournal& j, Document& d, const Id& id, const Adjust& a);
bool adjust_end(DocJournal& j);
// s030 -- FILTER LAYERS (docs/filter-layers.md): a filter over everything
// below it, non-destructive -- Affinity's live filter layers. Its one filter
// is in Layer::filters[0]; no pixels; a mask, opacity, blend mode, clip like
// an adjustment layer. Clipped, it filters its clip base alone. A new one
// sits above `above`, named "Gaussian Blur 1" ..., with a mask (the
// selection revealed, or Reveal All). One step.
Id   add_filter_layer(DocJournal& j, Document& d, const Filter& f, const Id& above = {});
// Its filter (normalised, the same kind). Presses inside the merge window
// fold (merge key "filterlayer:<id>"); a slider drag is ONE step.
bool set_filter_layer(DocJournal& j, Document& d, const Id& id, const Filter& f);
void filter_layer_begin(DocJournal& j, Document& d, const Id& id);
void filter_layer_to(DocJournal& j, Document& d, const Id& id, const Filter& f);
bool filter_layer_end(DocJournal& j);
// Image > Adjustments: `a` applied to a pixel layer's pixels for good,
// through the selection. Refused on a group / adjustment layer, under an All
// or a Pixels lock, on CMYK / Lab documents, or when it changes nothing.
bool apply_adjust(DocJournal& j, Document& d, const Id& id, const Adjust& a);
// s014 -- the Filter menu: `f` applied to a pixel layer's pixels for good,
// through the selection (a transparency lock keeps a blur off alpha). Refused
// as apply_adjust is. docs/filters.md.
bool apply_filter(DocJournal& j, Document& d, const Id& id, const Filter& f);
// "is a group" / "is an adjustment layer": why a pixel verb is refused.
std::string not_pixels(const Document& d, const Id& id);

// Who takes the selection when `id` goes: the row below it in the panel, else
// the row above, else its group, else nobody.
Id neighbour(const Document& d, const Id& id);

// The verb table: every verb, by name, runnable on the selftest's fixture
// (Background, a group "Group" holding "A" over "B", and "Top"). A verb the
// panel offers and this table lacks is the gap the gate cannot see -- add it
// here in the same change that adds the button.
struct Verb {
    std::string name;
    std::function<void(DocJournal&, Document&)> run;
    // s008: what must already be there (a selection to deselect), LOADED --
    // not a step -- before the gate takes its "before" picture. May be empty.
    std::function<void(Document&)> setup = {};
};
const std::vector<Verb>& verbs();
// The fixture the table runs on, LOADED (not undoable). Panel order:
//   L5 Top
//   L2 Group      (Pass Through)
//     L4 A
//     L3 B
//   L1 Background
void load_fixture(Document& d);
// s008: a rectangular selection, LOADED (a verb's setup; tests).
void load_selection(Document& d, const pixel::Rect& r);
// s030: a filter layer "L6" at the top (a verb's setup; tests).
void load_filter_layer(Document& d, FilterKind k);
// s035: PATH LAYERS (docs/paths.md). A new one, "Path 1" ..., just above
// `above` (as add_layer), holding `p` -- ONE step. Its outline: refused on
// any other kind; with path_select on, the selection follows in the same
// step. Its switches: Mask (the layer below shows only inside), Selection
// (the selection is the path's area -- switched on, it takes it at once).
Id   add_path_layer(DocJournal& j, Document& d, const path::Path& p, const Id& above = {}, const std::string& label = "New Path");
bool set_path_layer(DocJournal& j, Document& d, const Id& id, const path::Path& p, const std::string& label);
bool set_path_switches(DocJournal& j, Document& d, const Id& id, bool mask, bool select);
// A path layer "L6" holding `p` at the top, LOADED (a verb's setup; tests).
void load_path_layer(Document& d, const path::Path& p, bool mask = false, bool select = false);
// s032: these guides on the picture (a verb's setup; tests).
void load_guides(Document& d, const std::vector<Guide>& g);
// s012: a layer mask (a verb's setup; tests).
void load_mask(Document& d, const Id& id);
// s013: an adjustment layer "L6" of kind k at the top, its mask hiding all
// but (120, 110) 60 x 60, LOADED (a verb's setup; tests).
void load_adjustment(Document& d, AdjustKind k);
}  // namespace layers

}  // namespace klimt::core
