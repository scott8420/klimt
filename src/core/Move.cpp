#include "core/Move.hpp"
#include "core/Smart.hpp"   // s082
#include "core/Text.hpp"   // s046
#include "core/Selection.hpp"
#include "core/Canvas.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace klimt::core::move {

namespace {
using pixel::kTile;

// A lock that stops this layer moving: Position or All, its own or a group's above.
std::string lock_on(const Document& d, const Id& id) {
    for (Id at = id; !at.empty();) {
        const auto l = d.get(at);
        if (!l) break;
        if (l->locks & (lock::Position | lock::All)) return l->name.empty() ? at : l->name;
        at = l->parent;
    }
    return {};
}

}  // namespace

std::vector<Id> targets(const Document& d, const std::vector<Id>& selected) {
    std::set<Id> want;
    for (const auto& s : selected)
        for (const auto& id : d.subtree(s))
            if (const auto l = d.get(id); l && draws_pixels(l->kind)) want.insert(id);   // s046: type too
    std::vector<Id> out;
    for (const auto& id : d.walk())
        if (want.count(id)) out.push_back(id);
    return out;
}

// s058: the adjustment / filter layers in the selected trees that have a
// mask -- what a move carries besides pixels.
std::vector<Id> mask_layers(const Document& d, const std::vector<Id>& selected) {
    std::set<Id> want;
    for (const auto& s : selected)
        for (const auto& id : d.subtree(s))
            if (const auto l = d.get(id); l && (l->kind == Kind::Adjustment || l->kind == Kind::Filter) && l->mask) want.insert(id);
    std::vector<Id> out;
    for (const auto& id : d.walk())
        if (want.count(id)) out.push_back(id);
    return out;
}

std::string refusal(const Document& d, const std::vector<Id>& selected, const Id& mask_target) {
    const auto ids = targets(d, selected);
    // s059: an unlinked mask being painted moves whole; a selection lifts pixels.
    if (!mask_target.empty() && d.selection_ptr() && std::find(ids.begin(), ids.end(), mask_target) != ids.end())
        if (const auto l = d.get(mask_target); l && l->mask && !l->mask_linked)
            return "a mask moves whole -- deselect (Ctrl+D) to move it";
    const auto masks = mask_layers(d, selected);
    if (ids.empty() && masks.empty()) {
        if (selected.empty()) return "pick a layer to move";
        if (const auto l = d.get(selected.front()); l && (l->kind == Kind::Adjustment || l->kind == Kind::Filter))
            return "“" + l->name + "” has no mask to move -- an adjustment acts everywhere until it has one";
        return "nothing to move -- the group is empty";
    }
    // A selection lifts PIXELS; a mask moves whole. Only masks picked: say so.
    if (ids.empty() && d.selection_ptr()) return "a mask moves whole -- deselect (Ctrl+D) to move it";
    for (const auto& id : ids)
        if (const auto who = lock_on(d, id); !who.empty()) return "“" + who + "” is locked in place";
    for (const auto& id : masks)
        if (const auto who = lock_on(d, id); !who.empty()) return "“" + who + "” is locked in place";
    return {};
}

std::optional<Plan> plan(const Document& d, const std::vector<Id>& selected, const pixel::Px& background,
                         const Id& mask_target) {
    if (!refusal(d, selected, mask_target).empty()) return std::nullopt;
    Plan p;
    p.ids = targets(d, selected);
    // s059: an UNLINKED mask that is what's painted moves alone -- its
    // layer's pixels stay (the layer leaves the pixel targets, its mask rides
    // as an adjustment's does).
    if (!mask_target.empty() && !d.selection_ptr())
        if (const auto l = d.get(mask_target); l && l->mask && !l->mask_linked)
            if (const auto it = std::find(p.ids.begin(), p.ids.end(), mask_target); it != p.ids.end()) {
                p.ids.erase(it);
                p.mask_only.push_back(mask_target);
                p.masks[mask_target] = l->mask;
            }
    p.mask = d.selection_ptr();
    p.width = d.image().width;
    p.height = d.image().height;
    p.format = d.image().format;
    p.fill = p.format.alpha ? pixel::transparent() : background;
    // s024: a selected group's mask (and those of groups inside it) travels
    // with its contents -- on a whole move (a selection lifts pixels only).
    if (!p.mask)
        for (const auto& s : selected)
            for (const auto& id : d.subtree(s))
                if (const auto l = d.get(id); l && l->kind == Kind::Group && l->mask &&
                    std::find(p.groups.begin(), p.groups.end(), id) == p.groups.end()) {
                    p.groups.push_back(id);
                    p.masks[id] = l->mask;
                }
    if (!p.mask)   // s058: adjustment / filter layers' masks ride along
        for (const auto& id : mask_layers(d, selected)) {
            p.mask_only.push_back(id);
            p.masks[id] = d.get(id)->mask;
        }
    for (const auto& id : p.ids) {
        const auto l = *d.get(id);
        p.original[id] = l.pixels;
        if (l.mask && l.mask_linked) p.masks[id] = l.mask;   // s059: unlinked, it stays
        if (l.kind == Kind::Text && l.text) { p.texts[id] = l.text; continue; }   // s046: moves whole
        if (l.kind == Kind::Smart && l.smart) { p.smarts[id] = l.smart; continue; }   // s082: moves whole
        if (p.mask) {
            const pixel::TileMap src = l.pixels ? *l.pixels : d.blank();
            auto c = select::cleared(src, p.mask.get(), background);
            p.base[id] = c.tile_count() ? std::make_shared<const pixel::TileMap>(std::move(c)) : nullptr;
        }
    }
    return p;
}

pixel::TileMap shifted(const pixel::TileMap& src, int dx, int dy, const pixel::Px& fill) {
    return canvas::reframed(src, src.width(), src.height(), dx, dy, fill);   // s009 m2: one copier
}

pixel::TileMap over(const pixel::TileMap& under, const pixel::TileMap& top, const pixel::TileMap& cov, const pixel::Rect& r) {
    const auto& f = under.format();
    const int cc = f.colour_channels();
    const auto ai = static_cast<std::size_t>(f.channels() - 1);
    return under.paint(r, [&](int x, int y, const pixel::Px& o) {
        const float m = select::coverage(cov, x, y);
        if (m <= 0.f) return o;
        const pixel::Px t = top.get(x, y);
        pixel::Px p = o;
        if (!f.alpha) {
            for (int k = 0; k < cc; ++k) { const auto q = static_cast<std::size_t>(k); p.c[q] = o.c[q] + (t.c[q] - o.c[q]) * m; }
            return p;
        }
        const float ta = t.c[ai] * m, oa = o.c[ai], out = ta + oa * (1 - ta);
        if (out <= 0.f) return pixel::transparent();
        for (int k = 0; k < cc; ++k) {
            const auto q = static_cast<std::size_t>(k);
            p.c[q] = (t.c[q] * ta + o.c[q] * oa * (1 - ta)) / out;
        }
        p.c[ai] = out;
        return p;
    });
}

std::optional<Result> compute(const Plan& p, int dx, int dy, const std::function<bool()>& cancelled) {
    Result r;
    const auto stop = [&] { return cancelled && cancelled(); };
    const pixel::TileMap blank(p.format, p.width, p.height);
    // s046: type slides as type -- its Spec moved, set again (pure; any thread).
    for (const auto& [id, t] : p.texts) {
        if (stop()) return std::nullopt;
        if (dx == 0 && dy == 0) { r.texts[id] = t; r.pixels[id] = p.original.at(id); continue; }
        auto moved = std::make_shared<const text::Spec>(text::mapped(*t, {1, 0, 0, 1, double(dx), double(dy)}));
        Image im;
        im.width = p.width;
        im.height = p.height;
        im.format = p.format;
        r.pixels[id] = text_pixels(im, *moved);
        r.texts[id] = std::move(moved);
    }
    // s082: a smart layer's map slides; drawn again from its picture.
    for (const auto& [id, sm] : p.smarts) {
        if (stop()) return std::nullopt;
        if (dx == 0 && dy == 0) { r.smarts[id] = sm; r.pixels[id] = p.original.at(id); continue; }
        auto moved = std::make_shared<const smart::Smart>(
            smart::with_map(*sm, xform::Projective(xform::Affine::translate(dx, dy)) * smart::map_of(*sm)));
        Image im;
        im.width = p.width;
        im.height = p.height;
        im.format = p.format;
        auto px = smart::rendered(im, *moved, cancelled);
        if (!px) return std::nullopt;
        r.pixels[id] = *px;
        r.smarts[id] = std::move(moved);
    }
    if (!p.mask) {
        for (const auto& id : p.ids) {
            if (stop()) return std::nullopt;
            if (p.texts.count(id) || p.smarts.count(id)) continue;   // s046 / s082: set above
            const auto& o = p.original.at(id);
            if (!o) { r.pixels[id] = nullptr; continue; }
            if (dx == 0 && dy == 0) { r.pixels[id] = o; continue; }
            auto m = shifted(*o, dx, dy, p.fill);
            r.pixels[id] = m.tile_count() ? std::make_shared<const pixel::TileMap>(std::move(m)) : nullptr;
        }
        // s012: a whole layer's mask moves with it (beyond the canvas: kept).
        for (const auto& [id, lm] : p.masks)
            r.masks[id] = (dx == 0 && dy == 0) ? lm : std::make_shared<const pixel::TileMap>(shifted(*lm, dx, dy, pixel::transparent()));
        return r;
    }
    if (dx == 0 && dy == 0) {   // back where it started: exactly the original
        for (const auto& id : p.ids) r.pixels[id] = p.original.at(id);
        for (const auto& [id, sm] : p.smarts) r.smarts[id] = sm;   // s082
        r.mask = p.mask;
        return r;
    }
    auto moved_mask = canvas::reframed(*p.mask, p.width, p.height, dx, dy, pixel::transparent(), false);   // a mask keeps nothing beyond
    const bool any = !select::empty(moved_mask);
    r.mask = any ? std::make_shared<const pixel::TileMap>(moved_mask) : nullptr;
    // Where the lifted pixels land: the selection's extent, slid, on the canvas.
    const pixel::Rect ext = select::extent(*p.mask);
    const pixel::Rect land = pixel::Rect{ext.x + dx, ext.y + dy, ext.w, ext.h}.intersect({0, 0, p.width, p.height});
    for (const auto& id : p.ids) {
        if (stop()) return std::nullopt;
        if (p.texts.count(id) || p.smarts.count(id)) continue;   // s046 / s082: set above (whole)
        const auto& b = p.base.at(id);
        const auto& o = p.original.at(id);
        pixel::TileMap out = b ? *b : blank;
        if (o && any && !land.empty()) {
            const auto top = shifted(*o, dx, dy, pixel::transparent());
            out = over(out, top, moved_mask, land);
        }
        r.pixels[id] = out.tile_count() ? std::make_shared<const pixel::TileMap>(std::move(out)) : nullptr;
    }
    return r;
}

std::vector<Id> step_ids(const Plan& p) {
    auto ids = p.ids;
    ids.insert(ids.end(), p.groups.begin(), p.groups.end());   // s024
    ids.insert(ids.end(), p.mask_only.begin(), p.mask_only.end());   // s058
    if (p.mask) ids.push_back(kSelectionId);
    return ids;
}

void put(Document& d, const Plan& p, const Result& r) {
    for (const auto& id : p.ids) {
        auto l = d.get(id);
        const auto it = r.pixels.find(id);
        if (!l || it == r.pixels.end()) continue;
        l->pixels = it->second;
        if (const auto t = r.texts.find(id); t != r.texts.end() && l->kind == Kind::Text) l->text = t->second;   // s046
        if (const auto sm = r.smarts.find(id); sm != r.smarts.end() && l->kind == Kind::Smart) l->smart = sm->second;   // s082
        if (const auto m = r.masks.find(id); m != r.masks.end()) l->mask = m->second;   // s012
        d.put(id, *l);
    }
    std::vector<Id> carried = p.groups;   // s024: the groups' masks; s058: adjustment / filter layers'
    carried.insert(carried.end(), p.mask_only.begin(), p.mask_only.end());
    for (const auto& id : carried) {
        auto l = d.get(id);
        const auto m = r.masks.find(id);
        if (!l || m == r.masks.end()) continue;
        l->mask = m->second;
        d.put(id, *l);
    }
    if (p.mask) {
        auto s = d.get(kSelectionId).value_or(Layer{});
        s.name = "Selection";
        s.pixels = r.mask;
        if (r.mask) d.put(kSelectionId, s);
        else d.put(kSelectionId, std::nullopt);
    }
}

bool by(DocJournal& j, Document& d, const std::vector<Id>& selected, int dx, int dy,
        const pixel::Px& background, const std::string& merge_key) {
    if (dx == 0 && dy == 0) return false;
    const auto p = plan(d, selected, background);   // by(): the layers, as linked
    if (!p) return false;
    const auto r = compute(*p, dx, dy);
    if (!r) return false;
    return j.run("Move", step_ids(*p), [&] { put(d, *p, *r); }, merge_key);
}

}  // namespace klimt::core::move
