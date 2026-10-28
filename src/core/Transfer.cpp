// core::transfer -- layers from one document into another (s022).
#include "core/Transfer.hpp"
#include "core/Canvas.hpp"   // reframed

#include <algorithm>

namespace klimt::core::transfer {

namespace {
pixel::Px convert_px(const pixel::Px& p, const pixel::Format& from, const pixel::Format& to) {
    pixel::Px out{};
    const int fc = from.colour_channels(), tc = to.colour_channels();
    if (fc == tc) for (int k = 0; k < fc; ++k) out.c[static_cast<std::size_t>(k)] = p.c[static_cast<std::size_t>(k)];
    else if (fc == 3 && tc == 1) out.c[0] = 0.2126f * p.c[0] + 0.7152f * p.c[1] + 0.0722f * p.c[2];
    else if (fc == 1 && tc == 3) out.c[0] = out.c[1] = out.c[2] = p.c[0];
    const float a = from.alpha ? p.c[static_cast<std::size_t>(fc)] : 1.f;
    if (to.alpha) out.c[static_cast<std::size_t>(tc)] = a;
    if (to.depth != pixel::Depth::F32)
        for (int k = 0; k < to.channels(); ++k) out.c[static_cast<std::size_t>(k)] = std::clamp(out.c[static_cast<std::size_t>(k)], 0.f, 1.f);
    return out;
}
pixel::TileMap::TileRef convert_tile(const pixel::TileMap::TileRef& t, const pixel::Format& to) {
    if (!t) return nullptr;
    const auto& from = t->format();
    if (t->is_uniform()) return pixel::Tile::uniform(to, convert_px(t->get(0, 0), from, to));
    pixel::TileWriter w(to, pixel::transparent());
    for (int y = 0; y < pixel::kTile; ++y)
        for (int x = 0; x < pixel::kTile; ++x) w.set(x, y, convert_px(t->get(x, y), from, to));
    return w.freeze();
}
void walk_in(const Document& d, const Document::Id& id, Bundle& b) {
    b.layers.emplace_back(id, *d.get(id));
    for (const auto& c : d.panel(id)) walk_in(d, c, b);
}
}  // namespace

Bundle take(const Document& d, const std::vector<Document::Id>& ids) {
    Bundle b;
    b.image = d.image();
    for (const auto& id : d.walk()) {
        if (std::find(ids.begin(), ids.end(), id) == ids.end() || !d.get(id)) continue;
        bool inside = false;
        for (const auto& r : b.roots) if (d.within(id, r)) inside = true;
        if (inside) continue;
        b.roots.push_back(id);
        walk_in(d, id, b);
    }
    if (b.roots.size() == 1) b.name = d.get(b.roots[0])->name;
    else b.name = std::to_string(b.roots.size()) + " Layers";
    return b;
}

Bundle of_image(const Image& im, std::shared_ptr<const pixel::TileMap> pixels, const std::string& name) {
    Bundle b;
    b.image = im;
    Layer l;
    l.kind = Kind::Pixel;
    l.name = name;
    l.pixels = std::move(pixels);
    b.layers.emplace_back("drop", l);
    b.roots = {"drop"};
    b.name = name;
    return b;
}

std::string refusal(const Bundle& b, const Image& into) {
    if (b.roots.empty()) return "nothing to copy";
    if (into.width <= 0 || into.height <= 0) return "there is no image there";
    const auto fm = b.image.format.mode, tm = into.format.mode;
    const bool simple = (fm == pixel::Mode::RGB || fm == pixel::Mode::Gray) && (tm == pixel::Mode::RGB || tm == pixel::Mode::Gray);
    if (fm != tm && !simple) return "CMYK and Lab layers move only into an image of the same mode, for now";
    if (!into.format.alpha && b.image.format.alpha) return "that image has no transparency";
    return {};
}

pixel::TileMap converted(const pixel::TileMap& m, const pixel::Format& to) {
    if (m.format() == to) return m;
    std::vector<pixel::TileMap::TileRef> grid;
    grid.reserve(static_cast<std::size_t>(m.cols()) * static_cast<std::size_t>(m.rows()));
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) grid.push_back(convert_tile(m.tile(c, r), to));
    pixel::TileMap::Outside out;
    for (const auto& [k, t] : m.outside_tiles()) out[k] = convert_tile(t, to);
    return pixel::TileMap::from_tiles(to, m.width(), m.height(), std::move(grid)).with_outside(std::move(out));
}

}  // namespace klimt::core::transfer

namespace klimt::core::layers {

std::vector<Id> paste(DocJournal& j, Document& d, const transfer::Bundle& b, const Id& above) {
    const Image& to = d.image();
    if (!transfer::refusal(b, to).empty()) return {};
    const auto& from = b.image;
    // What was dragged lands in view: its CONTENT's box (every pixel layer's
    // pixels, within its image) centred on the target -- the canvas centred
    // when there are no pixels (adjustment layers, empty ones).
    pixel::Rect box{};
    for (const auto& [id, l] : b.layers) {
        if (!l.pixels) continue;
        const auto r = canvas::content_bounds(*l.pixels).intersect({0, 0, from.width, from.height});
        if (r.empty()) continue;
        if (box.empty()) box = r;
        else {
            const int x0 = std::min(box.x, r.x), y0 = std::min(box.y, r.y);
            const int x1 = std::max(box.x + box.w, r.x + r.w), y1 = std::max(box.y + box.h, r.y + r.h);
            box = {x0, y0, x1 - x0, y1 - y0};
        }
    }
    if (box.empty()) box = {0, 0, from.width, from.height};
    const int ox = (to.width - box.w) / 2 - box.x, oy = (to.height - box.h) / 2 - box.y;
    const auto mask_to = d.mask_format();
    const auto fit = [&](const std::shared_ptr<const pixel::TileMap>& m, const pixel::Format& f) -> std::shared_ptr<const pixel::TileMap> {
        if (!m) return nullptr;
        pixel::TileMap c = transfer::converted(*m, f);
        if (ox != 0 || oy != 0 || c.width() != to.width || c.height() != to.height)
            c = canvas::reframed(c, to.width, to.height, ox, oy, pixel::transparent());
        return std::make_shared<const pixel::TileMap>(std::move(c));
    };
    std::vector<std::pair<Id, Layer>> fitted;
    for (const auto& [id, l0] : b.layers) {
        Layer l = l0;
        l.pixels = fit(l.pixels, to.format);
        l.mask = fit(l.mask, mask_to);
        l.filter_mask = fit(l.filter_mask, mask_to);
        if (!l.pixels && l.kind == Kind::Pixel && l0.pixels) return {};
        fitted.emplace_back(id, std::move(l));
    }
    return insert(j, d, "Paste " + (b.roots.size() == 1 ? "“" + b.name + "”" : b.name), fitted, b.roots, above);
}

}  // namespace klimt::core::layers
