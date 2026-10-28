#include "core/Paint.hpp"
#include "core/File.hpp"   // s015: flatten, for All Layers

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
    // The spacing at the lighter end: a pressure-thinned stroke stays solid;
    // s017: a flat tip (or a leaning one) spaces by its narrow side.
    const double narrow = std::clamp(b.tilt ? std::min(b.roundness, 0.5) : b.roundness, 0.1, 1.0);
    const double sp = std::max(0.5, 2 * dab_radius(b, std::min(from.p, to.p)) * 0.25 * narrow);
    double at = sp - carry;
    if (at < 0) at = 0;
    while (at <= len) {
        const double t = len > 0 ? at / len : 0;
        const auto lerp = [t](float a, float b2) { return static_cast<float>(a + (b2 - a) * t); };
        out.push_back({from.x + dx * t, from.y + dy * t, lerp(from.p, to.p), lerp(from.tx, to.tx), lerp(from.ty, to.ty)});
        at += sp;
        if (len == 0) break;
    }
    carry = len - (at - sp);
    return out;
}

std::optional<Dab> Smoother::push(const Dab& p) {
    m_last_dab = p;
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
    return Dab{m_x, m_y, p.p, p.tx, p.ty};
}

std::optional<Dab> Smoother::finish() {
    if (!m_started || (m_x == m_last_dab.x && m_y == m_last_dab.y)) return std::nullopt;
    m_x = m_last_dab.x;
    m_y = m_last_dab.y;
    return m_last_dab;
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

namespace {
// Chalk's paper grain: value noise on a 3 px lattice, fixed to the image.
float lattice(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343U ^ static_cast<std::uint32_t>(y) * 0xd8163841U;
    h ^= h >> 13; h *= 0x5bd1e995U; h ^= h >> 15;
    return static_cast<float>(h & 0xffff) / 65535.f;
}
float grain(int ix, int iy) {
    constexpr int s = 3;
    const int gx = ix >= 0 ? ix / s : -((-ix + s - 1) / s), gy = iy >= 0 ? iy / s : -((-iy + s - 1) / s);
    const float fx = static_cast<float>(ix - gx * s) / s, fy = static_cast<float>(iy - gy * s) / s;
    const float a = lattice(gx, gy), b = lattice(gx + 1, gy), c = lattice(gx, gy + 1), d = lattice(gx + 1, gy + 1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}
}  // namespace

double tip_reach(double r, const Brush& b) { return (b.tip == Tip::Square ? r * 1.4143 : r) + 1; }

float tip_alpha(double dx, double dy, double r, const Brush& b, const Dab& d, int ix, int iy) {
    double angle = b.angle, round = b.roundness;
    if (b.tilt) {   // the pen's lean: face it, and flatten as it leans
        const double m = std::min(1.0, std::hypot(static_cast<double>(d.tx), static_cast<double>(d.ty)));
        if (m > 0.05) {
            angle = std::atan2(-static_cast<double>(d.ty), static_cast<double>(d.tx)) * 180 / M_PI;
            round = std::min(round, 1.0 - 0.8 * m);
        }
    }
    round = std::clamp(round, 0.05, 1.0);
    double u = dx, v = dy;
    if (angle != 0 || round < 1) {
        const double a = angle * M_PI / 180, c = std::cos(a), s = std::sin(a);
        u = dx * c - dy * s;       // into the tip's own frame (image y runs down,
        v = (dx * s + dy * c) / round;   // so counter-clockwise is -y)
    }
    const double dist = b.tip == Tip::Square ? std::max(std::abs(u), std::abs(v)) : std::hypot(u, v);
    float a = dab_alpha(dist, r, b.hardness);
    if (b.tip == Tip::Chalk && a > 0.f) a *= std::clamp(grain(ix, iy) * 1.6f - 0.3f, 0.f, 1.f);
    return a;
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
    const double r = tip_reach(std::max(0.5, b.size / 2), b);   // the largest a dab can be (pressure only shrinks it)
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
                const double pr = dab_radius(b, p.p), reach = tip_reach(pr, b);
                const float strength = dab_strength(b, p.p);
                const int x0 = std::max(cell.x, static_cast<int>(std::floor(p.x - reach)));
                const int x1 = std::min(cell.x + cell.w, static_cast<int>(std::ceil(p.x + reach)));
                const int y0 = std::max(cell.y, static_cast<int>(std::floor(p.y - reach)));
                const int y1 = std::min(cell.y + cell.h, static_cast<int>(std::ceil(p.y + reach)));
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) {
                        const float a = tip_alpha(x + 0.5 - p.x, y + 0.5 - p.y, pr, b, p, x, y) * strength;   // s017
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
    // s015: the clone source, when there is one (and it is the layer's mode).
    const pixel::TileMap* src = b.source && b.source->format().mode == f.mode && !b.erase ? b.source.get() : nullptr;
    const int sdx = static_cast<int>(std::lround(b.dx)), sdy = static_cast<int>(std::lround(b.dy));
    const bool salpha = src && src->format().alpha;
    const auto sai = src ? static_cast<std::size_t>(src->format().colour_channels()) : 0;
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
                    if (src) {   // s015: the Clone Stamp
                        const int sx = cell.x + x + sdx, sy = cell.y + y + sdy;
                        const pixel::Px s = src->get(sx, sy);
                        const float sa = salpha ? s.c[sai] : 1.f;
                        const float A = a * sa;
                        if (A <= 0.f) continue;
                        if (!f.alpha || b.alpha_lock) {
                            if (f.alpha && o.c[ai] <= 0.f) continue;
                            for (int k = 0; k < cc; ++k) { const auto q = static_cast<std::size_t>(k); p.c[q] += (s.c[q] - o.c[q]) * A; }
                        } else {
                            const float oa = o.c[ai], out = A + oa * (1 - A);
                            for (int k = 0; k < cc; ++k) {
                                const auto q = static_cast<std::size_t>(k);
                                p.c[q] = out > 0.f ? (s.c[q] * A + o.c[q] * oa * (1 - A)) / out : 0.f;
                            }
                            p.c[ai] = out;
                        }
                    } else if (b.erase) {
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

std::shared_ptr<const pixel::TileMap> clone_source(const Document& d, const Document::Id& id, bool all, std::string& why) {
    const auto l = d.get(id);
    if (!l) { why = "pick a layer to paint on"; return nullptr; }
    if (!all) return std::make_shared<const pixel::TileMap>(l->pixels ? *l->pixels : d.blank());
    if (d.image().format.mode != pixel::Mode::RGB) { why = "All Layers samples RGB documents only, for now"; return nullptr; }
    auto flat = file::flatten(d.image(), d.snapshot());
    if (!flat) { why = "couldn't flatten the picture"; return nullptr; }
    return std::make_shared<const pixel::TileMap>(std::move(*flat));
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

pixel::TileMap smudge(const pixel::TileMap& current, const pixel::TileMap* mask, const Brush& b,
                      const std::vector<Dab>& dabs, SmudgeCarry& carry, TileSet& touched) {
    const pixel::Format f = current.format();
    const int C = f.channels(), cc = f.colour_channels();
    const bool alpha = f.alpha;
    const float op = static_cast<float>(std::clamp(b.opacity, 0.0, 1.0));
    const float s = static_cast<float>(std::clamp(b.strength, 0.0, 1.0));
    const int R = static_cast<int>(std::ceil(tip_reach(std::max(0.5, b.size / 2), b)));
    const int side = 2 * R + 1;
    // Premultiplied <-> straight, for one pixel.
    const auto pre = [&](const pixel::Px& p, float* out) {
        const float a = alpha ? p.c[static_cast<std::size_t>(cc)] : 1.f;
        for (int k = 0; k < cc; ++k) out[k] = p.c[static_cast<std::size_t>(k)] * a;
        if (alpha) out[cc] = a;
    };
    const auto straight = [&](const float* in) {
        pixel::Px p{};
        const float a = alpha ? std::clamp(in[cc], 0.f, 1.f) : 1.f;
        for (int k = 0; k < cc; ++k) p.c[static_cast<std::size_t>(k)] = a > 1e-6f ? std::clamp(in[k] / a, 0.f, 1.f) : 0.f;
        if (alpha) p.c[static_cast<std::size_t>(cc)] = a;
        return p;
    };
    pixel::TileMap out = current;
    std::array<float, pixel::kMaxChannels> cur{}, mix{};
    for (const auto& d : dabs) {
        const int cx = static_cast<int>(std::lround(d.x)), cy = static_cast<int>(std::lround(d.y));
        if (!carry.started || carry.side != side || carry.channels != C) {   // the first dab: pick up, lay nothing
            carry.side = side;
            carry.channels = C;
            carry.c.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * static_cast<std::size_t>(C), 0.f);
            for (int y = 0; y < side; ++y)
                for (int x = 0; x < side; ++x)
                    pre(out.get(cx - R + x, cy - R + y), carry.c.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(C));
            carry.started = true;
            continue;
        }
        const double pr = dab_radius(b, d.p);
        const float strength = dab_strength(b, d.p) * op;
        const pixel::Rect box = pixel::Rect{cx - R, cy - R, side, side}.intersect({0, 0, out.width(), out.height()});
        if (box.empty()) continue;
        out = out.paint(box, [&](int x, int y, const pixel::Px& o) {
            float a = tip_alpha(x + 0.5 - d.x, y + 0.5 - d.y, pr, b, d, x, y) * strength;
            if (mask) a *= select::coverage(*mask, x, y);
            float* c = carry.c.data() + (static_cast<std::size_t>(y - cy + R) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x - cx + R)) * static_cast<std::size_t>(C);
            if (a <= 0.f) return o;   // outside the tip: the finger neither lays nor takes
            pre(o, cur.data());
            const float k = a * s;
            for (int i = 0; i < C; ++i) mix[static_cast<std::size_t>(i)] = cur[static_cast<std::size_t>(i)] + (c[i] - cur[static_cast<std::size_t>(i)]) * k;
            for (int i = 0; i < C; ++i) c[i] += (mix[static_cast<std::size_t>(i)] - c[i]) * a;   // the finger takes up the result
            pixel::Px p = straight(mix.data());
            if (b.alpha_lock && alpha) {
                if (o.c[static_cast<std::size_t>(cc)] <= 0.f) return o;
                p.c[static_cast<std::size_t>(cc)] = o.c[static_cast<std::size_t>(cc)];
            }
            if (alpha && p.c[static_cast<std::size_t>(cc)] <= 0.f) p = pixel::transparent();
            return p;
        });
        for (int row = box.y / kTile; row <= (box.y + box.h - 1) / kTile; ++row)
            for (int col = box.x / kTile; col <= (box.x + box.w - 1) / kTile; ++col)
                if (row < touched.rows && col < touched.cols) touched.add(col, row);
    }
    return out;
}

bool stroke_smudge(DocJournal& j, Document& d, const Document::Id& id, const std::vector<Dab>& path, Brush b) {
    if (path.empty() || !refusal(d, id, b).empty()) return false;
    const auto l0 = *d.get(id);
    b.alpha_lock = (l0.locks & lock::Alpha) != 0;
    b.erase = false;
    b.source = nullptr;
    pixel::TileMap cur = l0.pixels ? *l0.pixels : d.blank();
    SmudgeCarry carry;
    TileSet touched = tiles_of(d.image());
    double spacing = 1e9;
    for (std::size_t i = 0; i < path.size(); ++i)
        cur = smudge(cur, d.selection(), b, dabs_along(path[i == 0 ? 0 : i - 1], path[i], b, spacing), carry, touched);
    return layers::set_pixels_if(j, d, id, l0.pixels, std::move(cur), "Smudge");
}

bool stroke_selection(DocJournal& j, Document& d, const std::vector<Dab>& path, Brush b) {
    if (path.empty()) return false;
    const auto& im = d.image();
    b.erase = false;
    b.alpha_lock = false;
    b.source = nullptr;
    const pixel::TileMap original = d.selection() ? *d.selection() : select::all_mask(d.mask_format(), im.width, im.height);
    pixel::TileMap cov = new_coverage(original.width(), original.height());
    TileSet touched = tiles_of(im);
    double carry = 1e9;
    for (std::size_t i = 0; i < path.size(); ++i)
        cov = add_pen_dabs(cov, b, dabs_along(path[i == 0 ? 0 : i - 1], path[i], b, carry), touched);
    return select::set(j, d, apply(original, original, cov, nullptr, b, touched), "Quick Mask: Paint");
}

bool stroke_pen(DocJournal& j, Document& d, const Document::Id& id, const std::vector<Dab>& path, Brush b) {
    if (path.empty() || !refusal(d, id, b).empty()) return false;
    const auto l0 = *d.get(id);
    b.alpha_lock = (l0.locks & lock::Alpha) != 0;
    const pixel::TileMap original = l0.pixels ? *l0.pixels : d.blank();
    pixel::TileMap cov = new_coverage(original.width(), original.height());
    pixel::TileMap cur = original;
    j.begin(b.source ? "Clone Stamp" : b.erase ? "Eraser" : "Brush Stroke", {id});
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
