// core::merge -- Merge Down, Merge Visible, Flatten Image, Stamp Visible
// (s096; include/core/Merge.hpp, docs/merge.md).
#include "core/Merge.hpp"

#include "core/File.hpp"
#include "core/Transfer.hpp"

#include <algorithm>
#include <set>

namespace klimt::core::merge {

namespace {

std::string curly(const std::string& s) { return "“" + s + "”"; }

// A record that draws pixels, its content stripped to them: what a merged
// layer is -- a plain pixel layer.
Layer plain(Layer l, std::shared_ptr<const pixel::TileMap> px) {
    l.kind = Kind::Pixel;
    l.pixels = std::move(px);
    l.mask = nullptr;
    l.mask_on = true;
    l.mask_linked = true;
    l.edges = {};
    l.adjust = {};
    l.filters.clear();
    l.filter_mask = nullptr;
    l.styles = {};
    l.path = nullptr;
    l.path_mask = false;
    l.path_select = false;
    l.hints = nullptr;
    l.text = nullptr;
    l.smart = nullptr;
    l.fill = 100;
    return l;
}

// Merge Visible: what goes -- every shown layer, and a shown group once all
// it holds goes too. Children before their groups (undo puts groups back first).
std::vector<Id> visible_gone(const Document& d) {
    std::set<Id> gone;
    auto order = d.walk();                     // a group before its contents
    std::reverse(order.begin(), order.end());  // contents first
    for (const auto& id : order) {
        if (!d.shown(id)) continue;
        if (!d.is_group(id)) { gone.insert(id); continue; }
        bool all = true;
        for (const auto& c : d.subtree(id)) if (c != id && !gone.count(c)) { all = false; break; }
        if (all) gone.insert(id);
    }
    std::vector<Id> out;
    for (const auto& id : order) if (gone.count(id)) out.push_back(id);
    return out;
}

bool mode_ok(const Document& d) {
    const auto m = d.image().format.mode;
    return m == pixel::Mode::RGB || m == pixel::Mode::Gray;
}

}  // namespace

const char* label(How h) {
    switch (h) {
        case How::Down:    return "Merge Down";
        case How::Visible: return "Merge Visible";
        case How::Flatten: return "Flatten Image";
        case How::Stamp:   return "Stamp Visible";
    }
    return "Merge";
}

Id below(const Document& d, const Id& picked) {
    const auto l = d.get(picked);
    if (!l) return {};
    const auto sibs = d.children(l->parent);   // paint order: bottom first
    const auto it = std::find(sibs.begin(), sibs.end(), picked);
    if (it == sibs.end() || it == sibs.begin()) return {};
    return *(it - 1);
}

std::string refusal(const Document& d, How how, const Id& picked) {
    if (d.count() == 0) return "no image";
    if (!mode_ok(d)) return "RGB and Grayscale pictures only for now -- Image > Mode > RGB Colour first";
    if (how == How::Flatten) return {};
    if (how == How::Visible || how == How::Stamp) {
        bool any = false;
        for (const auto& id : d.walk()) if (!d.is_group(id) && d.shown(id)) { any = true; break; }
        if (!any) return "nothing is showing";
        if (how == How::Visible)
            for (const auto& id : visible_gone(d))
                if (d.locked_all(id)) return curly(d.get(id)->name) + " is locked";
        return {};
    }
    // Merge Down
    const auto up = d.get(picked);
    if (!up) return "pick a layer first";
    if (up->kind == Kind::Group) return curly(up->name) + " is a group -- Merge Visible or Flatten Image takes it";
    if (up->kind == Kind::Path) return curly(up->name) + " is a path -- it draws nothing to merge";
    const Id lo_id = below(d, picked);
    if (lo_id.empty()) return "nothing below " + curly(up->name) + " in its group";
    const auto lo = d.get(lo_id);
    if (!draws_pixels(lo->kind))
        return curly(lo->name) + " below isn't a pixel layer -- it merges into pixels only";
    if (!d.shown(picked)) return curly(up->name) + " is hidden -- show it first";
    if (!d.shown(lo_id)) return curly(lo->name) + " is hidden -- show it first";
    if (d.locked_all(picked)) return curly(up->name) + " is locked";
    if (d.locked_all(lo_id) || (lo->locks & lock::Pixels)) return curly(lo->name) + " is locked";
    return {};
}

std::map<Id, Layer> source(const Document& d, How how, const Id& picked) {
    if (how != How::Down) return d.snapshot();   // the compositor skips what's hidden
    std::map<Id, Layer> src;
    const Id lo_id = below(d, picked);
    auto up = d.get(picked);
    auto lo = d.get(lo_id);
    if (!up || !lo) return src;
    // The pair alone, at the top level: the lower one plain (its opacity,
    // blend, clipping and Blend If stay on the result, not in its pixels);
    // the upper one as it is -- clipped, it clips to the lower one.
    lo->parent.clear();
    lo->opacity = 100;
    lo->blend = Blend::Normal;
    lo->clipped = false;
    lo->blend_if = {};
    lo->visible = true;
    up->parent.clear();
    up->visible = true;
    src[lo_id] = *lo;
    src[picked] = *up;
    return src;
}

std::optional<std::shared_ptr<const pixel::TileMap>> render(const Image& im, const std::map<Id, Layer>& src,
                                                            const formats::Progress& p) {
    auto flat = file::flatten(im, src, p);
    if (!flat) return std::nullopt;
    if (flat->format() == im.format) return std::make_shared<const pixel::TileMap>(std::move(*flat));
    return std::make_shared<const pixel::TileMap>(transfer::converted(*flat, im.format));
}

int hidden_dropped(const Document& d) {
    int n = 0;
    for (const auto& id : d.walk()) if (!d.is_group(id) && !d.shown(id)) ++n;
    return n;
}

Id land(DocJournal& j, Document& d, How how, const Id& picked, std::shared_ptr<const pixel::TileMap> px) {
    if (!px || !refusal(d, how, picked).empty()) return {};
    if (px->width() != d.image().width || px->height() != d.image().height || px->format() != d.image().format) return {};

    if (how == How::Stamp)
        return layers::add_layer_of(j, d, Kind::Pixel, "Merged", picked,
                                    [&](Layer& l) { l.pixels = px; }, "Stamp Visible");

    if (how == How::Down) {
        const Id lo_id = below(d, picked);
        const auto up = d.get(picked);
        const Layer merged = plain(*d.get(lo_id), px);
        const bool ok = j.run(std::string("Merge Down ") + curly(up->name), {picked, lo_id}, [&] {
            d.put(picked, std::nullopt);
            d.put(lo_id, merged);
        });
        return ok ? lo_id : Id{};
    }

    // Merge Visible / Flatten Image: the ones that go, and the new layer at
    // the top level -- in the place of the topmost top-level layer that goes.
    std::vector<Id> gone;
    if (how == How::Flatten) { gone = d.walk(); std::reverse(gone.begin(), gone.end()); }
    else gone = visible_gone(d);
    const std::set<Id> going(gone.begin(), gone.end());

    const Id nid = d.fresh_id();
    Layer nl;
    nl.kind = Kind::Pixel;
    nl.name = how == How::Flatten ? "Background" : "Merged";
    nl.pixels = px;

    std::vector<Id> top;   // the new top level, panel order (top first)
    bool placed = false;
    for (const auto& id : d.panel("")) {
        if (going.count(id)) {
            if (!placed) { top.push_back(nid); placed = true; }
            continue;
        }
        top.push_back(id);
    }
    if (!placed) top.insert(top.begin(), nid);

    std::map<Id, Layer> next;
    const auto n = top.size();
    for (std::size_t i = 0; i < n; ++i) {
        Layer l = top[i] == nid ? nl : *d.get(top[i]);
        l.parent.clear();
        l.pos = static_cast<double>(n - i);
        next[top[i]] = l;
    }
    std::vector<Id> ids = gone;
    for (const auto& [id, l] : next) if (id == nid || !(d.get(id) == std::optional<Layer>(l))) ids.push_back(id);
    const bool ok = j.run(label(how), ids, [&] {
        for (const auto& id : gone) d.put(id, std::nullopt);
        for (const auto& id : ids) if (next.count(id)) d.put(id, next.at(id));
    });
    return ok ? nid : Id{};
}

Id run(DocJournal& j, Document& d, How how, const Id& picked) {
    if (!refusal(d, how, picked).empty()) return {};
    auto px = render(d.image(), source(d, how, picked));
    if (!px) return {};
    return land(j, d, how, picked, *px);
}

}  // namespace klimt::core::merge
