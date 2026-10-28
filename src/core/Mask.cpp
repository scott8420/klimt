// core -- LAYER MASKS (s012 m2). docs/masks.md.
//
// A mask is stored as how much it HIDES (0 shows, 1 hides): an absent tile
// shows everything, so a new "Reveal All" mask costs nothing. The UI speaks
// Photoshop's language (white reveals, black hides) and converts at the edge.
#include "core/Document.hpp"
#include "core/Selection.hpp"

namespace klimt::core::layers {

namespace {
using MapPtr = std::shared_ptr<const pixel::TileMap>;

bool maskable(const Document& d, const Id& id) {
    const auto l = d.get(id);
    return l && !d.locked_all(id);   // s013: adjustment layers too; s024: groups too
}

bool put_mask(DocJournal& j, Document& d, const Id& id, MapPtr mask, bool on, const std::string& label) {
    Layer l = *d.get(id);
    if (l.mask == mask && l.mask_on == on) return false;
    l.mask = std::move(mask);
    l.mask_on = on;
    return j.run(label, {id}, [&] { d.put(id, l); });
}
}  // namespace

bool add_mask(DocJournal& j, Document& d, const Id& id, MaskFrom from) {
    if (!maskable(d, id) || d.get(id)->mask) return false;
    const auto mf = d.mask_format();
    const int w = d.image().width, h = d.image().height;
    pixel::TileMap m(mf, w, h);
    const auto* sel = d.selection();
    switch (from) {
        case MaskFrom::RevealAll: break;
        case MaskFrom::HideAll: m = select::all_mask(mf, w, h); break;
        case MaskFrom::RevealSelection: if (sel) m = select::inverted(sel, mf, w, h); break;
        case MaskFrom::HideSelection: if (sel) m = *sel; break;
    }
    return put_mask(j, d, id, std::make_shared<const pixel::TileMap>(std::move(m)), true, "Add Layer Mask");
}

bool delete_mask(DocJournal& j, Document& d, const Id& id) {
    if (!maskable(d, id) || !d.get(id)->mask) return false;
    return put_mask(j, d, id, nullptr, true, "Delete Layer Mask");
}

bool apply_mask(DocJournal& j, Document& d, const Id& id) {
    if (!maskable(d, id) || d.get(id)->kind != Kind::Pixel) return false;   // nothing to bake into
    Layer l = *d.get(id);
    if (!l.mask || !d.image().format.alpha || (l.locks & (lock::Pixels | lock::Alpha))) return false;
    if (l.mask_on && l.pixels) {
        auto p = select::cleared(*l.pixels, l.mask.get(), pixel::transparent());   // alpha *= 1 - hide
        l.pixels = p.tile_count() ? std::make_shared<const pixel::TileMap>(std::move(p)) : nullptr;
    }
    l.mask = nullptr;   // a disabled mask applies as nothing: Photoshop's "apply" of an off mask just drops it
    l.mask_on = true;
    return j.run("Apply Layer Mask", {id}, [&] { d.put(id, l); });
}

bool set_mask_on(DocJournal& j, Document& d, const Id& id, bool on) {
    if (!maskable(d, id)) return false;
    const auto l = *d.get(id);
    if (!l.mask) return false;
    return put_mask(j, d, id, l.mask, on, on ? "Enable Layer Mask" : "Disable Layer Mask");
}

bool invert_mask(DocJournal& j, Document& d, const Id& id) {
    if (!maskable(d, id)) return false;
    const auto l = *d.get(id);
    if (!l.mask) return false;
    auto inv = select::inverted(l.mask->tile_count() ? l.mask.get() : nullptr, d.mask_format(), d.image().width, d.image().height);
    return put_mask(j, d, id, std::make_shared<const pixel::TileMap>(std::move(inv)), l.mask_on, "Invert Layer Mask");
}

bool set_mask_if(DocJournal& j, Document& d, const Id& id, const MapPtr& expect, pixel::TileMap after, const std::string& label) {
    if (!maskable(d, id)) return false;
    const auto l = *d.get(id);
    if (!l.mask || l.mask != expect) return false;
    if (!(after.format() == d.mask_format()) || after.width() != d.image().width || after.height() != d.image().height) return false;
    return put_mask(j, d, id, std::make_shared<const pixel::TileMap>(std::move(after)), l.mask_on, label);
}

}  // namespace klimt::core::layers
