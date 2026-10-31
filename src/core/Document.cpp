#include "core/Document.hpp"
#include "core/Paths.hpp"   // s035: the verb table
#include "core/Fade.hpp"   // s020: the verb table's fade
#include "core/Transfer.hpp"   // s022: the verb table's paste
#include "core/ColourRange.hpp"   // s024
#include "core/ColourChannels.hpp"   // s026
#include "core/Selection.hpp"   // s008: the verb table carries the selection verbs
#include "core/Paint.hpp"       // s008 m4: and the brush
#include "core/Move.hpp"        // s009 m1: and the Move tool
#include "core/Canvas.hpp"      // s009 m2: Crop, Canvas Size, Image Size
#include "core/Rulers.hpp"      // s031: the zero point
#include "core/Transform.hpp"   // s011: Free Transform, flips, rotations
#include "core/Gradient.hpp"    // s012: the Gradient tool
#include "core/Heal.hpp"        // s015 m3: the healing brushes
#include "core/Live.hpp"        // s018: live filters

#include <algorithm>
#include <set>

namespace klimt::core {

// ── blend modes ─────────────────────────────────────────────────────────────
const std::vector<BlendInfo>& blend_modes() {
    static const std::vector<BlendInfo> t = {
        {Blend::PassThrough,  "pass-through",  "Pass Through",  0},
        {Blend::Normal,       "normal",        "Normal",        1},
        {Blend::Dissolve,     "dissolve",      "Dissolve",      1},
        {Blend::Darken,       "darken",        "Darken",        2},
        {Blend::Multiply,     "multiply",      "Multiply",      2},
        {Blend::ColorBurn,    "color-burn",    "Color Burn",    2},
        {Blend::LinearBurn,   "linear-burn",   "Linear Burn",   2},
        {Blend::DarkerColor,  "darker-color",  "Darker Color",  2},
        {Blend::Lighten,      "lighten",       "Lighten",       3},
        {Blend::Screen,       "screen",        "Screen",        3},
        {Blend::ColorDodge,   "color-dodge",   "Color Dodge",   3},
        {Blend::LinearDodge,  "linear-dodge",  "Linear Dodge (Add)", 3},
        {Blend::LighterColor, "lighter-color", "Lighter Color", 3},
        {Blend::Overlay,      "overlay",       "Overlay",       4},
        {Blend::SoftLight,    "soft-light",    "Soft Light",    4},
        {Blend::HardLight,    "hard-light",    "Hard Light",    4},
        {Blend::VividLight,   "vivid-light",   "Vivid Light",   4},
        {Blend::LinearLight,  "linear-light",  "Linear Light",  4},
        {Blend::PinLight,     "pin-light",     "Pin Light",     4},
        {Blend::HardMix,      "hard-mix",      "Hard Mix",      4},
        {Blend::Difference,   "difference",    "Difference",    5},
        {Blend::Exclusion,    "exclusion",     "Exclusion",     5},
        {Blend::Subtract,     "subtract",      "Subtract",      5},
        {Blend::Divide,       "divide",        "Divide",        5},
        {Blend::Hue,          "hue",           "Hue",           6},
        {Blend::Saturation,   "saturation",    "Saturation",    6},
        {Blend::Color,        "color",         "Color",         6},
        {Blend::Luminosity,   "luminosity",    "Luminosity",    6},
    };
    return t;
}
std::string_view blend_key(Blend b) {
    for (const auto& i : blend_modes()) if (i.mode == b) return i.key;
    return {};
}
std::string_view blend_label(Blend b) {
    for (const auto& i : blend_modes()) if (i.mode == b) return i.label;
    return {};
}
std::optional<Blend> blend_from_key(std::string_view key) {
    for (const auto& i : blend_modes()) if (key == i.key) return i.mode;
    return std::nullopt;
}

// ── the store ───────────────────────────────────────────────────────────────
std::optional<Layer> Document::get(const Id& id) const {
    if (id == kSelectionId) return m_selection;
    if (is_channel(id)) {   // s016: a saved selection
        const auto c = m_channels.find(id);
        if (c == m_channels.end()) return std::nullopt;
        return c->second;
    }
    if (id == kImageId) {   // s009: the image, as a record
        Layer v;
        v.name = "Image";
        v.image = m_image_ptr;
        return v;
    }
    const auto it = m_items.find(id);
    if (it == m_items.end()) return std::nullopt;
    return it->second;
}

void Document::put(const Id& id, const std::optional<Layer>& v) {
    if (m_loading == 0 && m_door && !m_door()) ++m_strays;
    if (id == kSelectionId) {   // s008: the selection, apart from the tree
        m_selection = v;
        if (m_changed) m_changed();
        return;
    }
    if (is_channel(id)) {       // s016: a saved selection, apart from the tree
        if (v) m_channels[id] = *v;
        else   m_channels.erase(id);
        if (m_changed) m_changed();
        return;
    }
    if (id == kImageId) {       // s009: the image -- replaced, never removed
        if (v && v->image) {
            m_image_ptr = v->image;
            m_image = *v->image;
        }
        if (m_changed) m_changed();
        return;
    }
    if (v) m_items[id] = *v;
    else   m_items.erase(id);
    // A loaded "L<n>" keeps fresh_id() ahead of it.
    if (v && id.size() > 1 && id[0] == 'L' &&
        std::all_of(id.begin() + 1, id.end(), [](char c) { return c >= '0' && c <= '9'; }))
        m_next = std::max(m_next, std::stoi(id.substr(1)) + 1);
    if (m_changed) m_changed();
}

void Document::set_image(const Image& im) {
    if (m_loading == 0) ++m_strays;   // not a user choice: only a load sets it
    m_image = im;
    m_image_ptr = std::make_shared<const Image>(im);
    if (m_changed) m_changed();
}

std::vector<Document::Id> Document::children(const Id& parent) const {
    std::vector<Id> out;
    for (const auto& [id, l] : m_items) if (l.parent == parent) out.push_back(id);
    std::stable_sort(out.begin(), out.end(), [this](const Id& a, const Id& b) {
        return m_items.at(a).pos < m_items.at(b).pos;
    });
    return out;
}

std::vector<Document::Id> Document::panel(const Id& parent) const {
    auto out = children(parent);
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<Document::Id> Document::walk() const {
    std::vector<Id> out;
    std::function<void(const Id&)> go = [&](const Id& p) {
        for (const auto& id : panel(p)) {
            out.push_back(id);
            if (m_items.at(id).kind == Kind::Group) go(id);
        }
    };
    go({});
    return out;
}

std::vector<Document::Id> Document::subtree(const Id& id) const {
    std::vector<Id> out;
    if (!m_items.count(id)) return out;
    std::function<void(const Id&)> go = [&](const Id& p) {
        out.push_back(p);
        for (const auto& c : panel(p)) go(c);
    };
    go(id);
    return out;
}

bool Document::is_group(const Id& id) const {
    const auto it = m_items.find(id);
    return it != m_items.end() && it->second.kind == Kind::Group;
}

bool Document::within(const Id& id, const Id& ancestor) const {
    Id at = id;
    for (std::size_t guard = 0; guard <= m_items.size() && !at.empty(); ++guard) {
        if (at == ancestor) return true;
        const auto it = m_items.find(at);
        if (it == m_items.end()) return false;
        at = it->second.parent;
    }
    return false;
}

int Document::depth(const Id& id) const {
    int n = 0;
    auto it = m_items.find(id);
    while (it != m_items.end() && !it->second.parent.empty() && n <= static_cast<int>(m_items.size())) {
        ++n;
        it = m_items.find(it->second.parent);
    }
    return n;
}

bool Document::locked_all(const Id& id) const {
    Id at = id;
    for (std::size_t guard = 0; guard <= m_items.size() && !at.empty(); ++guard) {
        const auto it = m_items.find(at);
        if (it == m_items.end()) return false;
        if (it->second.locks & lock::All) return true;
        at = it->second.parent;
    }
    return false;
}

bool Document::shown(const Id& id) const {
    Id at = id;
    for (std::size_t guard = 0; guard <= m_items.size() && !at.empty(); ++guard) {
        const auto it = m_items.find(at);
        if (it == m_items.end() || !it->second.visible) return false;
        at = it->second.parent;
    }
    return true;
}

Document::Id Document::clip_base(const Id& id) const {
    const auto me = get(id);
    if (!me || !me->clipped) return {};
    const auto sibs = children(me->parent);   // bottom first
    auto it = std::find(sibs.begin(), sibs.end(), id);
    while (it != sibs.begin()) {
        --it;
        if (!m_items.at(*it).clipped) return *it;
    }
    return {};
}

void Document::replace(const Image& im, std::map<Id, Layer> layers, int next) {
    if (m_loading == 0) ++m_strays;   // not a user choice: only a load replaces
    m_image = im;
    m_image_ptr = std::make_shared<const Image>(im);
    m_items = std::move(layers);
    m_selection.reset();   // a new document starts with nothing selected
    m_channels.clear();    // s016: and no saved selections (a file's come after: replace_channels)
    m_next = std::max(1, next);
    for (const auto& [id, l] : m_items)   // a loaded "L<n>" keeps fresh_id() ahead of it
        if (id.size() > 1 && id[0] == 'L' && id.size() < 10 &&
            std::all_of(id.begin() + 1, id.end(), [](char c) { return c >= '0' && c <= '9'; }))
            m_next = std::max(m_next, std::stoi(id.substr(1)) + 1);
    if (m_changed) m_changed();
}

void Document::swap_parked(Parked& p) {
    if (m_loading == 0) ++m_strays;   // not a user choice: a tab switch, under Loading
    std::swap(m_items, p.items);
    std::swap(m_selection, p.selection);
    std::swap(m_channels, p.channels);
    std::swap(m_image, p.image);
    std::swap(m_image_ptr, p.image_ptr);
    if (!m_image_ptr) m_image_ptr = std::make_shared<const Image>(m_image);
    std::swap(m_next, p.next);
    if (m_changed) m_changed();
}

std::vector<Document::Id> Document::channel_order() const {
    std::vector<Id> v;
    for (const auto& [id, c] : m_channels) v.push_back(id);
    std::stable_sort(v.begin(), v.end(), [this](const Id& a, const Id& b) { return m_channels.at(a).pos < m_channels.at(b).pos; });
    return v;
}

void Document::replace_channels(std::map<Id, Layer> c) {
    if (m_loading == 0) ++m_strays;   // not a user choice: only a load replaces
    m_channels = std::move(c);
    if (m_changed) m_changed();
}

Document::Id Document::fresh_id() { return "L" + std::to_string(m_next++); }

std::string Document::check() const {
    std::map<Id, std::set<double>> places;
    for (const auto& [id, l] : m_items) {
        if (!l.parent.empty()) {
            const auto p = m_items.find(l.parent);
            if (p == m_items.end()) return id + ": parent " + l.parent + " is missing";
            if (p->second.kind != Kind::Group) return id + ": parent " + l.parent + " is not a group";
        }
        // No cycles: walking up must reach the top within count() steps.
        Id at = l.parent;
        std::size_t n = 0;
        while (!at.empty()) {
            if (++n > m_items.size() || at == id) return id + ": is inside itself";
            at = m_items.at(at).parent;
        }
        if (!places[l.parent].insert(l.pos).second) return id + ": shares its place with a sibling";
        if (l.opacity < 0 || l.opacity > 100) return id + ": opacity out of range";
        if (l.fill < 0 || l.fill > 100) return id + ": fill out of range";
        if (l.locks & ~lock::Mask) return id + ": unknown lock bits";
        if (l.blend == Blend::PassThrough && l.kind != Kind::Group) return id + ": Pass Through on a layer";
        if (l.mask) {   // s012
            if (!(l.mask->format() == mask_format()) || l.mask->width() != m_image.width || l.mask->height() != m_image.height)
                return id + ": a mask not at the image's size in the mask format";
        }
        if ((l.kind == Kind::Adjustment) != (l.adjust.kind != AdjustKind::None))
            return id + ": an adjustment where there is no adjustment layer, or none where there is";
        if (l.kind == Kind::Filter && (l.filters.size() != 1 || l.filters[0].f.kind == FilterKind::None))   // s030
            return id + ": a filter layer without exactly one filter";
        if ((l.kind == Kind::Path) != (l.path != nullptr))   // s035
            return id + ": a path where there is no path layer, or none where there is";
        if (l.kind == Kind::Adjustment && !adjust::refusal(l.adjust).empty())
            return id + ": an adjustment out of range (" + adjust::refusal(l.adjust) + ")";
        if (l.pixels) {
            if (l.kind != Kind::Pixel) return id + ": a group or an adjustment layer with pixels";
            if (!(l.pixels->format() == m_image.format) || l.pixels->width() != m_image.width ||
                l.pixels->height() != m_image.height)
                return id + ": pixels not at the image's size and format";
        }
    }
    // s009: the image record is the image.
    if (!m_image_ptr || !(*m_image_ptr == m_image)) return std::string("the image record is not the image");
    // s008: the selection's mask is at the image's size, in the mask format,
    // and holds no tile that is all unselected (that tile is absent).
    if (const auto* m = selection()) {
        if (!(m->format() == mask_format()) || m->width() != m_image.width || m->height() != m_image.height)
            return std::string("the selection is not at the image's size in the mask format");
        if (m->tile_count() == 0) return std::string("an empty selection is held (it should be none)");
    }
    return {};
}

// ── the verbs ───────────────────────────────────────────────────────────────
namespace layers {

namespace {
// A step's plan: the records it writes, each to its new value (nullopt: gone),
// in the order given (parents before children).
using Plan = std::vector<std::pair<Id, std::optional<Layer>>>;

std::string quoted(const Document& d, const Id& id) {
    const auto v = d.get(id);
    return "“" + (v && !v->name.empty() ? v->name : std::string("Untitled")) + "”";
}

// One door for every plan: the ids go in up front (a created id's BEFORE is
// simply absent), the writes happen inside run().
bool apply(DocJournal& j, Document& d, const std::string& label, const Plan& plan,
           const std::string& merge_key = {}) {
    if (plan.empty()) return false;
    std::vector<Id> ids;
    ids.reserve(plan.size());
    for (const auto& [id, v] : plan) ids.push_back(id);
    return j.run(label, ids, [&] { for (const auto& [id, v] : plan) d.put(id, v); }, merge_key);
}

// Renumber a parent's children to the panel order given (top first): pos n..1.
// Each record's new value is taken from `next` if the plan already holds it.
void place(const Document& d, std::map<Id, Layer>& next, const Id& parent, const std::vector<Id>& panel_order) {
    const auto n = panel_order.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Id& id = panel_order[i];
        Layer l = next.count(id) ? next[id] : *d.get(id);
        l.parent = parent;
        l.pos    = static_cast<double>(n - i);
        next[id] = l;
    }
}

// The plan from a map of new values: only records that change, parents first
// (by depth in the NEW tree, then walk order for a stable step).
Plan diff(const Document& d, const std::map<Id, Layer>& next, const std::vector<Id>& gone = {}) {
    Plan plan;
    std::vector<std::pair<int, Id>> order;
    for (const auto& [id, l] : next) {
        const auto old = d.get(id);
        if (old && *old == l) continue;
        int depth = 0;
        for (Id at = l.parent; !at.empty() && depth < 1000; ++depth) {
            const auto it = next.find(at);
            at = it != next.end() ? it->second.parent : (d.get(at) ? d.get(at)->parent : Id{});
        }
        order.emplace_back(depth, id);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [depth, id] : order) plan.emplace_back(id, next.at(id));
    for (const auto& id : gone) plan.emplace_back(id, std::nullopt);
    return plan;
}

// A selection made ready to act on: existing ids, no duplicates, none whose
// group is also in it (it rides along), in walk (panel) order.
std::vector<Id> tops(const Document& d, const std::vector<Id>& ids) {
    std::vector<Id> out;
    for (const auto& id : d.walk()) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
        bool inside_another = false;
        for (const auto& other : ids)
            if (other != id && d.get(other) && d.within(id, other)) { inside_another = true; break; }
        if (!inside_another) out.push_back(id);
    }
    return out;
}

std::string count_label(const std::string& verb, const Document& d, const std::vector<Id>& ids) {
    if (ids.size() == 1) return verb + " " + quoted(d, ids[0]);
    return verb + " " + std::to_string(ids.size()) + " Layers";
}

Id add_kind(DocJournal& j, Document& d, Kind kind, const std::string& name, const Id& above,
            const std::function<void(Layer&)>& extra = {}, const std::string& label_in = {}) {
    Id parent;
    std::size_t at = 0;   // panel index in parent
    if (const auto a = d.get(above)) {
        if (a->kind == Kind::Group) { parent = above; at = 0; }
        else {
            parent = a->parent;
            const auto sibs = d.panel(parent);
            at = static_cast<std::size_t>(std::find(sibs.begin(), sibs.end(), above) - sibs.begin());
        }
    }
    const Id made = d.fresh_id();
    Layer l;
    l.kind  = kind;
    l.name  = name;
    l.blend = kind == Kind::Group ? Blend::PassThrough : Blend::Normal;
    if (extra) extra(l);
    std::map<Id, Layer> next{{made, l}};
    auto sibs = d.panel(parent);
    sibs.insert(sibs.begin() + static_cast<long>(at), made);
    place(d, next, parent, sibs);
    const std::string label = !label_in.empty() ? label_in : kind == Kind::Group ? "New Group" : "New Layer";
    return apply(j, d, label + " " + "“" + name + "”", diff(d, next)) ? made : Id{};
}

// Set one field on one layer through the door.
template <class F>
bool edit(DocJournal& j, Document& d, const Id& id, const std::string& label, F&& f,
          const std::string& merge_key = {}) {
    auto v = d.get(id);
    if (!v) return false;
    f(*v);
    return apply(j, d, label, {{id, *v}}, merge_key);
}
}  // namespace

Id add_layer(DocJournal& j, Document& d, const std::string& name, const Id& above) {
    return add_kind(j, d, Kind::Pixel, name, above);
}
Id add_group(DocJournal& j, Document& d, const std::string& name, const Id& above) {
    return add_kind(j, d, Kind::Group, name, above);
}

// s013: an adjustment layer, with its mask, as one step.
Id add_adjustment(DocJournal& j, Document& d, AdjustKind k, const Id& above) {
    if (k == AdjustKind::None) return {};
    const std::string base(adjust::label(k));
    int n = 1;   // "Levels 1", "Levels 2" ...: one past the highest of its kind
    for (const auto& [id, l] : d.snapshot())
        if (l.kind == Kind::Adjustment && l.name.rfind(base + " ", 0) == 0) {
            const std::string tail = l.name.substr(base.size() + 1);
            if (!tail.empty() && tail.size() < 6 && std::all_of(tail.begin(), tail.end(), [](char c) { return c >= '0' && c <= '9'; }))
                n = std::max(n, std::stoi(tail) + 1);
        }
    const std::string name = base + " " + std::to_string(n);
    const auto* sel = d.selection();
    const int w = d.image().width, h = d.image().height;
    return add_kind(j, d, Kind::Adjustment, name, above, [&](Layer& l) {
        l.adjust = adjust::fresh(k);
        l.mask = std::make_shared<const pixel::TileMap>(sel ? select::inverted(sel, d.mask_format(), w, h)
                                                            : pixel::TileMap(d.mask_format(), w, h));
    }, "New Adjustment Layer");
}

// s035: PATH LAYERS.
namespace {
// The selection a path layer's outline makes (nullopt: none -- an empty path).
std::optional<Layer> selection_of(const Document& d, const path::Path& p) {
    if (p.empty()) return std::nullopt;
    auto m = paths::coverage(p, d.mask_format(), d.image().width, d.image().height);
    if (select::empty(m)) return std::nullopt;
    Layer v;
    v.name = "Selection";
    v.pixels = std::make_shared<const pixel::TileMap>(std::move(m));
    return v;
}
}  // namespace

Id add_path_layer(DocJournal& j, Document& d, const path::Path& p, const Id& above, const std::string& label) {
    int n = 1;
    for (const auto& [id, l] : d.snapshot())
        if (l.kind == Kind::Path && l.name.rfind("Path ", 0) == 0) {
            const std::string tail = l.name.substr(5);
            if (!tail.empty() && tail.size() < 6 && std::all_of(tail.begin(), tail.end(), [](char c) { return c >= '0' && c <= '9'; }))
                n = std::max(n, std::stoi(tail) + 1);
        }
    const std::string name = "Path " + std::to_string(n);
    const Id made = add_kind(j, d, Kind::Path, name, above, [&](Layer& l) {
        l.path = std::make_shared<const path::Path>(p);
    }, label);
    return made;
}

bool set_path_layer(DocJournal& j, Document& d, const Id& id, const path::Path& p, const std::string& label) {
    auto l = d.get(id);
    if (!l || l->kind != Kind::Path || d.locked_all(id)) return false;
    if (l->path && *l->path == p) return false;
    l->path = std::make_shared<const path::Path>(p);
    if (!l->path_select) return j.run(label, {id}, [&] { d.put(id, *l); });
    const auto sel = selection_of(d, p);   // the selection follows, in the same step
    return j.run(label, {id, kSelectionId}, [&] { d.put(id, *l); d.put(kSelectionId, sel); });
}

bool set_path_switches(DocJournal& j, Document& d, const Id& id, bool mask, bool select) {
    auto l = d.get(id);
    if (!l || l->kind != Kind::Path || d.locked_all(id)) return false;
    if (l->path_mask == mask && l->path_select == select) return false;
    const bool takes = select && !l->path_select;   // switched on: the selection becomes the path's area now
    std::string label = mask != l->path_mask ? (mask ? "Path as Mask" : "Path Mask Off")
                                             : (select ? "Path as Selection" : "Path Selection Off");
    l->path_mask = mask;
    l->path_select = select;
    if (!takes) return j.run(label + " “" + l->name + "”", {id}, [&] { d.put(id, *l); });
    const auto sel = selection_of(d, l->path ? *l->path : path::Path{});
    return j.run(label + " “" + l->name + "”", {id, kSelectionId}, [&] { d.put(id, *l); d.put(kSelectionId, sel); });
}

bool set_path_hints(DocJournal& j, Document& d, const Id& id, const path::Hints& h, const std::string& label) {
    auto l = d.get(id);
    if (!l || l->kind != Kind::Path || d.locked_all(id)) return false;
    const path::Hints none;
    if ((l->hints ? *l->hints : none) == h) return false;
    l->hints = h.empty() ? nullptr : std::make_shared<const path::Hints>(h);
    return j.run(label, {id}, [&] { d.put(id, *l); });
}

bool add_path_stroke(DocJournal& j, Document& d, const Id& id, const path::Stroke& s) {
    if (s.pts.empty() || !(s.radius > 0)) return false;
    const auto l = d.get(id);
    if (!l || l->kind != Kind::Path) return false;
    path::Hints h = l->hints ? *l->hints : path::Hints{};
    h.strokes.push_back(s);
    return set_path_hints(j, d, id, h, s.figure ? "Figure Stroke" : "Background Stroke");
}

// s030: a filter layer, with its mask, as one step.
Id add_filter_layer(DocJournal& j, Document& d, const Filter& f_in, const Id& above) {
    const Filter f = filter::normalised(f_in);
    if (!filter::livable(f.kind)) return {};   // s043 m4: not Displace
    const std::string base(filter::label(f.kind));
    int n = 1;
    for (const auto& [id, l] : d.snapshot())
        if (l.kind == Kind::Filter && l.name.rfind(base + " ", 0) == 0) {
            const std::string tail = l.name.substr(base.size() + 1);
            if (!tail.empty() && tail.size() < 6 && std::all_of(tail.begin(), tail.end(), [](char c) { return c >= '0' && c <= '9'; }))
                n = std::max(n, std::stoi(tail) + 1);
        }
    const auto* sel = d.selection();
    const int w = d.image().width, h = d.image().height;
    return add_kind(j, d, Kind::Filter, base + " " + std::to_string(n), above, [&](Layer& l) {
        LiveFilter lf;
        lf.f = f;
        l.filters = {lf};
        l.mask = std::make_shared<const pixel::TileMap>(sel ? select::inverted(sel, d.mask_format(), w, h)
                                                            : pixel::TileMap(d.mask_format(), w, h));
    }, "New Filter Layer");
}

namespace {
std::optional<Layer> filter_set(const Document& d, const Id& id, const Filter& f) {
    const auto l = d.get(id);
    if (!l || l->kind != Kind::Filter || d.locked_all(id) || l->filters.size() != 1) return std::nullopt;
    const Filter n = filter::normalised(f);
    if (n.kind != l->filters[0].f.kind || n == l->filters[0].f) return std::nullopt;
    Layer v = *l;
    v.filters[0].f = n;
    return v;
}
std::string filter_label(const Document& d, const Id& id) {
    const auto l = d.get(id);
    return std::string(filter::label(l->filters[0].f.kind)) + " of “" + l->name + "”";
}
}  // namespace

bool set_filter_layer(DocJournal& j, Document& d, const Id& id, const Filter& f) {
    const auto v = filter_set(d, id, f);
    if (!v) return false;
    return j.run(filter_label(d, id), {id}, [&] { d.put(id, *v); }, "filterlayer:" + id);
}

void filter_layer_begin(DocJournal& j, Document& d, const Id& id) {
    const auto l = d.get(id);
    if (l && l->kind == Kind::Filter && !d.locked_all(id) && l->filters.size() == 1) j.begin(filter_label(d, id), {id});
}

void filter_layer_to(DocJournal& j, Document& d, const Id& id, const Filter& f) {
    if (!j.in_gesture()) return;
    const auto v = filter_set(d, id, f);
    if (!v) return;
    j.run("Filter", {id}, [&] { d.put(id, *v); });   // inside the gesture: just writes
}

bool filter_layer_end(DocJournal& j) { return j.commit(); }

std::string not_pixels(const Document& d, const Id& id) {
    const auto l = d.get(id);
    if (!l) return "no layer";
    const std::string q = "“" + l->name + "”";
    if (l->kind == Kind::Group) return q + " is a group -- pick a layer in it";
    if (l->kind == Kind::Adjustment) return q + " is an adjustment layer -- it has no pixels";
    if (l->kind == Kind::Filter) return q + " is a filter layer -- it has no pixels";   // s030
    if (l->kind == Kind::Path) return q + " is a path layer -- edit it with the Pen (P); it has no pixels";   // s035
    return {};
}

Id group(DocJournal& j, Document& d, const std::vector<Id>& ids, const std::string& name) {
    const auto sel = tops(d, ids);
    if (sel.empty()) return {};
    const Id parent = d.get(sel[0])->parent;
    // Where the group goes: the first selected row's place among the siblings
    // that stay.
    std::vector<Id> stay;
    std::size_t at = 0;
    for (const auto& id : d.panel(parent)) {
        if (id == sel[0]) at = stay.size();
        if (std::find(sel.begin(), sel.end(), id) == sel.end()) stay.push_back(id);
    }
    const Id made = d.fresh_id();
    Layer g;
    g.kind  = Kind::Group;
    g.name  = name;
    g.blend = Blend::PassThrough;
    std::map<Id, Layer> next{{made, g}};
    stay.insert(stay.begin() + static_cast<long>(at), made);
    place(d, next, parent, stay);
    place(d, next, made, sel);
    // A clip that crossed into the group from outside is released (Photoshop
    // does the same): the base it clipped to stayed behind.
    for (const auto& id : sel) {
        const Id base = d.clip_base(id);
        if (next[id].clipped && std::find(sel.begin(), sel.end(), base) == sel.end())
            next[id].clipped = false;
    }
    return apply(j, d, "Group Layers", diff(d, next)) ? made : Id{};
}

bool ungroup(DocJournal& j, Document& d, const Id& group) {
    const auto g = d.get(group);
    if (!g || g->kind != Kind::Group) return false;
    std::vector<Id> sibs;
    for (const auto& id : d.panel(g->parent)) {
        if (id == group) for (const auto& c : d.panel(group)) sibs.push_back(c);
        else sibs.push_back(id);
    }
    std::map<Id, Layer> next;
    place(d, next, g->parent, sibs);
    return apply(j, d, "Ungroup " + quoted(d, group), diff(d, next, {group}));
}

bool remove(DocJournal& j, Document& d, const std::vector<Id>& ids) {
    const auto sel = tops(d, ids);
    if (sel.empty()) return false;
    std::vector<Id> gone;
    for (const auto& id : sel) for (const auto& s : d.subtree(id)) gone.push_back(s);
    if (gone.size() >= d.count()) return false;   // never an empty document
    // Children before their groups in the step, so undo (backwards) puts the
    // group back first.
    std::reverse(gone.begin(), gone.end());
    Plan plan;
    for (const auto& id : gone) plan.emplace_back(id, std::nullopt);
    return apply(j, d, count_label("Delete", d, sel), plan);
}

std::vector<Id> duplicate(DocJournal& j, Document& d, const std::vector<Id>& ids) {
    const auto sel = tops(d, ids);
    if (sel.empty()) return {};
    std::map<Id, Layer> next;
    std::map<Id, Id> copy_of;   // original -> copy (roots and descendants)
    std::vector<Id> roots;
    for (const auto& id : sel) {
        for (const auto& s : d.subtree(id)) {
            const Id c = d.fresh_id();
            copy_of[s] = c;
            Layer l = *d.get(s);
            if (s == id) l.name += " copy";
            else         l.parent = copy_of.at(l.parent);
            next[c] = l;
        }
        roots.push_back(copy_of.at(id));
    }
    // Each root copy goes just above its original, in the original's group.
    std::set<Id> parents;
    for (const auto& id : sel) parents.insert(d.get(id)->parent);
    for (const auto& p : parents) {
        std::vector<Id> sibs;
        for (const auto& id : d.panel(p)) {
            if (std::find(sel.begin(), sel.end(), id) != sel.end()) sibs.push_back(copy_of.at(id));
            sibs.push_back(id);
        }
        place(d, next, p, sibs);
    }
    if (!apply(j, d, count_label("Duplicate", d, sel), diff(d, next))) return {};
    return roots;
}

// s022: layers from elsewhere (another document), as values: each with its
// OLD id and its old parent; `roots` the old ids of the tops, panel order.
// They come in under fresh ids, the roots just above `above` (in its group;
// a group: at its top; none: the document's top), as ONE step.
namespace {
// The roots go into `parent` at panel index `at`.
std::vector<Id> insert_at(DocJournal& j, Document& d, const std::string& label,
                          const std::vector<std::pair<Id, Layer>>& layers, const std::vector<Id>& roots,
                          const Id& parent, std::size_t at) {
    if (layers.empty() || roots.empty()) return {};
    std::map<Id, Id> fresh;
    for (const auto& [old, l] : layers) fresh[old] = d.fresh_id();
    std::map<Id, Layer> next;
    std::vector<Id> made_roots;
    for (const auto& [old, l] : layers) {
        Layer v = l;
        const bool root = std::find(roots.begin(), roots.end(), old) != roots.end();
        if (!root) {
            const auto it = fresh.find(v.parent);
            if (it == fresh.end()) return {};   // a child whose group didn't come
            v.parent = it->second;
        }
        next[fresh.at(old)] = v;
    }
    for (const auto& r : roots) {
        if (!fresh.count(r)) return {};
        made_roots.push_back(fresh.at(r));
    }
    auto sibs = d.panel(parent);
    sibs.insert(sibs.begin() + static_cast<std::ptrdiff_t>(std::min(at, sibs.size())), made_roots.begin(), made_roots.end());
    place(d, next, parent, sibs);
    if (!apply(j, d, label, diff(d, next))) return {};
    return made_roots;
}
}  // namespace

std::vector<Id> insert(DocJournal& j, Document& d, const std::string& label,
                       const std::vector<std::pair<Id, Layer>>& layers, const std::vector<Id>& roots, const Id& above) {
    Id parent;
    std::size_t at = 0;
    if (const auto a = d.get(above)) {
        if (a->kind == Kind::Group) parent = above;
        else {
            parent = a->parent;
            const auto sibs = d.panel(parent);
            at = static_cast<std::size_t>(std::find(sibs.begin(), sibs.end(), above) - sibs.begin());
        }
    }
    return insert_at(j, d, label, layers, roots, parent, at);
}

namespace {
// The drop's plan, or nothing if it's refused. move() writes it; can_move()
// only asks -- the panel asks on every pointer motion to show or refuse a drop.
std::optional<std::map<Id, Layer>> plan_move(const Document& d, const std::vector<Id>& ids,
                                             const Id& target, Where where) {
    const auto sel = tops(d, ids);
    const auto t = d.get(target);
    if (sel.empty() || !t) return std::nullopt;
    if (where == Where::Inside && t->kind != Kind::Group) return std::nullopt;
    const Id parent = where == Where::Inside ? target : t->parent;
    for (const auto& id : sel) {
        if (id == target) return std::nullopt;                            // onto itself
        if (!parent.empty() && d.within(parent, id)) return std::nullopt; // into its own contents
    }
    std::vector<Id> sibs;
    for (const auto& id : d.panel(parent))
        if (std::find(sel.begin(), sel.end(), id) == sel.end()) sibs.push_back(id);
    std::size_t at = 0;
    if (where != Where::Inside) {
        at = static_cast<std::size_t>(std::find(sibs.begin(), sibs.end(), target) - sibs.begin());
        if (where == Where::Below) ++at;
    }
    sibs.insert(sibs.begin() + static_cast<long>(at), sel.begin(), sel.end());
    std::map<Id, Layer> next;
    place(d, next, parent, sibs);
    return next;
}
}  // namespace

bool can_move(const Document& d, const std::vector<Id>& ids, const Id& target, Where where) {
    const auto next = plan_move(d, ids, target, where);
    return next && !diff(d, *next).empty();
}

bool move(DocJournal& j, Document& d, const std::vector<Id>& ids, const Id& target, Where where) {
    const auto next = plan_move(d, ids, target, where);
    if (!next) return false;
    return apply(j, d, count_label("Move", d, tops(d, ids)), diff(d, *next));
}

// s022: Alt-drag -- copies of the layers (and what is inside them) where the
// drop says, ONE step. Onto (or into) itself is fine: the copy lands beside.
std::vector<Id> copy_to(DocJournal& j, Document& d, const std::vector<Id>& ids, const Id& target, Where where) {
    const auto t = d.get(target);
    if (!t || (where == Where::Inside && t->kind != Kind::Group)) return {};
    auto b = transfer::take(d, ids);
    if (b.roots.empty()) return {};
    for (auto& [id, l] : b.layers)
        if (std::find(b.roots.begin(), b.roots.end(), id) != b.roots.end()) l.name += " copy";
    const Id parent = where == Where::Inside ? target : t->parent;
    std::size_t at = 0;
    if (where != Where::Inside) {
        const auto sibs = d.panel(parent);
        at = static_cast<std::size_t>(std::find(sibs.begin(), sibs.end(), target) - sibs.begin()) + (where == Where::Below ? 1 : 0);
    }
    return insert_at(j, d, count_label("Duplicate", d, b.roots), b.layers, b.roots, parent, at);
}

namespace {
bool step(DocJournal& j, Document& d, const Id& id, int dir, const std::string& verb) {
    const auto l = d.get(id);
    if (!l) return false;
    auto sibs = d.panel(l->parent);
    const auto i = std::find(sibs.begin(), sibs.end(), id) - sibs.begin();
    const auto k = i + dir;
    if (k < 0 || k >= static_cast<long>(sibs.size())) return false;
    std::swap(sibs[static_cast<std::size_t>(i)], sibs[static_cast<std::size_t>(k)]);
    std::map<Id, Layer> next;
    place(d, next, l->parent, sibs);
    return apply(j, d, verb + " " + quoted(d, id), diff(d, next));
}
}  // namespace

bool arrange(DocJournal& j, Document& d, const Id& parent, const std::vector<Id>& order,
             const std::string& label) {
    if (!parent.empty() && !d.is_group(parent)) return false;
    auto now = d.panel(parent), want = order;
    std::sort(now.begin(), now.end());
    std::sort(want.begin(), want.end());
    if (now != want) return false;   // not exactly its contents: it changed
    std::map<Id, Layer> next;
    place(d, next, parent, order);
    return apply(j, d, label, diff(d, next));
}

bool natural_less(const std::string& a, const std::string& b) {
    std::size_t i = 0, k = 0;
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    while (i < a.size() && k < b.size()) {
        if (digit(a[i]) && digit(b[k])) {
            std::size_t i2 = i, k2 = k;
            while (i2 < a.size() && a[i2] == '0') ++i2;   // leading zeros don't count
            while (k2 < b.size() && b[k2] == '0') ++k2;
            std::size_t ie = i2, ke = k2;
            while (ie < a.size() && digit(a[ie])) ++ie;
            while (ke < b.size() && digit(b[ke])) ++ke;
            if (ie - i2 != ke - k2) return ie - i2 < ke - k2;   // fewer digits: smaller
            for (std::size_t n = 0; n < ie - i2; ++n)
                if (a[i2 + n] != b[k2 + n]) return a[i2 + n] < b[k2 + n];
            i = ie;
            k = ke;
            continue;
        }
        const char x = lower(a[i]), y = lower(b[k]);
        if (x != y) return x < y;
        ++i;
        ++k;
    }
    if (i < a.size() || k < b.size()) return k < b.size();   // a prefix comes first
    return a < b;   // equal ignoring case and zeros: a stable, total tie-break
}

std::vector<Id> sorted_by_name(std::vector<Named> rows) {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const Named& x, const Named& y) { return natural_less(x.name, y.name); });
    std::vector<Id> out;
    for (const auto& r : rows) out.push_back(r.id);
    return out;
}

bool raise(DocJournal& j, Document& d, const Id& id) { return step(j, d, id, -1, "Bring Forward"); }
bool lower(DocJournal& j, Document& d, const Id& id) { return step(j, d, id, +1, "Send Backward"); }

bool rename(DocJournal& j, Document& d, const Id& id, const std::string& name) {
    return edit(j, d, id, "Rename " + quoted(d, id), [&](Layer& l) { l.name = name; });
}

bool set_visible(DocJournal& j, Document& d, const Id& id, bool visible) {
    return edit(j, d, id, (visible ? "Show " : "Hide ") + quoted(d, id),
                [&](Layer& l) { l.visible = visible; });
}

bool set_blend(DocJournal& j, Document& d, const Id& id, Blend mode) {
    const auto l = d.get(id);
    if (!l || d.locked_all(id)) return false;
    if (mode == Blend::PassThrough && l->kind != Kind::Group) return false;
    return edit(j, d, id, "Blend Mode of " + quoted(d, id), [&](Layer& v) { v.blend = mode; });
}

bool set_clipped(DocJournal& j, Document& d, const Id& id, bool clipped) {
    if (d.locked_all(id)) return false;
    return edit(j, d, id, (clipped ? "Clip " : "Release Clip ") + quoted(d, id),
                [&](Layer& l) { l.clipped = clipped; });
}

bool set_locks(DocJournal& j, Document& d, const Id& id, std::uint8_t locks) {
    return edit(j, d, id, ((locks & lock::Mask) ? "Lock " : "Unlock ") + quoted(d, id),
                [&](Layer& l) { l.locks = locks & lock::Mask; });
}

bool set_opacity(DocJournal& j, Document& d, const Id& id, int percent) {
    if (d.locked_all(id)) return false;
    return edit(j, d, id, "Opacity of " + quoted(d, id),
                [&](Layer& l) { l.opacity = std::clamp(percent, 0, 100); }, "opacity:" + id);
}

bool set_fill(DocJournal& j, Document& d, const Id& id, int percent) {
    if (d.locked_all(id)) return false;
    return edit(j, d, id, "Fill of " + quoted(d, id),
                [&](Layer& l) { l.fill = std::clamp(percent, 0, 100); }, "fill:" + id);
}

void opacity_begin(DocJournal& j, Document& d, const Id& id) {
    if (d.get(id) && !d.locked_all(id)) j.begin("Opacity of " + quoted(d, id), {id});
}

void opacity_to(DocJournal& j, Document& d, const Id& id, int percent) {
    if (!j.in_gesture()) return;   // the drag was refused at begin
    auto v = d.get(id);
    if (!v || v->opacity == std::clamp(percent, 0, 100)) return;
    v->opacity = std::clamp(percent, 0, 100);
    j.run("Opacity", {id}, [&] { d.put(id, *v); });   // inside the gesture: just writes
}

bool opacity_end(DocJournal& j) { return j.commit(); }

// ── many at once ────────────────────────────────────────────────────────────
namespace {
// The selection's layers this edit may touch (existing, in walk order, and
// passing `ok`), the label ("Opacity of “A”" / "Opacity of 3 Layers") and
// the merge key ("opacity:L3,L5").
struct Many { std::vector<Id> ids; std::string label, key; };
template <class Ok>
Many many(const Document& d, const std::vector<Id>& sel, const std::string& verb, const std::string& key, Ok ok) {
    Many m;
    for (const auto& id : d.walk())
        if (std::find(sel.begin(), sel.end(), id) != sel.end() && ok(id)) m.ids.push_back(id);
    if (m.ids.empty()) return m;
    m.label = m.ids.size() == 1 ? verb + " " + quoted(d, m.ids[0])
                                : verb + " " + std::to_string(m.ids.size()) + " Layers";
    m.key = key;
    for (const auto& id : m.ids) m.key += (m.key.back() == ':' ? "" : ",") + id;
    return m;
}
template <class F>
bool edit_many(DocJournal& j, Document& d, const Many& m, F&& f, bool merge) {
    if (m.ids.empty()) return false;
    Plan plan;
    for (const auto& id : m.ids) {
        Layer l = *d.get(id);
        f(l);
        plan.emplace_back(id, l);
    }
    return apply(j, d, m.label, plan, merge ? m.key : std::string{});
}
}  // namespace

bool set_blend(DocJournal& j, Document& d, const std::vector<Id>& ids, Blend mode) {
    const auto m = many(d, ids, "Blend Mode of", "blend:", [&](const Id& id) {
        return !d.locked_all(id) && (mode != Blend::PassThrough || d.is_group(id));
    });
    return edit_many(j, d, m, [&](Layer& l) { l.blend = mode; }, false);
}

bool set_locks(DocJournal& j, Document& d, const std::vector<Id>& ids, std::uint8_t locks) {
    const auto m = many(d, ids, (locks & lock::Mask) ? "Lock" : "Unlock", "locks:", [](const Id&) { return true; });
    return edit_many(j, d, m, [&](Layer& l) { l.locks = locks & lock::Mask; }, false);
}

bool set_opacity(DocJournal& j, Document& d, const std::vector<Id>& ids, int percent) {
    const auto m = many(d, ids, "Opacity of", "opacity:", [&](const Id& id) { return !d.locked_all(id); });
    return edit_many(j, d, m, [&](Layer& l) { l.opacity = std::clamp(percent, 0, 100); }, true);
}

bool set_fill(DocJournal& j, Document& d, const std::vector<Id>& ids, int percent) {
    const auto m = many(d, ids, "Fill of", "fill:", [&](const Id& id) { return !d.locked_all(id); });
    return edit_many(j, d, m, [&](Layer& l) { l.fill = std::clamp(percent, 0, 100); }, true);
}

void opacity_begin(DocJournal& j, Document& d, const std::vector<Id>& ids) {
    const auto m = many(d, ids, "Opacity of", "opacity:", [&](const Id& id) { return !d.locked_all(id); });
    if (!m.ids.empty()) j.begin(m.label, m.ids);
}

void opacity_to(DocJournal& j, Document& d, const std::vector<Id>& ids, int percent) {
    if (!j.in_gesture()) return;   // the drag was refused at begin
    const auto m = many(d, ids, "Opacity", "opacity:", [&](const Id& id) { return !d.locked_all(id); });
    Plan plan;
    for (const auto& id : m.ids) {
        Layer l = *d.get(id);
        if (l.opacity == std::clamp(percent, 0, 100)) continue;
        l.opacity = std::clamp(percent, 0, 100);
        plan.emplace_back(id, l);
    }
    if (!plan.empty()) apply(j, d, "Opacity", plan);   // inside the gesture: just writes
}

// ── pixels ──────────────────────────────────────────────────────────────────
namespace {
bool pixels_writable(const Document& d, const Id& id) {
    const auto l = d.get(id);
    return l && l->kind == Kind::Pixel && !d.locked_all(id) && !(l->locks & lock::Pixels);
}

bool write_pixels(DocJournal& j, Document& d, const Id& id, const std::string& label,
                  const std::function<pixel::TileMap(const pixel::TileMap&)>& edit) {
    if (!pixels_writable(d, id)) return false;
    Layer l = *d.get(id);
    const pixel::TileMap before = l.pixels ? *l.pixels : d.blank();
    pixel::TileMap after = edit(before);
    // Nothing changed: the edit handed back a map sharing every tile.
    bool same = true;
    for (int r = 0; r < after.rows() && same; ++r)
        for (int c = 0; c < after.cols() && same; ++c) same = after.same_tile(before, c, r);
    if (same) return false;
    l.pixels = after.tile_count() ? std::make_shared<const pixel::TileMap>(std::move(after)) : nullptr;
    return apply(j, d, label, {{id, l}});
}
}  // namespace

bool fill(DocJournal& j, Document& d, const Id& id, const pixel::Rect& r, const pixel::Px& colour) {
    const auto l = d.get(id);
    if (!l) return false;
    const pixel::Format f = d.image().format;
    const std::size_t a = static_cast<std::size_t>(f.channels() - 1);
    if (l->locks & lock::Alpha) {
        // Lock transparent pixels: colour only where there is something.
        return write_pixels(j, d, id, "Fill " + quoted(d, id), [&](const pixel::TileMap& m) {
            return m.paint(r, [&](int, int, const pixel::Px& old) {
                if (!f.alpha || old.c[a] <= 0.f) return old;
                pixel::Px p = colour;
                p.c[a] = old.c[a];
                return p;
            });
        });
    }
    return write_pixels(j, d, id, "Fill " + quoted(d, id),
                        [&](const pixel::TileMap& m) { return m.fill(r, colour); });
}

bool clear(DocJournal& j, Document& d, const Id& id, const pixel::Rect& r) {
    const auto l = d.get(id);
    if (!l || (l->locks & lock::Alpha)) return false;   // clearing IS changing alpha
    return write_pixels(j, d, id, "Clear " + quoted(d, id),
                        [&](const pixel::TileMap& m) { return m.fill(r, pixel::transparent()); });
}

Id neighbour(const Document& d, const Id& id) {
    const auto l = d.get(id);
    if (!l) return {};
    const auto sibs = d.panel(l->parent);
    const auto it = std::find(sibs.begin(), sibs.end(), id);
    if (it + 1 != sibs.end()) return *(it + 1);
    if (it != sibs.begin()) return *(it - 1);
    return l->parent;
}

void load_fixture(Document& d) {
    Document::Loading load(d);
    d.set_image({.width = 600, .height = 400, .format = pixel::Format{pixel::Depth::U8, pixel::Mode::RGB, true}});
    const auto add = [&](const Id& id, Kind k, const char* name, const Id& parent, double pos) {
        Layer l;
        l.kind = k;
        l.name = name;
        l.parent = parent;
        l.pos = pos;
        l.blend = k == Kind::Group ? Blend::PassThrough : Blend::Normal;
        d.put(id, l);
    };
    add("L1", Kind::Pixel, "Background", "", 1);
    add("L2", Kind::Group, "Group", "", 2);
    add("L3", Kind::Pixel, "B", "L2", 1);
    add("L4", Kind::Pixel, "A", "L2", 2);
    add("L5", Kind::Pixel, "Top", "", 3);
    // Background painted white, "A" a red square (pixels for the gate's
    // pixel verbs to find).
    auto bg = *d.get("L1");
    bg.pixels = std::make_shared<const pixel::TileMap>(d.blank().fill({0, 0, 600, 400}, pixel::rgba(1, 1, 1, 1)));
    d.put("L1", bg);
    auto a = *d.get("L4");
    a.pixels = std::make_shared<const pixel::TileMap>(d.blank().fill({100, 100, 200, 150}, pixel::rgba(1, 0, 0, 1)));
    d.put("L4", a);
}

// s012: a mask on `id` hiding everything but (120, 110) 60 x 60, LOADED.
void load_mask(Document& d, const Id& id) {
    Document::Loading load(d);
    auto l = *d.get(id);
    l.mask = std::make_shared<const pixel::TileMap>(select::inverted(
        std::make_shared<pixel::TileMap>(select::rect_mask(d.mask_format(), d.image().width, d.image().height, {120, 110, 60, 60})).get(),
        d.mask_format(), d.image().width, d.image().height));
    d.put(id, l);
}

void load_adjustment(Document& d, AdjustKind k) {
    {
        Document::Loading load(d);
        Layer l;
        l.kind = Kind::Adjustment;
        l.name = std::string(adjust::label(k)) + " 1";
        l.pos = 4;
        l.adjust = adjust::fresh(k);
        d.put("L6", l);
    }
    load_mask(d, "L6");
}

void load_guides(Document& d, const std::vector<Guide>& g) {   // s032
    Document::Loading load(d);
    Image im = d.image();
    im.guides = g;
    d.set_image(im);
}

void load_path_layer(Document& d, const path::Path& p, bool mask, bool select) {   // s035
    Document::Loading load(d);
    Layer l;
    l.kind = Kind::Path;
    l.name = "Path 1";
    l.pos = 4;
    l.path = std::make_shared<const path::Path>(p);
    l.path_mask = mask;
    l.path_select = select;
    d.put("L6", l);
}

void load_filter_layer(Document& d, FilterKind k) {   // s030
    Document::Loading load(d);
    Layer l;
    l.kind = Kind::Filter;
    l.name = std::string(filter::label(k)) + " 1";
    l.pos = 4;
    LiveFilter lf;
    lf.f = filter::fresh(k);
    l.filters = {lf};
    l.mask = std::make_shared<const pixel::TileMap>(d.mask_format(), d.image().width, d.image().height);
    d.put("L6", l);
}

// A selection already there (a verb's setup): LOADED, not a step.
void load_selection(Document& d, const pixel::Rect& r) {
    Document::Loading load(d);
    Layer v;
    v.name = "Selection";
    v.pixels = std::make_shared<const pixel::TileMap>(select::rect_mask(d.mask_format(), d.image().width, d.image().height, r));
    d.put(kSelectionId, v);
}

const std::vector<Verb>& verbs() {
    static const std::vector<Verb> table = {
        {"add_layer",     [](DocJournal& j, Document& d) { add_layer(j, d, "New", "L4"); }},
        {"add_group",     [](DocJournal& j, Document& d) { add_group(j, d, "Set", "L5"); }},
        {"group",         [](DocJournal& j, Document& d) { group(j, d, {"L5", "L1"}); }},
        {"ungroup",       [](DocJournal& j, Document& d) { ungroup(j, d, "L2"); }},
        {"remove",        [](DocJournal& j, Document& d) { remove(j, d, {"L2"}); }},
        {"duplicate",     [](DocJournal& j, Document& d) { duplicate(j, d, {"L2", "L1"}); }},
        {"move",          [](DocJournal& j, Document& d) { move(j, d, {"L5", "L1"}, "L3", Where::Above); }},
        {"move_inside",   [](DocJournal& j, Document& d) { move(j, d, {"L1"}, "L2", Where::Inside); }},
        {"raise",         [](DocJournal& j, Document& d) { raise(j, d, "L3"); }},
        {"lower",         [](DocJournal& j, Document& d) { lower(j, d, "L5"); }},
        {"arrange",       [](DocJournal& j, Document& d) { arrange(j, d, "L2", {"L3", "L4"}, "Sort"); }},
        {"rename",        [](DocJournal& j, Document& d) { rename(j, d, "L4", "Sky"); }},
        {"set_visible",   [](DocJournal& j, Document& d) { set_visible(j, d, "L2", false); }},
        {"set_blend",     [](DocJournal& j, Document& d) { set_blend(j, d, "L4", Blend::Multiply); }},
        {"set_clipped",   [](DocJournal& j, Document& d) { set_clipped(j, d, "L4", true); }},
        {"set_locks",     [](DocJournal& j, Document& d) { set_locks(j, d, "L1", lock::All); }},
        {"set_opacity",   [](DocJournal& j, Document& d) { set_opacity(j, d, "L5", 40); }},
        {"set_fill",      [](DocJournal& j, Document& d) { set_fill(j, d, "L5", 60); }},
        {"opacity_many",  [](DocJournal& j, Document& d) { set_opacity(j, d, std::vector<Id>{"L5", "L4", "L1"}, 25); }},
        {"blend_many",    [](DocJournal& j, Document& d) { set_blend(j, d, std::vector<Id>{"L3", "L4"}, Blend::Screen); }},
        {"locks_many",    [](DocJournal& j, Document& d) { set_locks(j, d, std::vector<Id>{"L1", "L2"}, lock::Position); }},
        {"opacity_drag_many", [](DocJournal& j, Document& d) {
             opacity_begin(j, d, std::vector<Id>{"L3", "L4"}); opacity_to(j, d, std::vector<Id>{"L3", "L4"}, 50);
             opacity_to(j, d, std::vector<Id>{"L3", "L4"}, 10); opacity_end(j); }},
        {"fill",          [](DocJournal& j, Document& d) { fill(j, d, "L3", {10, 10, 300, 300}, pixel::rgba(0, 0, 1, 1)); }},
        {"clear",         [](DocJournal& j, Document& d) { clear(j, d, "L4", {150, 150, 50, 50}); }},
        {"opacity_drag",  [](DocJournal& j, Document& d) {
             opacity_begin(j, d, "L4"); opacity_to(j, d, "L4", 80); opacity_to(j, d, "L4", 35); opacity_end(j); }},
        // s008 -- the selection (core/Selection) and pixels through it.
        {"select_rect",   [](DocJournal& j, Document& d) { select::rect(j, d, {50, 50, 300, 200}); }},
        {"select_ellipse", [](DocJournal& j, Document& d) { select::ellipse(j, d, 40, 30, 380, 290); }},
        {"select_lasso",  [](DocJournal& j, Document& d) {
             select::polygon(j, d, {{10, 10}, {400, 40}, {300, 350}, {20, 200}}); }},
        {"select_add",    [](DocJournal& j, Document& d) { select::rect(j, d, {200, 200, 100, 100}, select::Op::Add); },
             [](Document& d) { load_selection(d, {0, 0, 100, 100}); }},
        {"select_subtract", [](DocJournal& j, Document& d) { select::ellipse(j, d, 100, 100, 300, 300, select::Op::Subtract); },
             [](Document& d) { load_selection(d, {0, 0, 600, 400}); }},
        {"select_intersect", [](DocJournal& j, Document& d) { select::rect(j, d, {200, 200, 300, 150}, select::Op::Intersect); },
             [](Document& d) { load_selection(d, {0, 0, 300, 300}); }},
        {"select_all",    [](DocJournal& j, Document& d) { select::all(j, d); }},
        {"deselect",      [](DocJournal& j, Document& d) { select::none(j, d); },
             [](Document& d) { load_selection(d, {0, 0, 600, 400}); }},
        {"select_invert", [](DocJournal& j, Document& d) { select::invert(j, d); },
             [](Document& d) { load_selection(d, {10, 10, 100, 100}); }},
        {"select_feather", [](DocJournal& j, Document& d) { select::feather(j, d, 8); },
             [](Document& d) { load_selection(d, {100, 100, 200, 100}); }},
        {"fill_selection", [](DocJournal& j, Document& d) { fill_selection(j, d, "L4", pixel::rgba(0, 0.5f, 1, 1)); },
             [](Document& d) { load_selection(d, {80, 80, 240, 200}); }},
        {"clear_selection", [](DocJournal& j, Document& d) { clear_selection(j, d, "L4", pixel::rgba(1, 1, 1, 1)); },
             [](Document& d) { load_selection(d, {150, 120, 100, 60}); }},
        // s008 m4 -- a stroke is ONE step, however many dabs.
        {"brush",         [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 40; b.colour = pixel::rgba(0, 0, 1, 1);
             paint::stroke(j, d, "L5", {{50, 50}, {200, 80}, {400, 300}}, b); }},
        {"brush_in_selection", [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 60; b.colour = pixel::rgba(0, 1, 0, 1);
             paint::stroke(j, d, "L5", {{0, 200}, {600, 200}}, b); },
             [](Document& d) { load_selection(d, {200, 0, 200, 400}); }},
        {"eraser",        [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 30; b.erase = true;
             paint::stroke(j, d, "L4", {{100, 120}, {300, 230}}, b); }},
        // s009 m3: a pen stroke, light to heavy.
        {"brush_pen",     [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 50; b.pressure_opacity = true; b.colour = pixel::rgba(0, 0, 1, 1);
             paint::stroke_pen(j, d, "L5", {{40, 300, 0.1f}, {300, 200, 0.6f}, {560, 100, 1.f}}, b); }},
        // s012 m1: build-up -- a flow under 100% over the same line.
        {"brush_flow",    [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 40; b.flow = 0.3; b.colour = pixel::rgba(1, 0, 1, 1);
             paint::stroke_pen(j, d, "L5", {{50, 100, 1.f}, {500, 100, 1.f}, {50, 110, 1.f}}, b); }},
        // s017: a flat, angled tip and a chalk one.
        {"brush_flat",    [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 50; b.roundness = 0.2; b.angle = 30; b.colour = pixel::rgba(1, 0, 0, 1);
             paint::stroke(j, d, "L5", {{50, 300}, {500, 100}}, b); }},
        {"brush_chalk",   [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 60; b.tip = paint::Tip::Chalk; b.colour = pixel::rgba(0, 0, 0, 1);
             paint::stroke(j, d, "L5", {{50, 200}, {550, 200}}, b); }},
        // s017: Smudge -- dragging the red square's edge outwards.
        {"smudge",        [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 40; b.strength = 0.7;
             paint::stroke_smudge(j, d, "L4", {{250, 175, 1.f}, {360, 175, 1.f}}, b); }},
        // s015: the Clone Stamp -- from the layer itself, and from All Layers onto an empty one.
        {"clone_layer",   [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 40; std::string why;
             b.source = paint::clone_source(d, "L4", false, why); b.dx = -200; b.dy = -100;
             paint::stroke(j, d, "L4", {{350, 250}, {450, 300}}, b); }},
        {"clone_all",     [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 40; std::string why;
             b.source = paint::clone_source(d, "L5", true, why); b.dx = -250; b.dy = -150;
             paint::stroke(j, d, "L5", {{400, 300}, {450, 320}}, b); }},
        // s015 m3: healing -- with a source, and Spot (it finds one).
        {"heal",          [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 30;
             heal::stroke(j, d, "L4", {{120, 120, 1.f}, {160, 130, 1.f}}, b, false, 100, 60); }},
        {"heal_spot",     [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 30;
             heal::stroke(j, d, "L4", {{298, 150, 1.f}}, b, true, 0, 0, true); }},
        // s016: Quick Mask -- a black stroke masks part of a selection.
        {"quick_mask",    [](DocJournal& j, Document& d) {
             paint::Brush b; b.size = 40; b.colour = pixel::Px{};
             paint::stroke_selection(j, d, {{120, 150, 1.f}, {200, 150, 1.f}}, b); },
             [](Document& d) { load_selection(d, {100, 100, 200, 100}); }},
        // s018: live filters -- added, and one hidden.
        {"add_live",      [](DocJournal& j, Document& d) { add_live(j, d, "L4", filter::fresh(FilterKind::GaussianBlur)); }},
        {"toggle_live",   [](DocJournal& j, Document& d) { toggle_live(j, d, "L4", 0); },
             [](Document& d) {
                 Document::Loading load(d);
                 auto l = *d.get("L4"); l.filters.push_back({filter::fresh(FilterKind::GaussianBlur), true}); d.put("L4", l);
             }},
        // s018 m3: a filter mask hiding the blur on the left half.
        {"filter_mask",   [](DocJournal& j, Document& d) {
             set_filter_mask(j, d, "L4", std::make_shared<const pixel::TileMap>(
                 select::rect_mask(d.mask_format(), d.image().width, d.image().height, {0, 0, 200, 400}))); },
             [](Document& d) {
                 Document::Loading load(d);
                 auto l = *d.get("L4"); l.filters.push_back({filter::fresh(FilterKind::GaussianBlur), true}); d.put("L4", l);
             }},
        // s019: layer styles -- a drop shadow and a stroke on the square.
        {"set_styles",    [](DocJournal& j, Document& d) {
             Styles s; s.drop.on = true; s.stroke.on = true; set_styles(j, d, "L4", s); }},
        {"styles_drag",   [](DocJournal& j, Document& d) {
             Styles s; s.drop.on = true;
             styles_begin(j, d, "L4");
             for (int k = 5; k <= 25; k += 5) { s.drop.distance = k; styles_to(j, d, "L4", s); }
             styles_end(j); }},
        {"paste_styles",  [](DocJournal& j, Document& d) {
             Styles s; s.outer.on = true; set_styles_on(j, d, {"L4", "L5", "L2"}, s, "Paste Layer Style"); }},
        // s022: layers from another document (here, a copy of this one's group).
        {"paste",         [](DocJournal& j, Document& d) { paste(j, d, transfer::take(d, {"L2"}), "L5"); }},
        {"copy_to",       [](DocJournal& j, Document& d) { copy_to(j, d, {"L4", "L5"}, "L1", Where::Above); }},
        // s026: the colour channels -- Red loaded, Blue duplicated.
        {"load_channel_k", [](DocJournal& j, Document& d) { select::load_colour_channel(j, d, 0); }},
        {"dup_channel_k",  [](DocJournal& j, Document& d) { select::duplicate_colour_channel(j, d, 2); }},
        // s024: Select > Color Range -- the red square.
        {"colour_range",  [](DocJournal& j, Document& d) {
             select::ColourRange r; r.samples = {{1, 0, 0}}; select::colour_range(j, d, r); }},
        // s024: a mask on a group.
        {"group_mask",    [](DocJournal& j, Document& d) { add_mask(j, d, "L2", MaskFrom::HideAll); }},
        // s023: a live filter's blending.
        {"live_blend",    [](DocJournal& j, Document& d) { set_live_blend(j, d, "L4", 0, 40, Blend::Multiply); },
             [](Document& d) {
                 Document::Loading load(d);
                 auto l = *d.get("L4"); l.filters.push_back({filter::fresh(FilterKind::GaussianBlur), true}); d.put("L4", l);
             }},
        // s020: Edit > Fade -- the square half-way back to nothing.
        {"fade",          [](DocJournal& j, Document& d) {
             layers::fade(j, d, "L4", nullptr, d.get("L4")->pixels, 0.5, Blend::Multiply, "Brush"); }},
        // s016: saved selections -- save, load (adding), delete.
        {"save_channel",  [](DocJournal& j, Document& d) { select::save_channel(j, d); },
             [](Document& d) { load_selection(d, {100, 100, 80, 60}); }},
        {"load_channel",  [](DocJournal& j, Document& d) { select::load_channel(j, d, "@channel/1", select::Op::Add); },
             [](Document& d) {
                 load_selection(d, {10, 10, 50, 50});
                 Document::Loading load(d);
                 Layer c; c.name = "Alpha 1";
                 c.mask = std::make_shared<const pixel::TileMap>(select::rect_mask(d.mask_format(), d.image().width, d.image().height, {200, 200, 90, 90}));
                 d.put("@channel/1", c);
             }},
        // s016: Patch -- the selection healed from 120 px to the right.
        {"patch",         [](DocJournal& j, Document& d) { heal::patch(j, d, "L4", 120, 0); },
             [](Document& d) { load_selection(d, {100, 120, 60, 60}); }},
        // s009 m1: the Move tool -- a layer, a group's contents, a lifted selection.
        {"move_layer",    [](DocJournal& j, Document& d) { move::by(j, d, {"L4"}, 37, -20, pixel::rgba(1, 1, 1, 1)); }},
        {"move_group",    [](DocJournal& j, Document& d) { move::by(j, d, {"L2"}, -300, 5, pixel::rgba(1, 1, 1, 1)); }},
        {"move_selection", [](DocJournal& j, Document& d) { move::by(j, d, {"L4"}, 120, 60, pixel::rgba(1, 1, 1, 1)); },
             [](Document& d) { load_selection(d, {150, 120, 100, 80}); }},
        // s011 m1: transforms -- the whole layer, or the selection's pixels.
        {"flip_h",        [](DocJournal& j, Document& d) { xform::flip(j, d, {"L4"}, true, pixel::rgba(1, 1, 1, 1)); }},
        {"rotate_90",     [](DocJournal& j, Document& d) { xform::rotate(j, d, {"L2"}, 90, pixel::rgba(1, 1, 1, 1)); }},
        {"free_transform", [](DocJournal& j, Document& d) {
             xform::apply(j, d, {"L4"}, xform::Affine::about({200, 175}, xform::Affine::rotate(0.3) * xform::Affine::scale(1.4, 0.8)),
                          "Free Transform", pixel::rgba(1, 1, 1, 1)); }},
        {"transform_selection", [](DocJournal& j, Document& d) {
             xform::apply(j, d, {"L4"}, xform::Affine::about({200, 160}, xform::Affine::scale(2, 2)), "Free Transform",
                          pixel::rgba(1, 1, 1, 1)); },
             [](Document& d) { load_selection(d, {150, 120, 100, 80}); }},
        // s012 m4: the Gradient tool, on pixels and on a mask.
        {"gradient",      [](DocJournal& j, Document& d) {
             gradient::Spec s; s.a = {50, 50}; s.b = {500, 300}; s.from = pixel::rgba(1, 1, 0, 1); s.to = pixel::rgba(0, 0, 1, 1);
             gradient::apply(j, d, "L5", s); }},
        {"gradient_radial_selection", [](DocJournal& j, Document& d) {
             gradient::Spec s; s.shape = gradient::Shape::Radial; s.a = {200, 200}; s.b = {300, 200};
             s.from = pixel::rgba(1, 0, 0, 1); s.to = pixel::rgba(1, 0, 0, 0); gradient::apply(j, d, "L5", s); },
             [](Document& d) { load_selection(d, {100, 100, 250, 200}); }},
        {"gradient_mask", [](DocJournal& j, Document& d) {
             gradient::Spec s; s.a = {100, 0}; s.b = {300, 0}; s.to.c[0] = 1; gradient::apply(j, d, "L4", s, true); },
             [](Document& d) { load_mask(d, "L4"); }},
        // s012 m2: layer masks.
        {"add_mask",      [](DocJournal& j, Document& d) { add_mask(j, d, "L4", MaskFrom::RevealSelection); },
             [](Document& d) { load_selection(d, {120, 110, 60, 60}); }},
        {"invert_mask",   [](DocJournal& j, Document& d) { invert_mask(j, d, "L4"); }, [](Document& d) { load_mask(d, "L4"); }},
        {"mask_off",      [](DocJournal& j, Document& d) { set_mask_on(j, d, "L4", false); }, [](Document& d) { load_mask(d, "L4"); }},
        {"mask_edges",    [](DocJournal& j, Document& d) {   // s036
             MaskEdges e; e.density = 60; e.feather = 8; set_mask_edges(j, d, "L4", e); }, [](Document& d) { load_mask(d, "L4"); }},
        {"apply_mask",    [](DocJournal& j, Document& d) { apply_mask(j, d, "L4"); }, [](Document& d) { load_mask(d, "L4"); }},
        {"delete_mask",   [](DocJournal& j, Document& d) { delete_mask(j, d, "L4"); }, [](Document& d) { load_mask(d, "L4"); }},
        {"move_masked",   [](DocJournal& j, Document& d) { move::by(j, d, {"L4"}, 40, 30, pixel::rgba(1, 1, 1, 1)); },
             [](Document& d) { load_mask(d, "L4"); }},
        {"crop_masked",   [](DocJournal& j, Document& d) { canvas::crop(j, d, {50, 40, 400, 300}, pixel::rgba(1, 1, 1, 1)); },
             [](Document& d) { load_mask(d, "L4"); }},
        // s011 m4: Select > Modify.
        {"select_expand", [](DocJournal& j, Document& d) { select::modify(j, d, select::Modify::Expand, 12); },
             [](Document& d) { load_selection(d, {150, 120, 100, 80}); }},
        {"select_contract", [](DocJournal& j, Document& d) { select::modify(j, d, select::Modify::Contract, 6); },
             [](Document& d) { load_selection(d, {150, 120, 100, 80}); }},
        {"select_border", [](DocJournal& j, Document& d) { select::modify(j, d, select::Modify::Border, 10); },
             [](Document& d) { load_selection(d, {150, 120, 100, 80}); }},
        {"select_smooth", [](DocJournal& j, Document& d) { select::modify(j, d, select::Modify::Smooth, 3); },
             [](Document& d) { load_selection(d, {150, 120, 100, 80}); }},
        // s011 m3: the whole canvas turned, rotated, straightened.
        {"canvas_turn_cw", [](DocJournal& j, Document& d) { xform::turn(j, d, xform::Turn::Clockwise); },
             [](Document& d) { load_selection(d, {10, 10, 100, 50}); }},
        {"canvas_flip_v", [](DocJournal& j, Document& d) { xform::turn(j, d, xform::Turn::FlipVertical); }},
        {"canvas_rotate", [](DocJournal& j, Document& d) { xform::rotate_canvas(j, d, 30, pixel::rgba(1, 1, 1, 1)); }},
        {"straighten",    [](DocJournal& j, Document& d) { xform::straighten(j, d, {0, 0}, {200, 15}, pixel::rgba(1, 1, 1, 1)); }},
        // s009 m2: the canvas -- the image itself is a record, so these are steps.
        {"crop",          [](DocJournal& j, Document& d) { canvas::crop(j, d, {50, 40, 400, 300}, pixel::rgba(1, 1, 1, 1)); }},
        {"crop_selected", [](DocJournal& j, Document& d) { canvas::crop(j, d, {150, 120, 100, 80}, pixel::rgba(1, 1, 1, 1)); },
             [](Document& d) { load_selection(d, {150, 120, 100, 80}); }},
        {"canvas_size",   [](DocJournal& j, Document& d) { canvas::canvas_size(j, d, 800, 300, 1, 2, pixel::rgba(1, 1, 1, 1)); },
             [](Document& d) { load_selection(d, {10, 10, 100, 100}); }},
        {"image_size",    [](DocJournal& j, Document& d) { canvas::image_size(j, d, 300, 200, 144); },
             [](Document& d) { load_selection(d, {10, 10, 100, 100}); }},
        // s010: pixels beyond the image -- kept by a crop, given back by Reveal All.
        {"crop_delete",   [](DocJournal& j, Document& d) { canvas::crop(j, d, {200, 0, 400, 400}, pixel::rgba(1, 1, 1, 1), true); }},
        {"reveal_all",    [](DocJournal& j, Document& d) { canvas::reveal_all(j, d); },
             [](Document& d) {
                 Document::Loading load(d);
                 auto l = *d.get("L4");
                 l.pixels = std::make_shared<const pixel::TileMap>(
                     canvas::reframed(*l.pixels, 600, 400, -250, 0, pixel::transparent()));   // the square half off the left
                 d.put("L4", l);
             }},
        {"resolution",    [](DocJournal& j, Document& d) { canvas::image_size(j, d, 600, 400, 300); }},
        // s013: adjustment layers -- made, edited (a press, a drag), carried
        // by the canvas verbs; and Image > Adjustments on the pixels.
        {"add_adjustment", [](DocJournal& j, Document& d) { add_adjustment(j, d, AdjustKind::Levels, "L4"); },
             [](Document& d) { load_selection(d, {120, 110, 60, 60}); }},
        {"add_adjustment_top", [](DocJournal& j, Document& d) { add_adjustment(j, d, AdjustKind::Curves); }},
        {"set_adjust",    [](DocJournal& j, Document& d) {
             Adjust a = adjust::fresh(AdjustKind::HueSat); a.hue = 120; a.saturation = -30; set_adjust(j, d, "L6", a); },
             [](Document& d) { load_adjustment(d, AdjustKind::HueSat); }},
        {"adjust_drag",   [](DocJournal& j, Document& d) {
             Adjust a = adjust::fresh(AdjustKind::Curves);
             adjust_begin(j, d, "L6");
             for (double y = 0.5; y <= 0.8; y += 0.1) { a.curves[0] = {{0, 0}, {0.5, y}, {1, 1}}; adjust_to(j, d, "L6", a); }
             adjust_end(j); },
             [](Document& d) { load_adjustment(d, AdjustKind::Curves); }},
        {"crop_adjustment", [](DocJournal& j, Document& d) { canvas::crop(j, d, {50, 40, 400, 300}, pixel::rgba(1, 1, 1, 1)); },
             [](Document& d) { load_adjustment(d, AdjustKind::Levels); }},
        {"turn_adjustment", [](DocJournal& j, Document& d) { xform::turn(j, d, xform::Turn::Clockwise); },
             [](Document& d) { load_adjustment(d, AdjustKind::BrightCon); }},
        {"apply_adjust",  [](DocJournal& j, Document& d) {
             Adjust a = adjust::fresh(AdjustKind::HueSat); a.hue = 120; a.lightness = 20; apply_adjust(j, d, "L4", a); },
             [](Document& d) { load_selection(d, {150, 120, 100, 80}); }},
        // s031: the rulers' zero point.
        {"ruler_origin", [](DocJournal& j, Document& d) { canvas::set_ruler_origin(j, d, 100, 50); }},
        // s035: path layers.
        {"path_layer_new",  [](DocJournal& j, Document& d) { add_path_layer(j, d, paths::rect_path(40, 30, 200, 160), "L4"); }},
        {"path_layer_set",  [](DocJournal& j, Document& d) { set_path_layer(j, d, "L6", paths::ellipse_path(120, 100, 60, 40), "Move Anchor Point"); },
             [](Document& d) { load_path_layer(d, paths::rect_path(40, 30, 200, 160)); }},
        {"path_layer_set_selecting", [](DocJournal& j, Document& d) { set_path_layer(j, d, "L6", paths::ellipse_path(120, 100, 60, 40), "Add Anchor Point"); },
             [](Document& d) { load_path_layer(d, paths::rect_path(40, 30, 200, 160), false, true); }},
        {"path_as_mask",    [](DocJournal& j, Document& d) { set_path_switches(j, d, "L6", true, false); },
             [](Document& d) { load_path_layer(d, paths::ellipse_path(120, 100, 60, 40)); }},
        {"path_as_selection", [](DocJournal& j, Document& d) { set_path_switches(j, d, "L6", false, true); },
             [](Document& d) { load_path_layer(d, paths::ellipse_path(120, 100, 60, 40)); }},
        {"path_selection",  [](DocJournal& j, Document& d) { paths::to_selection(j, d, "L6", select::Op::Add, 2); },
             [](Document& d) { load_path_layer(d, paths::ellipse_path(100, 100, 50, 40)); load_selection(d, {300, 200, 50, 50}); }},
        {"path_make",       [](DocJournal& j, Document& d) { paths::from_selection(j, d, 2.0, "L4"); },
             [](Document& d) { load_selection(d, {150, 120, 100, 80}); }},
        {"path_isolate",    [](DocJournal& j, Document& d) { paths::isolate(j, d, "L6", 8); },
             [](Document& d) { load_path_layer(d, paths::rect_path(90, 90, 310, 260)); }},
        // s037: Isolate's strokes.
        {"path_stroke",     [](DocJournal& j, Document& d) { add_path_stroke(j, d, "L6", {false, 6, {{100, 100}, {140, 100}}}); },
             [](Document& d) { load_path_layer(d, paths::rect_path(90, 90, 310, 260)); }},
        {"path_strokes_clear", [](DocJournal& j, Document& d) { set_path_hints(j, d, "L6", {}, "Clear Isolate Strokes"); },
             [](Document& d) {
                 load_path_layer(d, paths::rect_path(90, 90, 310, 260));
                 Document::Loading load(d);
                 auto l = *d.get("L6");
                 path::Hints h;
                 h.strokes = {{true, 8, {{150, 150}}}};
                 l.hints = std::make_shared<const path::Hints>(h);
                 d.put("L6", l);
             }},
        {"crop_paths",      [](DocJournal& j, Document& d) { canvas::crop(j, d, {50, 40, 400, 300}, pixel::rgba(1, 1, 1, 1)); },
             [](Document& d) { load_path_layer(d, paths::ellipse_path(100, 100, 50, 40), true); }},
        // s032: guides.
        {"add_guide",    [](DocJournal& j, Document& d) { canvas::add_guide(j, d, {true, 150.4}); }},
        {"move_guide",   [](DocJournal& j, Document& d) { canvas::move_guide(j, d, 0, 120); },
             [](Document& d) { load_guides(d, {{false, 80}}); }},
        {"move_guide_off", [](DocJournal& j, Document& d) { canvas::move_guide(j, d, 0, -40); },   // dragged off: removed
             [](Document& d) { load_guides(d, {{true, 50}, {false, 80}}); }},
        {"remove_guide", [](DocJournal& j, Document& d) { canvas::remove_guide(j, d, 0); },
             [](Document& d) { load_guides(d, {{true, 300}}); }},
        {"clear_guides", [](DocJournal& j, Document& d) { canvas::clear_guides(j, d); },
             [](Document& d) { load_guides(d, {{true, 10}, {false, 20}}); }},
        {"crop_guides",  [](DocJournal& j, Document& d) { canvas::crop(j, d, {50, 40, 400, 300}, pixel::rgba(1, 1, 1, 1)); },
             [](Document& d) { load_guides(d, {{true, 100}, {false, 100}}); }},
        // s030: filter layers -- made (through a selection, at the top), set, dragged.
        {"add_filter_layer", [](DocJournal& j, Document& d) { add_filter_layer(j, d, filter::fresh(FilterKind::GaussianBlur), "L4"); },
             [](Document& d) { load_selection(d, {120, 110, 60, 60}); }},
        {"add_filter_layer_top", [](DocJournal& j, Document& d) { add_filter_layer(j, d, filter::fresh(FilterKind::UnsharpMask)); }},
        {"set_filter_layer", [](DocJournal& j, Document& d) {
             Filter f = filter::fresh(FilterKind::GaussianBlur); f.radius = 9; set_filter_layer(j, d, "L6", f); },
             [](Document& d) { load_filter_layer(d, FilterKind::GaussianBlur); }},
        {"filter_layer_drag", [](DocJournal& j, Document& d) {
             Filter f = filter::fresh(FilterKind::HighPass);
             filter_layer_begin(j, d, "L6");
             for (double r = 4; r <= 12; r += 2) { f.radius = r; filter_layer_to(j, d, "L6", f); }
             filter_layer_end(j); },
             [](Document& d) { load_filter_layer(d, FilterKind::HighPass); }},
        // s014: the Filter menu on the pixels -- through a selection, and whole.
        {"apply_filter_blur", [](DocJournal& j, Document& d) { apply_filter(j, d, "L4", filter::fresh(FilterKind::GaussianBlur)); },
             [](Document& d) { load_selection(d, {50, 120, 100, 80}); }},   // across the square's edge
        {"apply_filter_noise", [](DocJournal& j, Document& d) { apply_filter(j, d, "L4", filter::fresh(FilterKind::AddNoise)); }},
        {"move_nudge",    [](DocJournal& j, Document& d) {
             move::by(j, d, {"L4"}, 1, 0, pixel::rgba(1, 1, 1, 1), "nudge");
             move::by(j, d, {"L4"}, 0, 10, pixel::rgba(1, 1, 1, 1), "nudge"); }},
    };
    return table;
}

}  // namespace layers
}  // namespace klimt::core
