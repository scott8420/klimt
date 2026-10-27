#include "core/Selection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace klimt::core::select {

namespace {
using pixel::kTile;
using pixel::TileMap;
using TileRef = TileMap::TileRef;
constexpr std::size_t kPx = static_cast<std::size_t>(kTile) * kTile;

// ── reading and writing mask samples, fast (Gray, no alpha: 1 sample) ───────
float sample(const pixel::Tile& t, std::size_t i) {
    const std::uint8_t* d = t.data();
    if (t.is_uniform()) i = 0;
    switch (t.format().depth) {
        case pixel::Depth::U8:  return d[i] / 255.f;
        case pixel::Depth::U16: { std::uint16_t v; std::memcpy(&v, d + 2 * i, 2); return v / 65535.f; }
        case pixel::Depth::F32: { float v; std::memcpy(&v, d + 4 * i, 4); return v; }
    }
    return 0.f;
}

// A whole tile's coverage into `out` (kPx floats); false when absent (all 0).
bool tile_cov(const TileMap& m, int col, int row, std::vector<float>& out) {
    const auto t = m.tile(col, row);
    if (!t) return false;
    out.resize(kPx);
    if (t->is_uniform()) { std::fill(out.begin(), out.end(), sample(*t, 0)); return true; }
    for (std::size_t i = 0; i < kPx; ++i) out[i] = sample(*t, i);
    return true;
}

bool uniform_value(const TileRef& t, float& v) {
    if (!t || !t->is_uniform()) return false;
    v = sample(*t, 0);
    return true;
}

// Coverage (kPx floats, 0..1) -> a mask tile; nullptr when all zero.
TileRef make_tile(const pixel::Format& mf, const std::vector<float>& cov) {
    const auto b = static_cast<std::size_t>(mf.bytes_per_sample());
    std::vector<std::uint8_t> bytes(kPx * b);
    bool any = false;
    for (std::size_t i = 0; i < kPx; ++i) {
        const float v = std::clamp(cov[i], 0.f, 1.f);
        switch (mf.depth) {
            case pixel::Depth::U8: {
                const auto q = static_cast<std::uint8_t>(std::lround(v * 255.f));
                bytes[i] = q;
                any = any || q;
                break;
            }
            case pixel::Depth::U16: {
                const auto q = static_cast<std::uint16_t>(std::lround(v * 65535.f));
                std::memcpy(bytes.data() + 2 * i, &q, 2);
                any = any || q;
                break;
            }
            case pixel::Depth::F32:
                std::memcpy(bytes.data() + 4 * i, &v, 4);
                any = any || v > 0.f;
                break;
        }
    }
    return any ? pixel::Tile::from_bytes(mf, std::move(bytes)) : nullptr;
}

TileRef full_tile(const pixel::Format& mf) {
    pixel::Px one;
    one.c[0] = 1.f;
    return pixel::Tile::uniform(mf, one);
}

// Build a mask tile by tile: `f(col, row, cov)` returns 0 (empty), 1 (all
// selected) or 2 (partial -- cov filled with kPx coverages).
TileMap build(const pixel::Format& mf, int w, int h, const std::function<int(int, int, std::vector<float>&)>& f) {
    const TileMap blank(mf, w, h);
    std::vector<TileRef> grid(static_cast<std::size_t>(blank.cols()) * static_cast<std::size_t>(blank.rows()));
    const TileRef full = full_tile(mf);
    std::vector<float> cov;
    for (int row = 0; row < blank.rows(); ++row)
        for (int col = 0; col < blank.cols(); ++col) {
            const int k = f(col, row, cov);
            TileRef t = k == 1 ? full : k == 2 ? make_tile(mf, cov) : nullptr;
            grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(blank.cols()) + static_cast<std::size_t>(col)] = std::move(t);
        }
    return TileMap::from_tiles(mf, w, h, std::move(grid));
}

// Two tiles hold the same pixels (the same tile, or equal bytes).
bool tiles_equal(const TileRef& a, const TileRef& b) {
    if (a == b) return true;
    if (!a || !b || a->is_uniform() != b->is_uniform() || a->bytes() != b->bytes()) return false;
    return std::memcmp(a->data(), b->data(), a->bytes()) == 0;
}

// A written tile, collapsed: all one colour -> uniform; all zero with alpha
// -> no tile.
TileRef collapsed(const TileRef& t) {
    if (!t || t->is_uniform()) return t;
    return pixel::Tile::from_bytes(t->format(), std::vector<std::uint8_t>(t->data(), t->data() + t->bytes()));
}

// The part of tile (col, row) inside a w x h image.
pixel::Rect live(int col, int row, int w, int h) {
    return pixel::Rect{col * kTile, row * kTile, kTile, kTile}.intersect({0, 0, w, h});
}
}  // namespace

const char* op_name(Op op) {
    switch (op) {
        case Op::Replace:   return "Select";
        case Op::Add:       return "Add to Selection";
        case Op::Subtract:  return "Subtract from Selection";
        case Op::Intersect: return "Intersect Selection";
    }
    return "Select";
}

// ── shapes ──────────────────────────────────────────────────────────────────
TileMap rect_mask(const pixel::Format& mf, int w, int h, const pixel::Rect& r0) {
    const pixel::Rect r = r0.intersect({0, 0, w, h});
    return build(mf, w, h, [&](int col, int row, std::vector<float>& cov) {
        const pixel::Rect cell = live(col, row, w, h);
        const pixel::Rect hit = r.intersect(cell);
        if (hit.empty()) return 0;
        if (hit == cell) return 1;
        cov.assign(kPx, 0.f);
        for (int y = hit.y; y < hit.y + hit.h; ++y)
            for (int x = hit.x; x < hit.x + hit.w; ++x)
                cov[static_cast<std::size_t>(y - row * kTile) * kTile + static_cast<std::size_t>(x - col * kTile)] = 1.f;
        return 2;
    });
}

TileMap ellipse_mask(const pixel::Format& mf, int w, int h, double x0, double y0, double x1, double y1) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    if (rx <= 0 || ry <= 0) return TileMap(mf, w, h);
    const auto in = [&](double x, double y) {
        const double dx = (x - cx) / rx, dy = (y - cy) / ry;
        return dx * dx + dy * dy <= 1.0;
    };
    // Normalised distance a pixel's centre may be from the rim and still be
    // decided by its centre alone (a pixel is ~1.5 px across, diagonally).
    const double margin = 1.5 / std::min(rx, ry);
    return build(mf, w, h, [&](int col, int row, std::vector<float>& cov) {
        const pixel::Rect c = live(col, row, w, h);
        if (c.empty()) return 0;
        if (in(c.x, c.y) && in(c.x + c.w, c.y) && in(c.x, c.y + c.h) && in(c.x + c.w, c.y + c.h)) return 1;   // convex
        const double nx = std::clamp(cx, static_cast<double>(c.x), static_cast<double>(c.x + c.w));
        const double ny = std::clamp(cy, static_cast<double>(c.y), static_cast<double>(c.y + c.h));
        if (!in(nx, ny)) return 0;   // the cell's nearest point is outside
        cov.assign(kPx, 0.f);
        for (int y = c.y; y < c.y + c.h; ++y)
            for (int x = c.x; x < c.x + c.w; ++x) {
                const double dx = (x + 0.5 - cx) / rx, dy = (y + 0.5 - cy) / ry;
                const double d = std::sqrt(dx * dx + dy * dy);
                float v;
                if (d < 1 - margin) v = 1.f;
                else if (d > 1 + margin) v = 0.f;
                else {
                    int n = 0;
                    for (int sy = 0; sy < 4; ++sy)
                        for (int sx = 0; sx < 4; ++sx) n += in(x + (sx + 0.5) / 4, y + (sy + 0.5) / 4);
                    v = static_cast<float>(n) / 16.f;
                }
                cov[static_cast<std::size_t>(y - row * kTile) * kTile + static_cast<std::size_t>(x - col * kTile)] = v;
            }
        return 2;
    });
}

TileMap polygon_mask(const pixel::Format& mf, int w, int h, const std::vector<Pt>& pts) {
    const TileMap blank(mf, w, h);
    if (pts.size() < 3 || w <= 0 || h <= 0) return blank;
    double minx = pts[0].x, maxx = pts[0].x, miny = pts[0].y, maxy = pts[0].y;
    for (const auto& p : pts) {
        minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
    }
    const int ry0 = std::clamp(static_cast<int>(std::floor(miny)), 0, h), ry1 = std::clamp(static_cast<int>(std::ceil(maxy)), 0, h);
    const int cx0 = std::clamp(static_cast<int>(std::floor(minx)), 0, w), cx1 = std::clamp(static_cast<int>(std::ceil(maxx)), 0, w);
    if (ry1 <= ry0 || cx1 <= cx0) return blank;
    const int span = cx1 - cx0;
    std::vector<TileRef> grid(static_cast<std::size_t>(blank.cols()) * static_cast<std::size_t>(blank.rows()));
    const TileRef full = full_tile(mf);
    std::vector<float> band;            // 256 rows x span, coverage
    std::vector<std::pair<double, int>> xs;
    for (int row = ry0 / kTile; row <= (ry1 - 1) / kTile; ++row) {
        const int by0 = std::max(ry0, row * kTile), by1 = std::min(ry1, row * kTile + kTile);
        band.assign(static_cast<std::size_t>(kTile) * static_cast<std::size_t>(span), 0.f);
        for (int y = by0; y < by1; ++y) {
            float* line = band.data() + static_cast<std::size_t>(y - row * kTile) * static_cast<std::size_t>(span);
            for (int s = 0; s < 4; ++s) {
                const double yy = y + (s + 0.5) / 4;
                xs.clear();
                for (std::size_t i = 0; i < pts.size(); ++i) {
                    const Pt& a = pts[i];
                    const Pt& b = pts[(i + 1) % pts.size()];
                    if ((a.y <= yy) == (b.y <= yy)) continue;   // doesn't cross this sub-row
                    const double t = (yy - a.y) / (b.y - a.y);
                    xs.emplace_back(a.x + t * (b.x - a.x), b.y > a.y ? 1 : -1);
                }
                std::sort(xs.begin(), xs.end());
                int wind = 0;
                for (std::size_t k = 0; k + 1 < xs.size() + 1 && k < xs.size(); ++k) {
                    wind += xs[k].second;
                    if (wind == 0 || k + 1 >= xs.size()) continue;
                    // Inside from xs[k] to xs[k+1] (non-zero winding).
                    const double xa = std::clamp(xs[k].first, static_cast<double>(cx0), static_cast<double>(cx1)) - cx0;
                    const double xb = std::clamp(xs[k + 1].first, static_cast<double>(cx0), static_cast<double>(cx1)) - cx0;
                    if (xb <= xa) continue;
                    const int ia = static_cast<int>(std::floor(xa)), ib = static_cast<int>(std::floor(xb));
                    constexpr float q = 0.25f;
                    if (ia == ib) { if (ia < span) line[ia] += q * static_cast<float>(xb - xa); continue; }
                    line[ia] += q * static_cast<float>(ia + 1 - xa);
                    for (int i = ia + 1; i < ib && i < span; ++i) line[i] += q;
                    if (ib < span) line[ib] += q * static_cast<float>(xb - ib);
                }
            }
        }
        // The band into tiles.
        std::vector<float> cov(kPx);
        for (int col = cx0 / kTile; col <= (cx1 - 1) / kTile; ++col) {
            std::fill(cov.begin(), cov.end(), 0.f);
            bool any = false, all = true;
            const pixel::Rect cell = live(col, row, w, h);
            for (int y = cell.y; y < cell.y + cell.h; ++y)
                for (int x = cell.x; x < cell.x + cell.w; ++x) {
                    float v = 0.f;
                    if (x >= cx0 && x < cx1)
                        v = std::min(1.f, band[static_cast<std::size_t>(y - row * kTile) * static_cast<std::size_t>(span) +
                                               static_cast<std::size_t>(x - cx0)]);
                    cov[static_cast<std::size_t>(y - row * kTile) * kTile + static_cast<std::size_t>(x - col * kTile)] = v;
                    any = any || v > 0.f;
                    all = all && v >= 1.f;
                }
            grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(blank.cols()) + static_cast<std::size_t>(col)] =
                !any ? nullptr : all ? full : make_tile(mf, cov);
        }
    }
    return TileMap::from_tiles(mf, w, h, std::move(grid));
}

// ── set arithmetic ──────────────────────────────────────────────────────────
TileMap combine(const TileMap* old, const TileMap& shape, Op op) {
    if (op == Op::Replace || !old) {
        if (op == Op::Replace || op == Op::Add) return shape;
        return TileMap(shape.format(), shape.width(), shape.height());   // subtract / intersect from nothing
    }
    const pixel::Format mf = shape.format();
    const int w = shape.width(), h = shape.height();
    std::vector<float> a, b;
    return build(mf, w, h, [&](int col, int row, std::vector<float>& cov) {
        const TileRef ta = old->tile(col, row), tb = shape.tile(col, row);
        float ua = 0, ub = 0;
        const bool una = !ta || uniform_value(ta, ua), unb = !tb || uniform_value(tb, ub);
        if (una && unb) {
            const float v = op == Op::Add ? std::max(ua, ub) : op == Op::Subtract ? std::min(ua, 1 - ub) : std::min(ua, ub);
            if (v <= 0.f) return 0;
            if (v >= 1.f) return 1;
            cov.assign(kPx, v);
            return 2;
        }
        if (!tile_cov(*old, col, row, a)) a.assign(kPx, 0.f);
        if (!tile_cov(shape, col, row, b)) b.assign(kPx, 0.f);
        cov.resize(kPx);
        for (std::size_t i = 0; i < kPx; ++i)
            cov[i] = op == Op::Add ? std::max(a[i], b[i]) : op == Op::Subtract ? std::min(a[i], 1 - b[i]) : std::min(a[i], b[i]);
        return 2;
    });
}

TileMap all_mask(const pixel::Format& mf, int w, int h) { return rect_mask(mf, w, h, {0, 0, w, h}); }

TileMap inverted(const TileMap* old, const pixel::Format& mf, int w, int h) {
    if (!old) return all_mask(mf, w, h);
    std::vector<float> a;
    return build(mf, w, h, [&](int col, int row, std::vector<float>& cov) {
        const TileRef t = old->tile(col, row);
        float u = 0;
        if (!t) return live(col, row, w, h).empty() ? 0 : 1;
        if (uniform_value(t, u)) {
            if (u >= 1.f) return 0;
            if (u <= 0.f) return 1;
            cov.assign(kPx, 1 - u);
            return 2;
        }
        tile_cov(*old, col, row, a);
        cov.resize(kPx);
        for (std::size_t i = 0; i < kPx; ++i) cov[i] = 1 - a[i];
        return 2;
    });
}

bool empty(const TileMap& m) { return m.tile_count() == 0; }

pixel::Rect extent(const TileMap& m) {
    int c0 = m.cols(), r0 = m.rows(), c1 = -1, r1 = -1;
    for (int row = 0; row < m.rows(); ++row)
        for (int col = 0; col < m.cols(); ++col)
            if (m.tile(col, row)) { c0 = std::min(c0, col); c1 = std::max(c1, col); r0 = std::min(r0, row); r1 = std::max(r1, row); }
    if (c1 < 0) return {};
    return pixel::Rect{c0 * kTile, r0 * kTile, (c1 - c0 + 1) * kTile, (r1 - r0 + 1) * kTile}.intersect({0, 0, m.width(), m.height()});
}

float coverage(const TileMap& m, int x, int y) {
    if (x < 0 || y < 0 || x >= m.width() || y >= m.height()) return 0.f;
    const auto t = m.tile(x / kTile, y / kTile);
    if (!t) return 0.f;
    return sample(*t, static_cast<std::size_t>(y % kTile) * kTile + static_cast<std::size_t>(x % kTile));
}

std::optional<TileMap> feathered(const TileMap& m, double radius, const std::function<bool()>& cancelled) {
    if (radius <= 0 || empty(m)) return m;
    // Three box passes approximate a Gaussian of sigma = radius / 2.
    const double sigma = radius / 2;
    // The box is ODD (2 * half + 1 samples, divided by the same): an even
    // divisor over an odd window inflated the mask ~10% a pass.
    const int ideal = std::max(1, static_cast<int>(std::lround(std::sqrt(12.0 * sigma * sigma / 3 + 1))));
    const int half = std::max(1, ideal / 2), box = 2 * half + 1, reach = 3 * half + 1;
    const pixel::Rect e = extent(m);
    const pixel::Rect r = pixel::Rect{e.x - reach, e.y - reach, e.w + 2 * reach, e.h + 2 * reach}.intersect({0, 0, m.width(), m.height()});
    const auto W = static_cast<std::size_t>(r.w), H = static_cast<std::size_t>(r.h);
    std::vector<float> a(W * H), tmp(std::max(W, H));
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) a[static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x)] = coverage(m, r.x + x, r.y + y);
    // One box blur along a line of n samples, stride s (edges: clamp).
    const auto pass = [&](float* p, std::size_t n, std::size_t s) {
        double sum = 0;
        const auto at = [&](long i) { return p[static_cast<std::size_t>(std::clamp<long>(i, 0, static_cast<long>(n) - 1)) * s]; };
        for (long i = -half; i <= half; ++i) sum += at(i);
        for (std::size_t i = 0; i < n; ++i) {
            tmp[i] = static_cast<float>(sum / box);
            sum += at(static_cast<long>(i) + half + 1) - at(static_cast<long>(i) - half);
        }
        for (std::size_t i = 0; i < n; ++i) p[i * s] = tmp[i];
    };
    for (int k = 0; k < 3; ++k) {
        for (std::size_t y = 0; y < H; ++y) {
            if (cancelled && (y & 63) == 0 && cancelled()) return std::nullopt;
            pass(a.data() + y * W, W, 1);
        }
        for (std::size_t x = 0; x < W; ++x) {
            if (cancelled && (x & 63) == 0 && cancelled()) return std::nullopt;
            pass(a.data() + x, H, W);
        }
    }
    const pixel::Format mf = m.format();
    return build(mf, m.width(), m.height(), [&](int col, int row, std::vector<float>& cov) {
        const pixel::Rect cell = live(col, row, m.width(), m.height());
        if (cell.intersect(r).empty()) return 0;
        cov.assign(kPx, 0.f);
        for (int y = cell.y; y < cell.y + cell.h; ++y)
            for (int x = cell.x; x < cell.x + cell.w; ++x)
                if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
                    cov[static_cast<std::size_t>(y - row * kTile) * kTile + static_cast<std::size_t>(x - col * kTile)] =
                        a[static_cast<std::size_t>(y - r.y) * W + static_cast<std::size_t>(x - r.x)];
        return 2;
    });
}

// ── the Magic Wand ──────────────────────────────────────────────────────────
std::optional<TileMap> wand_mask(const TileMap& src, const pixel::Format& mf, int x0, int y0, float tol,
                                 bool contiguous, const std::function<bool()>& cancelled) {
    const int w = src.width(), h = src.height();
    if (x0 < 0 || y0 < 0 || x0 >= w || y0 >= h) return TileMap(mf, w, h);
    const int n = src.format().channels();
    const pixel::Px seed = src.get(x0, y0);
    // Tiles' pixels decoded once each while in use: a few slots, reused
    // round-robin (a run crossing a tile border, and the rows above and
    // below it, touch at most four tiles at once).
    struct Slot { int col = -1, row = -1; std::vector<pixel::Px> px; };
    std::array<Slot, 8> slots;
    std::size_t next_slot = 0;
    const auto px = [&](int x, int y) -> const pixel::Px& {
        const int col = x / kTile, row = y / kTile;
        Slot* hit = nullptr;
        for (auto& sl : slots) if (sl.col == col && sl.row == row) { hit = &sl; break; }
        if (!hit) {
            hit = &slots[next_slot++ % slots.size()];
            hit->col = col;
            hit->row = row;
            hit->px.assign(kPx, pixel::transparent());
            if (const auto t = src.tile(col, row))
                for (int ty = 0; ty < kTile; ++ty)
                    for (int tx = 0; tx < kTile; ++tx) hit->px[static_cast<std::size_t>(ty) * kTile + static_cast<std::size_t>(tx)] = t->get(tx, ty);
        }
        return hit->px[static_cast<std::size_t>(y % kTile) * kTile + static_cast<std::size_t>(x % kTile)];
    };
    const auto like = [&](const pixel::Px& p) {
        for (int k = 0; k < n; ++k)
            if (std::abs(p.c[static_cast<std::size_t>(k)] - seed.c[static_cast<std::size_t>(k)]) > tol + 1e-6f) return false;
        return true;
    };
    std::vector<std::uint8_t> on(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    const auto at = [&](int x, int y) -> std::uint8_t& { return on[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; };
    if (contiguous) {
        // Scanline flood fill: runs, not single pixels, on the stack.
        std::vector<std::pair<int, int>> stack{{x0, y0}};
        std::size_t steps = 0;
        while (!stack.empty()) {
            if (cancelled && (steps++ & 1023) == 0 && cancelled()) return std::nullopt;
            auto [x, y] = stack.back();
            stack.pop_back();
            if (at(x, y) || !like(px(x, y))) continue;
            int l = x, r = x;
            while (l > 0 && !at(l - 1, y) && like(px(l - 1, y))) --l;
            while (r < w - 1 && !at(r + 1, y) && like(px(r + 1, y))) ++r;
            for (int i = l; i <= r; ++i) at(i, y) = 1;
            for (const int ny : {y - 1, y + 1}) {
                if (ny < 0 || ny >= h) continue;
                bool open = false;
                for (int i = l; i <= r; ++i) {
                    const bool cand = !at(i, ny) && like(px(i, ny));
                    if (cand && !open) stack.emplace_back(i, ny);
                    open = cand;
                }
            }
        }
    } else {
        for (int y = 0; y < h; ++y) {
            if (cancelled && (y & 63) == 0 && cancelled()) return std::nullopt;
            for (int x = 0; x < w; ++x) at(x, y) = like(px(x, y)) ? 1 : 0;
        }
    }
    return build(mf, w, h, [&](int col, int row, std::vector<float>& cov) {
        const pixel::Rect c = live(col, row, w, h);
        bool any = false, all = true;
        for (int y = c.y; y < c.y + c.h && (all || !any); ++y)
            for (int x = c.x; x < c.x + c.w; ++x) { if (at(x, y)) any = true; else all = false; }
        if (!any) return 0;
        if (all) return 1;
        cov.assign(kPx, 0.f);
        for (int y = c.y; y < c.y + c.h; ++y)
            for (int x = c.x; x < c.x + c.w; ++x)
                if (at(x, y)) cov[static_cast<std::size_t>(y - row * kTile) * kTile + static_cast<std::size_t>(x - col * kTile)] = 1.f;
        return 2;
    });
}

// ── the outline (marching ants) ─────────────────────────────────────────────
std::vector<Seg> outline(const TileMap& m) {
    const int w = m.width(), h = m.height();
    const auto on = [&](int x, int y) { return coverage(m, x, y) >= 0.5f; };
    std::vector<std::pair<int, int>> hz;   // (y, x): an edge on line y from x to x+1
    std::vector<std::pair<int, int>> vt;   // (x, y): an edge on line x from y to y+1
    for (int row = 0; row < m.rows(); ++row)
        for (int col = 0; col < m.cols(); ++col) {
            const pixel::Rect c = live(col, row, w, h);
            const TileRef t = m.tile(col, row);
            float u = 0;
            const bool uni = !t || uniform_value(t, u);
            const bool uon = t ? u >= 0.5f : false;
            // Inside the tile: only a non-uniform tile has edges within.
            if (!uni)
                for (int y = c.y; y < c.y + c.h; ++y)
                    for (int x = c.x; x < c.x + c.w; ++x) {
                        const bool here = on(x, y);
                        if (x > c.x && on(x - 1, y) != here) vt.emplace_back(x, y);
                        if (y > c.y && on(x, y - 1) != here) hz.emplace_back(y, x);
                    }
            // The tile's left and top borders (against the neighbour or outside),
            // and the image's right and bottom edges.
            const TileRef left = col > 0 ? m.tile(col - 1, row) : nullptr;
            const TileRef up = row > 0 ? m.tile(col, row - 1) : nullptr;
            float ul = 0, uu = 0;
            const bool lsame = uni && (!left || uniform_value(left, ul)) && ((left ? ul >= 0.5f : false) == uon);
            const bool usame = uni && (!up || uniform_value(up, uu)) && ((up ? uu >= 0.5f : false) == uon);
            if (!lsame)
                for (int y = c.y; y < c.y + c.h; ++y)
                    if (on(c.x - 1, y) != on(c.x, y)) vt.emplace_back(c.x, y);
            if (!usame)
                for (int x = c.x; x < c.x + c.w; ++x)
                    if (on(x, c.y - 1) != on(x, c.y)) hz.emplace_back(c.y, x);
            if (c.x + c.w == w && (!uni || uon))
                for (int y = c.y; y < c.y + c.h; ++y) if (on(w - 1, y)) vt.emplace_back(w, y);
            if (c.y + c.h == h && (!uni || uon))
                for (int x = c.x; x < c.x + c.w; ++x) if (on(x, h - 1)) hz.emplace_back(h, x);
        }
    std::sort(hz.begin(), hz.end());
    std::sort(vt.begin(), vt.end());
    std::vector<Seg> out;
    for (std::size_t i = 0; i < hz.size();) {
        std::size_t j = i + 1;
        while (j < hz.size() && hz[j].first == hz[i].first && hz[j].second == hz[j - 1].second + 1) ++j;
        out.push_back({hz[i].second, hz[i].first, hz[j - 1].second + 1, hz[i].first});
        i = j;
    }
    for (std::size_t i = 0; i < vt.size();) {
        std::size_t j = i + 1;
        while (j < vt.size() && vt[j].first == vt[i].first && vt[j].second == vt[j - 1].second + 1) ++j;
        out.push_back({vt[i].first, vt[i].second, vt[i].first, vt[j - 1].second + 1});
        i = j;
    }
    return out;
}

// ── pixels through a mask ───────────────────────────────────────────────────
namespace {
// Each tile the mask holds (all tiles when no mask), through `px(old, m)`.
// A tile the mask fully selects and `whole` answers for is set whole.
TileMap through(const TileMap& layer, const TileMap* mask,
                const std::function<pixel::Px(const pixel::Px&, float)>& px,
                const std::function<std::optional<TileRef>(const TileRef&)>& whole) {
    const pixel::Format f = layer.format();
    std::vector<TileRef> grid(static_cast<std::size_t>(layer.cols()) * static_cast<std::size_t>(layer.rows()));
    std::vector<float> cov;
    for (int row = 0; row < layer.rows(); ++row)
        for (int col = 0; col < layer.cols(); ++col) {
            const TileRef old = layer.tile(col, row);
            auto& slot = grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(layer.cols()) + static_cast<std::size_t>(col)];
            slot = old;
            float u = 1;
            const TileRef mt = mask ? mask->tile(col, row) : nullptr;
            if (mask && !mt) continue;                       // nothing selected here: shared
            const bool full = !mask || (uniform_value(mt, u) && u >= 1.f);
            if (full)
                if (auto w = whole(old)) { slot = *w; continue; }
            if (!full) tile_cov(*mask, col, row, cov);
            pixel::TileWriter wr = old ? pixel::TileWriter(old) : pixel::TileWriter(f, pixel::transparent());
            const pixel::Rect c = live(col, row, layer.width(), layer.height());
            for (int y = 0; y < c.h; ++y)
                for (int x = 0; x < c.w; ++x) {
                    const float mv = full ? 1.f : cov[static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)];
                    if (mv <= 0.f) continue;
                    wr.set(x, y, px(wr.get(x, y), mv));
                }
            // A tile written to nothing (all transparent) is no tile; one
            // colour is a uniform tile.
            slot = collapsed(wr.freeze());
        }
    return TileMap::from_tiles(f, layer.width(), layer.height(), std::move(grid));
}
}  // namespace

TileMap filled(const TileMap& layer, const TileMap* mask, const pixel::Px& colour, bool alpha_lock) {
    const pixel::Format f = layer.format();
    const int n = f.channels(), cc = f.colour_channels();
    const auto a = static_cast<std::size_t>(n - 1);
    const float ca = f.alpha ? colour.c[a] : 1.f;
    return through(layer, mask,
        [&](const pixel::Px& o, float m) {
            pixel::Px p = o;
            if (!f.alpha) {
                for (int k = 0; k < cc; ++k) p.c[static_cast<std::size_t>(k)] += (colour.c[static_cast<std::size_t>(k)] - o.c[static_cast<std::size_t>(k)]) * m;
                return p;
            }
            if (alpha_lock) {
                if (o.c[a] <= 0.f) return o;
                for (int k = 0; k < cc; ++k) p.c[static_cast<std::size_t>(k)] += (colour.c[static_cast<std::size_t>(k)] - o.c[static_cast<std::size_t>(k)]) * m;
                return p;
            }
            const float sa = ca * m, oa = o.c[a], out = sa + oa * (1 - sa);
            for (int k = 0; k < cc; ++k) {
                const auto i = static_cast<std::size_t>(k);
                p.c[i] = out > 0.f ? (colour.c[i] * sa + o.c[i] * oa * (1 - sa)) / out : 0.f;
            }
            p.c[a] = out;
            return p;
        },
        [&](const TileRef& old) -> std::optional<TileRef> {
            // Wholly selected, an opaque colour, no lock: one uniform tile.
            if (alpha_lock || ca < 1.f) return std::nullopt;
            (void)old;
            pixel::Px p = colour;
            if (f.alpha) p.c[a] = 1.f;
            return pixel::Tile::uniform(f, p);
        });
}

TileMap cleared(const TileMap& layer, const TileMap* mask, const pixel::Px& background) {
    const pixel::Format f = layer.format();
    const int cc = f.colour_channels();
    const auto a = static_cast<std::size_t>(f.channels() - 1);
    return through(layer, mask,
        [&](const pixel::Px& o, float m) {
            pixel::Px p = o;
            if (!f.alpha) {
                for (int k = 0; k < cc; ++k) p.c[static_cast<std::size_t>(k)] += (background.c[static_cast<std::size_t>(k)] - o.c[static_cast<std::size_t>(k)]) * m;
                return p;
            }
            p.c[a] = o.c[a] * (1 - m);
            if (p.c[a] <= 0.f) return pixel::transparent();
            return p;
        },
        [&](const TileRef&) -> std::optional<TileRef> {
            if (!f.alpha) return pixel::Tile::uniform(f, background);
            return TileRef{};   // wholly deleted: no tile
        });
}

// ── verbs ───────────────────────────────────────────────────────────────────
bool set_if(DocJournal& j, Document& d, const std::shared_ptr<const pixel::TileMap>& expect,
            std::optional<TileMap> mask, const std::string& label) {
    if (d.selection_ptr() != expect) return false;
    return set(j, d, std::move(mask), label);
}

bool set(DocJournal& j, Document& d, std::optional<TileMap> mask, const std::string& label) {
    const auto old = d.get(kSelectionId);
    std::shared_ptr<const TileMap> next;
    if (mask && !empty(*mask)) {
        if (!(mask->format() == d.mask_format()) || mask->width() != d.image().width || mask->height() != d.image().height)
            return false;
        // The same mask (every tile the same tile, or the same pixels)? No
        // change, no step -- and the selection keeps its own tiles.
        if (const auto* cur = d.selection()) {
            bool same = true;
            for (int r = 0; r < mask->rows() && same; ++r)
                for (int c = 0; c < mask->cols() && same; ++c) same = tiles_equal(mask->tile(c, r), cur->tile(c, r));
            if (same) return false;
        }
        next = std::make_shared<const TileMap>(std::move(*mask));
    }
    if (!next && !d.selection()) return false;   // nothing to nothing
    Layer v;
    v.name = "Selection";
    v.pixels = next;
    std::optional<Layer> value;
    if (next) value = v;
    (void)old;
    return j.run(label, {kSelectionId}, [&] { d.put(kSelectionId, value); });
}

namespace {
bool shape(DocJournal& j, Document& d, const TileMap& s, Op op, const std::string& what) {
    const std::string label = op == Op::Replace ? what : op_name(op);
    return set(j, d, combine(d.selection(), s, op), label);
}
}  // namespace

bool rect(DocJournal& j, Document& d, const pixel::Rect& r, Op op) {
    const auto& im = d.image();
    return shape(j, d, rect_mask(d.mask_format(), im.width, im.height, r), op, "Rectangle Select");
}

bool ellipse(DocJournal& j, Document& d, double x0, double y0, double x1, double y1, Op op) {
    const auto& im = d.image();
    return shape(j, d, ellipse_mask(d.mask_format(), im.width, im.height, x0, y0, x1, y1), op, "Ellipse Select");
}

bool polygon(DocJournal& j, Document& d, const std::vector<Pt>& pts, Op op) {
    const auto& im = d.image();
    return shape(j, d, polygon_mask(d.mask_format(), im.width, im.height, pts), op, "Lasso");
}

bool all(DocJournal& j, Document& d) {
    const auto& im = d.image();
    return set(j, d, all_mask(d.mask_format(), im.width, im.height), "Select All");
}

bool none(DocJournal& j, Document& d) { return set(j, d, std::nullopt, "Deselect"); }

bool invert(DocJournal& j, Document& d) {
    const auto& im = d.image();
    return set(j, d, inverted(d.selection(), d.mask_format(), im.width, im.height), "Invert Selection");
}

bool feather(DocJournal& j, Document& d, double radius) {
    const auto* m = d.selection();
    if (!m || radius <= 0) return false;
    auto f = feathered(*m, radius);
    if (!f) return false;
    char buf[48];
    std::snprintf(buf, sizeof buf, "Feather %g px", radius);
    return set(j, d, std::move(*f), buf);
}

}  // namespace klimt::core::select

// ── layers: pixels through the selection ────────────────────────────────────
namespace klimt::core::layers {

namespace {
bool writable(const Document& d, const Id& id) {
    const auto l = d.get(id);
    return l && l->kind == Kind::Pixel && !d.locked_all(id) && !(l->locks & lock::Pixels);
}
std::string q(const Document& d, const Id& id) {
    const auto l = d.get(id);
    return l ? "“" + l->name + "”" : id;
}
}  // namespace

bool set_pixels_if(DocJournal& j, Document& d, const Id& id, const std::shared_ptr<const pixel::TileMap>& expect,
                   pixel::TileMap after, const std::string& label) {
    if (!writable(d, id)) return false;
    Layer l = *d.get(id);
    if (l.pixels != expect) return false;
    const pixel::TileMap before = l.pixels ? *l.pixels : d.blank();
    if (!(after.format() == before.format()) || after.width() != before.width() || after.height() != before.height()) return false;
    bool same = true;
    for (int r = 0; r < after.rows() && same; ++r)
        for (int c = 0; c < after.cols() && same; ++c) same = after.same_tile(before, c, r);
    if (same) return false;
    l.pixels = after.tile_count() ? std::make_shared<const pixel::TileMap>(std::move(after)) : nullptr;
    return j.run(label, {id}, [&] { d.put(id, l); });
}

bool fill_selection(DocJournal& j, Document& d, const Id& id, const pixel::Px& colour) {
    if (!writable(d, id)) return false;
    const auto l = *d.get(id);
    const pixel::TileMap before = l.pixels ? *l.pixels : d.blank();
    return set_pixels_if(j, d, id, l.pixels, select::filled(before, d.selection(), colour, (l.locks & lock::Alpha) != 0),
                         "Fill " + q(d, id));
}

bool clear_selection(DocJournal& j, Document& d, const Id& id, const pixel::Px& background) {
    if (!writable(d, id)) return false;
    const auto l = *d.get(id);
    if ((l.locks & lock::Alpha) && d.image().format.alpha) return false;   // deleting IS changing alpha
    const pixel::TileMap before = l.pixels ? *l.pixels : d.blank();
    return set_pixels_if(j, d, id, l.pixels, select::cleared(before, d.selection(), background), "Delete in " + q(d, id));
}

}  // namespace klimt::core::layers
