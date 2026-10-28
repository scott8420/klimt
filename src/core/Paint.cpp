#include "core/Paint.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace klimt::core::paint {

namespace {
using pixel::kTile;
constexpr std::size_t kPx = static_cast<std::size_t>(kTile) * kTile;
const pixel::Format kCov{pixel::Depth::F32, pixel::Mode::Gray, false};

float cov_at(const pixel::Tile* t, std::size_t i) {
    if (!t) return 0.f;
    float v;
    std::memcpy(&v, t->data() + (t->is_uniform() ? 0 : i * 4), 4);
    return v;
}
}  // namespace

std::vector<select::Pt> dabs_along(select::Pt from, select::Pt to, double spacing, double& carry) {
    std::vector<select::Pt> out;
    const double dx = to.x - from.x, dy = to.y - from.y, len = std::hypot(dx, dy);
    double at = spacing - carry;   // distance along the segment to the next dab
    if (at < 0) at = 0;
    while (at <= len) {
        const double t = len > 0 ? at / len : 0;
        out.push_back({from.x + dx * t, from.y + dy * t});
        at += spacing;
        if (len == 0) break;
    }
    carry = len - (at - spacing);
    return out;
}

std::vector<Dab> dabs_along(const Dab& from, const Dab& to, const Brush& b, double& carry) {
    std::vector<Dab> out;
    const double dx = to.x - from.x, dy = to.y - from.y, len = std::hypot(dx, dy);
    // The spacing at the lighter end: a pressure-thinned stroke stays solid.
    const double sp = std::max(0.5, 2 * dab_radius(b, std::min(from.p, to.p)) * 0.25);
    double at = sp - carry;
    if (at < 0) at = 0;
    while (at <= len) {
        const double t = len > 0 ? at / len : 0;
        out.push_back({from.x + dx * t, from.y + dy * t, static_cast<float>(from.p + (to.p - from.p) * t)});
        at += sp;
        if (len == 0) break;
    }
    carry = len - (at - sp);
    return out;
}

std::optional<Dab> Smoother::push(const Dab& p) {
    m_last = {p.x, p.y, p.p};
    if (!m_started) {   // the first point: the brush is there
        m_started = true;
        m_x = p.x;
        m_y = p.y;
        return p;
    }
    const double dx = p.x - m_x, dy = p.y - m_y, d = std::hypot(dx, dy);
    if (d <= m_r) return std::nullopt;   // inside the string: the brush waits
    const double k = (d - m_r) / d;
    m_x += dx * k;
    m_y += dy * k;
    return Dab{m_x, m_y, p.p};
}

std::optional<Dab> Smoother::finish() {
    if (!m_started || (m_x == m_last.x && m_y == m_last.y)) return std::nullopt;
    m_x = m_last.x;
    m_y = m_last.y;
    return Dab{m_x, m_y, m_last.p};
}

double dab_radius(const Brush& b, float p) {
    const double r = std::max(0.5, b.size / 2);
    if (!b.pressure_size) return r;
    return std::max(0.5, r * std::clamp(static_cast<double>(p), static_cast<double>(kMinPressure), 1.0));
}

float dab_strength(const Brush& b, float p) {
    return b.pressure_opacity ? std::clamp(p, 0.f, 1.f) : 1.f;
}

float dab_alpha(double d, const Brush& b) { return dab_alpha(d, std::max(0.5, b.size / 2), b.hardness); }

float dab_alpha(double d, double r, double hardness) {
    r = std::max(0.5, r);
    if (d >= r + 0.5) return 0.f;
    // The hard core, then a smooth falloff to the rim; the rim itself is
    // anti-aliased over one pixel.
    const double core = std::clamp(hardness, 0.0, 1.0) * r;
    double a = 1;
    if (d > core) {
        const double t = std::clamp((d - core) / std::max(1e-6, r - core), 0.0, 1.0);
        a = 1 - t * t * (3 - 2 * t);
    }
    if (d > r - 0.5) a *= std::clamp(r + 0.5 - d, 0.0, 1.0);
    return static_cast<float>(std::clamp(a, 0.0, 1.0));
}

pixel::TileMap new_coverage(int w, int h) { return pixel::TileMap(kCov, w, h); }

pixel::TileMap add_dabs(const pixel::TileMap& cov, const Brush& b, const std::vector<select::Pt>& pts, TileSet& touched) {
    std::vector<Dab> dabs;
    dabs.reserve(pts.size());
    for (const auto& p : pts) dabs.push_back({p.x, p.y, 1.f});
    return add_pen_dabs(cov, b, dabs, touched);
}

pixel::TileMap add_pen_dabs(const pixel::TileMap& cov, const Brush& b, const std::vector<Dab>& dabs, TileSet& touched) {
    if (dabs.empty()) return cov;
    const int w = cov.width(), h = cov.height();
    const double r = std::max(0.5, b.size / 2) + 1;   // the largest a dab can be (pressure only shrinks it)
    // Group the work per tile: each touched tile is copied once.
    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(cov.cols()) * static_cast<std::size_t>(cov.rows()));
    for (int row = 0; row < cov.rows(); ++row)
        for (int col = 0; col < cov.cols(); ++col)
            grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(cov.cols()) + static_cast<std::size_t>(col)] = cov.tile(col, row);
    std::vector<float> buf(kPx);
    for (int row = 0; row < cov.rows(); ++row)
        for (int col = 0; col < cov.cols(); ++col) {
            const pixel::Rect cell = pixel::Rect{col * kTile, row * kTile, kTile, kTile}.intersect({0, 0, w, h});
            bool hit = false;
            for (const auto& p : dabs)
                if (p.x + r >= cell.x && p.x - r <= cell.x + cell.w && p.y + r >= cell.y && p.y - r <= cell.y + cell.h) { hit = true; break; }
            if (!hit) continue;
            auto& slot = grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(cov.cols()) + static_cast<std::size_t>(col)];
            for (std::size_t i = 0; i < kPx; ++i) buf[i] = cov_at(slot.get(), i);
            bool changed = false;
            for (const auto& p : dabs) {
                const double pr = dab_radius(b, p.p), reach = pr + 1;
                const float strength = dab_strength(b, p.p);
                const int x0 = std::max(cell.x, static_cast<int>(std::floor(p.x - reach)));
                const int x1 = std::min(cell.x + cell.w, static_cast<int>(std::ceil(p.x + reach)));
                const int y0 = std::max(cell.y, static_cast<int>(std::floor(p.y - reach)));
                const int y1 = std::min(cell.y + cell.h, static_cast<int>(std::ceil(p.y + reach)));
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) {
                        const float a = dab_alpha(std::hypot(x + 0.5 - p.x, y + 0.5 - p.y), pr, b.hardness) * strength;
                        float& c = buf[static_cast<std::size_t>(y - cell.y) * kTile + static_cast<std::size_t>(x - cell.x)];
                        if (b.flow >= 1) {
                            if (a > c) { c = a; changed = true; }
                        } else if (a > 0.f) {   // s012: build up, a flow's worth a dab
                            const float n = c + a * static_cast<float>(b.flow) * (1 - c);
                            if (n > c) { c = n; changed = true; }
                        }
                    }
            }
            if (!changed) continue;
            std::vector<std::uint8_t> bytes(kPx * 4);
            std::memcpy(bytes.data(), buf.data(), bytes.size());
            slot = pixel::Tile::from_bytes(kCov, std::move(bytes));
            touched.add(col, row);
        }
    return pixel::TileMap::from_tiles(kCov, w, h, std::move(grid));
}

pixel::TileMap apply(const pixel::TileMap& original, const pixel::TileMap& current, const pixel::TileMap& cov,
                     const pixel::TileMap* mask, const Brush& b, const TileSet& touched) {
    const pixel::Format f = original.format();
    const int cc = f.colour_channels();
    const auto ai = static_cast<std::size_t>(f.channels() - 1);
    const float op = static_cast<float>(std::clamp(b.opacity, 0.0, 1.0));
    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(current.cols()) * static_cast<std::size_t>(current.rows()));
    for (int row = 0; row < current.rows(); ++row)
        for (int col = 0; col < current.cols(); ++col) {
            auto& slot = grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(current.cols()) + static_cast<std::size_t>(col)];
            slot = current.tile(col, row);
            if (!touched.has(col, row)) continue;
            const auto ct = cov.tile(col, row);
            const auto ot = original.tile(col, row);
            if (!ct) { slot = ot; continue; }
            pixel::TileWriter wr = ot ? pixel::TileWriter(ot) : pixel::TileWriter(f, pixel::transparent());
            const pixel::Rect cell = pixel::Rect{col * kTile, row * kTile, kTile, kTile}.intersect({0, 0, original.width(), original.height()});
            for (int y = 0; y < cell.h; ++y)
                for (int x = 0; x < cell.w; ++x) {
                    const std::size_t i = static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x);
                    float a = cov_at(ct.get(), i) * op;
                    if (a <= 0.f) continue;
                    if (mask) a *= select::coverage(*mask, cell.x + x, cell.y + y);
                    if (a <= 0.f) continue;
                    const pixel::Px o = wr.get(x, y);
                    pixel::Px p = o;
                    if (b.erase) {
                        if (f.alpha) {
                            p.c[ai] = o.c[ai] * (1 - a);
                            if (p.c[ai] <= 0.f) p = pixel::transparent();
                        } else {
                            for (int k = 0; k < cc; ++k) { const auto q = static_cast<std::size_t>(k); p.c[q] += (b.background.c[q] - o.c[q]) * a; }
                        }
                    } else if (!f.alpha || b.alpha_lock) {
                        if (f.alpha && o.c[ai] <= 0.f) continue;
                        for (int k = 0; k < cc; ++k) { const auto q = static_cast<std::size_t>(k); p.c[q] += (b.colour.c[q] - o.c[q]) * a; }
                    } else {
                        const float oa = o.c[ai], out = a + oa * (1 - a);
                        for (int k = 0; k < cc; ++k) {
                            const auto q = static_cast<std::size_t>(k);
                            p.c[q] = out > 0.f ? (b.colour.c[q] * a + o.c[q] * oa * (1 - a)) / out : 0.f;
                        }
                        p.c[ai] = out;
                    }
                    wr.set(x, y, p);
                }
            auto t = wr.freeze();
            if (t && !t->is_uniform())   // all transparent -> no tile; one colour -> uniform
                t = pixel::Tile::from_bytes(f, std::vector<std::uint8_t>(t->data(), t->data() + t->bytes()));
            slot = t;
        }
    return current.with_grid(std::move(grid));   // s010: keeps the pixels beyond the image
}

std::string refusal(const Document& d, const Document::Id& id, const Brush& b) {
    const auto l = d.get(id);
    if (!l) return "pick a layer to paint on";
    if (l->kind != Kind::Pixel) return layers::not_pixels(d, id);
    if (d.locked_all(id) || (l->locks & lock::Pixels)) return "“" + l->name + "” is locked";
    if (b.erase && (l->locks & lock::Alpha) && d.image().format.alpha) return "“" + l->name + "” has its transparent pixels locked";
    if (!l->visible || !d.shown(id)) return "“" + l->name + "” is hidden";
    return {};
}

bool stroke(DocJournal& j, Document& d, const Document::Id& id, const std::vector<select::Pt>& path, Brush b) {
    std::vector<Dab> dabs;
    for (const auto& p : path) dabs.push_back({p.x, p.y, 1.f});
    return stroke_pen(j, d, id, dabs, b);
}

bool stroke_pen(DocJournal& j, Document& d, const Document::Id& id, const std::vector<Dab>& path, Brush b) {
    if (path.empty() || !refusal(d, id, b).empty()) return false;
    const auto l0 = *d.get(id);
    b.alpha_lock = (l0.locks & lock::Alpha) != 0;
    const pixel::TileMap original = l0.pixels ? *l0.pixels : d.blank();
    pixel::TileMap cov = new_coverage(original.width(), original.height());
    pixel::TileMap cur = original;
    j.begin(b.erase ? "Eraser" : "Brush Stroke", {id});
    double carry = 1e9;   // the first dab lands at the first point
    for (std::size_t i = 0; i < path.size(); ++i) {
        const auto dabs = dabs_along(path[i == 0 ? 0 : i - 1], path[i], b, carry);
        TileSet touched = tiles_of(d.image());
        cov = add_pen_dabs(cov, b, dabs, touched);
        cur = apply(original, cur, cov, d.selection(), b, touched);
        Layer l = *d.get(id);
        l.pixels = cur.tile_count() ? std::make_shared<const pixel::TileMap>(cur) : nullptr;
        d.put(id, l);   // inside the gesture: the door is open
    }
    return j.commit();
}

}  // namespace klimt::core::paint
