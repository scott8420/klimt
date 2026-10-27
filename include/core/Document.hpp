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

enum class Kind : std::uint8_t { Pixel, Group };   // Text, Adjustment come later

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
    bool operator==(const Layer&) const = default;
};

// The image every layer's pixels share: its size and format. Set when a
// document is made or opened (a load -- not a step). s009: it is a record of
// the store too, under kImageId, so Crop / Image Size / Canvas Size are steps
// like any other (core/Canvas.hpp).
struct Image {
    int           width = 0, height = 0;
    pixel::Format format;
    // s007: what a file said of itself -- its resolution, and its colour
    // profile (kept and saved again; used once libs/tint lands). Shared:
    // a profile is a few KB and never changes.
    double        dpi = 72;
    std::shared_ptr<const std::vector<std::uint8_t>> icc{};
    bool operator==(const Image&) const = default;
};

// s008: THE SELECTION is a record of the store too -- under this reserved id,
// held APART from the layer tree (walk / panel / snapshot / count / check's
// tree never see it). Its value is a Layer whose `pixels` is the MASK (nullptr
// = nothing selected), so a selection change goes through the one door and
// undoes like any step, with no second journal (Scott, s007 -> s008, answer
// 2). docs/selection.md.
inline constexpr const char* kSelectionId = "@selection";
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
// THE DROP. `ids` (a multi-selection; a layer whose group is also selected
// rides along with it) land contiguously, in their panel order, at `where`
// relative to `target`. Refused: a group into itself or its own contents; a
// target that is itself being moved; Inside on a layer that isn't a group.
bool move(DocJournal& j, Document& d, const std::vector<Id>& ids, const Id& target, Where where);
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
// s012: a layer mask (a verb's setup; tests).
void load_mask(Document& d, const Id& id);
}  // namespace layers

}  // namespace klimt::core
