#include "core/Transform.hpp"
#include "core/Text.hpp"   // s046: type transforms as type
#include "core/Canvas.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numbers>
#include <set>

namespace klimt::core::xform {

namespace {
using pixel::kTile;
constexpr std::size_t kPx = static_cast<std::size_t>(kTile) * kTile;
using Cell = pixel::TileMap::Cell;

pixel::TileMap::TileRef zero_is_none(pixel::TileMap::TileRef t) {
    if (!t || !t->is_uniform()) return t;
    const auto n = static_cast<std::size_t>(t->format().bytes_per_pixel());
    for (std::size_t i = 0; i < n; ++i) if (t->data()[i]) return t;
    return nullptr;
}

// A tile's stored bytes, a uniform one expanded.
std::vector<std::uint8_t> bytes_of(const pixel::Tile& t) {
    const auto bpp = static_cast<std::size_t>(t.format().bytes_per_pixel());
    std::vector<std::uint8_t> out(kPx * bpp);
    if (t.is_uniform()) for (std::size_t i = 0; i < kPx; ++i) std::memcpy(out.data() + i * bpp, t.data(), bpp);
    else std::memcpy(out.data(), t.data(), out.size());
    return out;
}

// A whole cell of a map, premultiplied: its grid tile inside the image, its
// outside tile beyond (s010). Absent: zeros.
void decode_cell(const pixel::TileMap& m, int col, int row, std::vector<float>& out) {
    const auto ch = static_cast<std::size_t>(m.format().channels());
    out.assign(kPx * ch, 0.f);
    const pixel::Rect cell{col * kTile, row * kTile, kTile, kTile};
    const pixel::Rect live = cell.intersect({0, 0, m.width(), m.height()});
    const bool in_grid = !live.empty();
    const auto g = in_grid ? m.tile(col, row) : nullptr;
    const auto o = (live == cell) ? nullptr : m.outside(col, row);
    std::vector<float> tmp;
    if (g) g->premultiplied(out);
    if (!o) {
        if (g && live != cell)   // the grid tile's part beyond the image means nothing
            for (int y = 0; y < kTile; ++y)
                for (int x = 0; x < kTile; ++x)
                    if (x < live.x - cell.x || x >= live.x - cell.x + live.w || y < live.y - cell.y || y >= live.y - cell.y + live.h)
                        std::fill_n(out.data() + (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * ch, ch, 0.f);
        return;
    }
    o->premultiplied(tmp);
    for (int y = 0; y < kTile; ++y)
        for (int x = 0; x < kTile; ++x) {
            const bool in = !live.empty() && x >= live.x - cell.x && x < live.x - cell.x + live.w &&
                            y >= live.y - cell.y && y < live.y - cell.y + live.h;
            if (in) continue;
            const std::size_t i = (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * ch;
            std::copy_n(tmp.data() + i, ch, out.data() + i);
        }
}

// Reads a map's pixels, premultiplied, with a virtual alpha: an image with no
// alpha is opaque inside `box` and nothing outside it. Cells decoded once.
class Sampler {
public:
    Sampler(const pixel::TileMap& m, const pixel::Rect& box)
        : m_m(m), m_box(box), m_ch(m.format().channels()), m_alpha(m.format().alpha) {}
    int out_channels() const { return m_alpha ? m_ch : m_ch + 1; }   // colours + alpha
    // Pixel (x, y): out_channels() floats into v.
    void px(int x, int y, float* v) {
        const int n = out_channels();
        if (x < m_box.x || y < m_box.y || x >= m_box.x + m_box.w || y >= m_box.y + m_box.h) { std::fill_n(v, n, 0.f); return; }
        const int col = pixel::cell_of(x), row = pixel::cell_of(y);
        const float* c = cell(col, row);
        const float* p = c + (static_cast<std::size_t>(y - row * kTile) * kTile + static_cast<std::size_t>(x - col * kTile)) * static_cast<std::size_t>(m_ch);
        std::copy_n(p, m_ch, v);
        if (!m_alpha) v[m_ch] = 1.f;
    }

private:
    const float* cell(int col, int row) {
        if (m_last && m_last_cell == Cell{col, row}) return m_last;
        auto it = m_cells.find({col, row});
        if (it == m_cells.end()) {
            if (m_cells.size() > 160) m_cells.clear();   // ~160 MB at RGBA float: start over
            it = m_cells.emplace(Cell{col, row}, std::vector<float>{}).first;
            decode_cell(m_m, col, row, it->second);
        }
        m_last_cell = {col, row};
        m_last = it->second.data();
        return m_last;
    }
    const pixel::TileMap& m_m;
    pixel::Rect m_box;
    int m_ch;
    bool m_alpha;
    std::map<Cell, std::vector<float>> m_cells;
    Cell m_last_cell{0, 0};
    const float* m_last = nullptr;
};

// Catmull-Rom (B = 0, C = 1/2): bicubic that passes through the pixels, so a
// transform that only moves keeps them sharp. (Image Size's Mitchell is the
// softer choice for big reductions.)
double mitchell(double x) {
    constexpr double B = 0, C = 0.5;
    x = std::abs(x);
    if (x < 1) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6;
    if (x < 2) return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6;
    return 0;
}
}  // namespace

// ── Affine ──────────────────────────────────────────────────────────────────
Affine operator*(const Affine& l, const Affine& r) {
    return {l.a * r.a + l.c * r.b, l.b * r.a + l.d * r.b,
            l.a * r.c + l.c * r.d, l.b * r.c + l.d * r.d,
            l.a * r.e + l.c * r.f + l.e, l.b * r.e + l.d * r.f + l.f};
}

Affine Affine::inverse() const {
    const double det = a * d - b * c;
    if (std::abs(det) < 1e-12) return {};
    const double ia = d / det, ib = -b / det, ic = -c / det, id = a / det;
    return {ia, ib, ic, id, -(ia * e + ic * f), -(ib * e + id * f)};
}

Affine Affine::rotate(double r) {
    const double cs = std::cos(r), sn = std::sin(r);
    // Snap the exact quarter turns (cos 90 deg is 6e-17, not 0).
    const auto snap = [](double v) { return std::abs(v) < 1e-12 ? 0.0 : (std::abs(std::abs(v) - 1) < 1e-12 ? std::copysign(1.0, v) : v); };
    return {snap(cs), snap(sn), snap(-sn), snap(cs), 0, 0};
}

Affine Affine::about(Pt p, const Affine& m) { return translate(p.x, p.y) * m * translate(-p.x, -p.y); }

// ── Projective (s073) ───────────────────────────────────────────────────────
Pt Projective::apply(Pt p) const {
    const double w = h[6] * p.x + h[7] * p.y + h[8];
    const double X = h[0] * p.x + h[1] * p.y + h[2], Y = h[3] * p.x + h[4] * p.y + h[5];
    if (std::abs(w) < 1e-300) return {X > 0 ? 1e300 : -1e300, Y > 0 ? 1e300 : -1e300};
    return {X / w, Y / w};
}

Projective operator*(const Projective& l, const Projective& r) {
    Projective o;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double s = 0;
            for (int k = 0; k < 3; ++k) s += l.h[i * 3 + k] * r.h[k * 3 + j];
            o.h[i * 3 + j] = s;
        }
    return o;
}

Projective Projective::inverse() const {
    const double* m = h;
    const double c0 = m[4] * m[8] - m[5] * m[7], c1 = m[5] * m[6] - m[3] * m[8], c2 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c0 + m[1] * c1 + m[2] * c2;
    if (std::abs(det) < 1e-300) return {};
    Projective o;
    o.h[0] = c0 / det;
    o.h[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    o.h[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    o.h[3] = c1 / det;
    o.h[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    o.h[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    o.h[6] = c2 / det;
    o.h[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    o.h[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    return o;
}

bool Projective::affine() const {
    const double s = std::abs(h[8]);
    return s > 1e-300 && std::abs(h[6]) <= 1e-12 * s && std::abs(h[7]) <= 1e-12 * s;
}

Affine Projective::to_affine() const {
    const double s = std::abs(h[8]) > 1e-300 ? h[8] : 1;
    return {h[0] / s, h[3] / s, h[1] / s, h[4] / s, h[2] / s, h[5] / s};
}

bool convex(const Quad& q, double min_area) {
    int sign = 0;
    double area = 0;
    for (int i = 0; i < 4; ++i) {
        const Pt a = q.p[i], b = q.p[(i + 1) % 4], c = q.p[(i + 2) % 4];
        const double z = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        if (std::abs(z) < 1e-12) return false;   // three corners in a line: a fold
        const int s = z > 0 ? 1 : -1;
        if (sign && s != sign) return false;
        sign = s;
        area += a.x * b.y - b.x * a.y;
    }
    return std::abs(area) / 2 >= min_area;
}

namespace {
// The map from the unit square's corners (0,0) (1,0) (1,1) (0,1) to q's
// (Heckbert's square-to-quad).
std::optional<Projective> square_to_quad(const Quad& q) {
    const double x0 = q.p[0].x, y0 = q.p[0].y, x1 = q.p[1].x, y1 = q.p[1].y;
    const double x2 = q.p[2].x, y2 = q.p[2].y, x3 = q.p[3].x, y3 = q.p[3].y;
    const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    Projective m;
    if (std::abs(sx) < 1e-12 && std::abs(sy) < 1e-12) {   // a parallelogram: affine
        m.h[0] = x1 - x0; m.h[1] = x3 - x0; m.h[2] = x0;
        m.h[3] = y1 - y0; m.h[4] = y3 - y0; m.h[5] = y0;
        m.h[6] = 0; m.h[7] = 0; m.h[8] = 1;
        return m;
    }
    const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
    const double den = dx1 * dy2 - dx2 * dy1;
    if (std::abs(den) < 1e-12) return std::nullopt;
    const double g = (sx * dy2 - dx2 * sy) / den, hh = (dx1 * sy - sx * dy1) / den;
    m.h[0] = x1 - x0 + g * x1; m.h[1] = x3 - x0 + hh * x3; m.h[2] = x0;
    m.h[3] = y1 - y0 + g * y1; m.h[4] = y3 - y0 + hh * y3; m.h[5] = y0;
    m.h[6] = g; m.h[7] = hh; m.h[8] = 1;
    return m;
}
}  // namespace

std::optional<Projective> Projective::box_to_quad(double x, double y, double w, double hgt, const Quad& q) {
    if (w <= 0 || hgt <= 0 || !convex(q)) return std::nullopt;
    const auto sq = square_to_quad(q);
    if (!sq) return std::nullopt;
    Projective to_unit;   // the box to the unit square
    to_unit.h[0] = 1 / w; to_unit.h[2] = -x / w;
    to_unit.h[4] = 1 / hgt; to_unit.h[5] = -y / hgt;
    Projective m = *sq * to_unit;
    if (std::abs(m.h[8]) > 1e-300) for (double& v : m.h) v /= m.h[8];   // h8 = 1 (W > 0 over the box: convex)
    // Snap what is affine within rounding to affine (a skew stays affine).
    if (std::abs(m.h[6]) < 1e-14 && std::abs(m.h[7]) < 1e-14) m.h[6] = m.h[7] = 0;
    return m;
}

std::optional<Projective> Projective::box_to_quad(const pixel::Rect& b, const Quad& q) {
    return box_to_quad(b.x, b.y, b.w, b.h, q);
}

pixel::Rect mapped(const pixel::Rect& r, const Projective& m) {
    if (m.affine()) return mapped(r, m.to_affine());
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    for (const Pt p : {Pt{double(r.x), double(r.y)}, Pt{double(r.x + r.w), double(r.y)},
                       Pt{double(r.x), double(r.y + r.h)}, Pt{double(r.x + r.w), double(r.y + r.h)}}) {
        if (m.w_at(p) <= 1e-12) return {};   // beyond the horizon
        const Pt q = m.apply(p);
        x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y);
    }
    // A convex quad's box holds the whole map of the box (W > 0 over it).
    constexpr double kFar = 1 << 20;
    if (x0 < -kFar || y0 < -kFar || x1 > kFar || y1 > kFar) return {};
    const int ix0 = static_cast<int>(std::floor(x0 + 1e-6)), iy0 = static_cast<int>(std::floor(y0 + 1e-6));
    const int ix1 = static_cast<int>(std::ceil(x1 - 1e-6)), iy1 = static_cast<int>(std::ceil(y1 - 1e-6));
    return {ix0, iy0, std::max(0, ix1 - ix0), std::max(0, iy1 - iy0)};
}

pixel::Rect mapped(const pixel::Rect& r, const Affine& m) {
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    for (const Pt p : {Pt{double(r.x), double(r.y)}, Pt{double(r.x + r.w), double(r.y)},
                       Pt{double(r.x), double(r.y + r.h)}, Pt{double(r.x + r.w), double(r.y + r.h)}}) {
        const Pt q = m.apply(p);
        x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y);
    }
    const int ix0 = static_cast<int>(std::floor(x0 + 1e-6)), iy0 = static_cast<int>(std::floor(y0 + 1e-6));
    const int ix1 = static_cast<int>(std::ceil(x1 - 1e-6)), iy1 = static_cast<int>(std::ceil(y1 - 1e-6));
    return {ix0, iy0, std::max(0, ix1 - ix0), std::max(0, iy1 - iy0)};
}

// ── the resample ────────────────────────────────────────────────────────────
namespace {
// s074: where each landing pixel comes from -- the projective inverse, or a
// warp's mesh. cell() is told each tile's part before its pixels (a warp
// rasterizes its triangles there); at() gives the source point and the
// filter's widening, false: nothing lands there.
class Mapper {
public:
    virtual ~Mapper() = default;
    virtual void cell(const pixel::Rect&) {}
    virtual bool at(int x, int y, Pt& sp, double& kx, double& ky) = 0;
};

class ProjMapper final : public Mapper {
public:
    ProjMapper(const Projective& m, const pixel::Rect& box, Quality q)
        : m_m(m), m_inv(m.inverse()), m_box(box), m_q(q), m_flat(m.affine()) {
        if (m_flat) widths({box.x + box.w / 2.0, box.y + box.h / 2.0}, m_kx, m_ky);
        m_rx = q == Quality::Best ? 2 * m_kx : 1;
        m_ry = q == Quality::Best ? 2 * m_ky : 1;
        if (!m_flat && q == Quality::Best) m_rx = m_ry = 32;
    }
    bool at(int x, int y, Pt& sp, double& kx, double& ky) override {
        const Pt dp{x + 0.5, y + 0.5};
        if (!m_flat && m_inv.w_at(dp) <= 0) return false;   // s073: beyond the horizon
        sp = m_inv.apply(dp);
        if (sp.x < m_box.x - m_rx - 1 || sp.y < m_box.y - m_ry - 1 || sp.x > m_box.x + m_box.w + m_rx + 1 ||
            sp.y > m_box.y + m_box.h + m_ry + 1)
            return false;
        kx = m_kx; ky = m_ky;
        if (!m_flat) widths(sp, kx, ky);
        return true;
    }

private:
    // The filter: bicubic widened along a source axis the map shrinks. s073:
    // a projective map shrinks by different amounts in different places --
    // its derivative at the source point says how much (at most 16 x).
    void widths(Pt sp, double& kx, double& ky) const {
        if (m_q != Quality::Best) { kx = ky = 1; return; }
        const Projective& m = m_m;
        const double wt = m.w_at(sp);
        const Pt d = m.apply(sp);
        const double iw = std::abs(wt) > 1e-300 ? 1 / wt : 0;
        const double xx = (m.h[0] - d.x * m.h[6]) * iw, yx = (m.h[3] - d.y * m.h[6]) * iw;   // d(x', y') / dx
        const double xy = (m.h[1] - d.x * m.h[7]) * iw, yy = (m.h[4] - d.y * m.h[7]) * iw;   // d(x', y') / dy
        kx = std::clamp(1.0 / std::max(1e-6, std::hypot(xx, yx)), 1.0, 16.0);
        ky = std::clamp(1.0 / std::max(1e-6, std::hypot(xy, yy)), 1.0, 16.0);
    }
    Projective m_m, m_inv;
    pixel::Rect m_box;
    Quality m_q;
    bool m_flat;
    double m_kx = 1, m_ky = 1, m_rx = 1, m_ry = 1;
};

// THE RESAMPLE's body (s011; s074: behind a Mapper): `land` is where pixels
// may land.
std::optional<pixel::TileMap> resample(const pixel::TileMap& base, const pixel::TileMap& src,
                                       const pixel::TileMap* mask, const pixel::Rect& box, pixel::Rect land,
                                       Quality q, const std::function<bool()>& cancelled, Mapper& mp) {
    if (box.empty()) return base;
    const pixel::Format f = base.format();
    const int ch = f.channels();
    const auto chs = static_cast<std::size_t>(ch);
    const bool alpha = f.alpha, fl = f.depth == pixel::Depth::F32;
    const int w = base.width(), h = base.height();
    land = {land.x - 2, land.y - 2, land.w + 4, land.h + 4};   // a pixel of slack for the filter's reach
    if (land.empty()) return base;

    Sampler s(src, box);
    const int sn = s.out_channels();   // ch (alpha) or ch + 1
    std::optional<Sampler> ms;
    if (mask) ms.emplace(*mask, pixel::Rect{0, 0, mask->width(), mask->height()});


    float acc[pixel::kMaxChannels + 1], tap[pixel::kMaxChannels + 1];
    const auto sample = [&](double u, double v, double fkx, double fky, float* out) {
        std::fill_n(out, sn, 0.f);
        const double rx = q == Quality::Best ? 2 * fkx : 1, ry = q == Quality::Best ? 2 * fky : 1;
        const double cu = u - 0.5, cv = v - 0.5;   // pixel centres at i + 0.5
        if (q == Quality::Fast) {
            const int x0 = static_cast<int>(std::floor(cu)), y0 = static_cast<int>(std::floor(cv));
            const float fx = static_cast<float>(cu - x0), fy = static_cast<float>(cv - y0);
            const float wts[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
            const int xs[4] = {x0, x0 + 1, x0, x0 + 1}, ys[4] = {y0, y0, y0 + 1, y0 + 1};
            for (int k = 0; k < 4; ++k) {
                if (wts[k] <= 0) continue;
                s.px(xs[k], ys[k], tap);
                for (int c = 0; c < sn; ++c) out[c] += tap[c] * wts[k];
            }
            return;
        }
        const int x0 = static_cast<int>(std::ceil(cu - rx)), x1 = static_cast<int>(std::floor(cu + rx));
        const int y0 = static_cast<int>(std::ceil(cv - ry)), y1 = static_cast<int>(std::floor(cv + ry));
        double sum = 0;
        for (int yy = y0; yy <= y1; ++yy) {
            const double wy = mitchell((yy - cv) / fky);
            if (wy == 0) continue;
            for (int xx = x0; xx <= x1; ++xx) {
                const double wgt = wy * mitchell((xx - cu) / fkx);
                if (wgt == 0) continue;
                s.px(xx, yy, tap);
                for (int c = 0; c < sn; ++c) out[c] += tap[c] * static_cast<float>(wgt);
                sum += wgt;
            }
        }
        if (sum != 0) for (int c = 0; c < sn; ++c) out[c] /= static_cast<float>(sum);
    };
    const auto coverage = [&](double u, double v) -> float {
        if (!ms) return 1.f;
        const double cu = u - 0.5, cv = v - 0.5;
        const int x0 = static_cast<int>(std::floor(cu)), y0 = static_cast<int>(std::floor(cv));
        const float fx = static_cast<float>(cu - x0), fy = static_cast<float>(cv - y0);
        float t[2], r = 0;
        ms->px(x0, y0, t);         r += t[0] * (1 - fx) * (1 - fy);
        ms->px(x0 + 1, y0, t);     r += t[0] * fx * (1 - fy);
        ms->px(x0, y0 + 1, t);     r += t[0] * (1 - fx) * fy;
        ms->px(x0 + 1, y0 + 1, t); r += t[0] * fx * fy;
        return std::clamp(r, 0.f, 1.f);
    };

    // The cells the landing touches; the rest of base is shared as it is.
    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(base.cols()) * static_cast<std::size_t>(base.rows()));
    for (int row = 0; row < base.rows(); ++row)
        for (int col = 0; col < base.cols(); ++col)
            grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(base.cols()) + static_cast<std::size_t>(col)] = base.tile(col, row);
    pixel::TileMap::Outside outside = base.outside_tiles();
    const pixel::Rect image{0, 0, w, h};
    std::vector<float> buf;
    for (int row = pixel::cell_of(land.y); row <= pixel::cell_of(land.y + land.h - 1); ++row) {
        if (cancelled && cancelled()) return std::nullopt;
        for (int col = pixel::cell_of(land.x); col <= pixel::cell_of(land.x + land.w - 1); ++col) {
            const pixel::Rect cell{col * kTile, row * kTile, kTile, kTile};
            const pixel::Rect hit = cell.intersect(land);
            decode_cell(base, col, row, buf);
            bool changed = false;
            mp.cell(hit);
            for (int y = hit.y; y < hit.y + hit.h; ++y)
                for (int x = hit.x; x < hit.x + hit.w; ++x) {
                    Pt sp;
                    double pkx = 1, pky = 1;
                    if (!mp.at(x, y, sp, pkx, pky)) continue;
                    sample(sp.x, sp.y, pkx, pky, acc);
                    const float cov = coverage(sp.x, sp.y);
                    const float a = std::clamp(acc[sn - 1] * cov, 0.f, 1.f);
                    if (a <= 0.f) continue;
                    float* d = buf.data() + (static_cast<std::size_t>(y - cell.y) * kTile + static_cast<std::size_t>(x - cell.x)) * chs;
                    const int colours = alpha ? ch - 1 : ch;
                    for (int c = 0; c < colours; ++c) {
                        float v = acc[c] * cov + d[c] * (1 - a);
                        v = fl ? std::max(0.f, v) : std::clamp(v, 0.f, alpha ? 1.f : 1.f);
                        d[c] = v;
                    }
                    if (alpha) {
                        const float out_a = a + d[ch - 1] * (1 - a);
                        d[ch - 1] = std::clamp(out_a, 0.f, 1.f);
                        if (!fl) for (int c = 0; c < colours; ++c) d[c] = std::min(d[c], d[ch - 1]);
                    }
                    changed = true;
                }
            if (!changed) continue;
            pixel::TileWriter wr(f, pixel::transparent());
            wr.set_premultiplied(buf);
            const auto t = wr.freeze();
            const auto bytes = t ? bytes_of(*t) : std::vector<std::uint8_t>(kPx * static_cast<std::size_t>(f.bytes_per_pixel()), 0);
            const auto bpp = static_cast<std::size_t>(f.bytes_per_pixel());
            const pixel::Rect live = cell.intersect(image);
            const auto in_live = [&](int x, int y) {
                return !live.empty() && x >= live.x - cell.x && x < live.x - cell.x + live.w && y >= live.y - cell.y && y < live.y - cell.y + live.h;
            };
            if (!live.empty()) {   // the grid tile: beyond the image means nothing
                auto g = bytes;
                if (live != cell)
                    for (int y = 0; y < kTile; ++y)
                        for (int x = 0; x < kTile; ++x)
                            if (!in_live(x, y)) std::memset(g.data() + (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * bpp, 0, bpp);
                grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(base.cols()) + static_cast<std::size_t>(col)] =
                    zero_is_none(pixel::Tile::from_bytes(f, std::move(g)));
            }
            if (live != cell) {    // the outside tile: inside the image means nothing
                auto o = bytes;
                if (!live.empty())
                    for (int y = live.y - cell.y; y < live.y - cell.y + live.h; ++y)
                        std::memset(o.data() + (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(live.x - cell.x)) * bpp, 0,
                                    static_cast<std::size_t>(live.w) * bpp);
                auto ot = zero_is_none(pixel::Tile::from_bytes(f, std::move(o)));
                if (ot) outside[{col, row}] = ot;
                else outside.erase({col, row});
            }
        }
    }
    return base.with_grid(std::move(grid)).with_outside(std::move(outside));
}
}  // namespace

std::optional<pixel::TileMap> over_transformed(const pixel::TileMap& base, const pixel::TileMap& src,
                                               const pixel::TileMap* mask, const pixel::Rect& box,
                                               const Projective& m, Quality q,
                                               const std::function<bool()>& cancelled) {
    if (box.empty()) return base;
    ProjMapper mp(m, box, q);
    return resample(base, src, mask, box, mapped(box, m), q, cancelled, mp);
}

// ── Warp (s074) ─────────────────────────────────────────────────────────────
namespace {
// Bernstein cubic weights at t.
void bern(double t, double b[4]) {
    const double s = 1 - t;
    b[0] = s * s * s; b[1] = 3 * s * s * t; b[2] = 3 * s * t * t; b[3] = t * t * t;
}
}  // namespace

Mesh Mesh::of(const pixel::Rect& box) {
    Mesh m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m.p[r * 4 + c] = {box.x + box.w * c / 3.0, box.y + box.h * r / 3.0};
    return m;
}

Mesh Mesh::of(const pixel::Rect& box, const Projective& pm) {
    Mesh m = of(box);
    for (auto& q : m.p) q = pm.apply(q);
    return m;
}

Pt Mesh::at(double u, double v) const {
    double bu[4], bv[4];
    bern(u, bu);
    bern(v, bv);
    Pt o{0, 0};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const double w = bv[r] * bu[c];
            o.x += w * p[r * 4 + c].x;
            o.y += w * p[r * 4 + c].y;
        }
    return o;
}

bool Mesh::identity_for(const pixel::Rect& box) const {
    const Mesh id = of(box);
    for (int i = 0; i < 16; ++i)
        if (std::abs(p[i].x - id.p[i].x) > 1e-9 || std::abs(p[i].y - id.p[i].y) > 1e-9) return false;
    return true;
}

pixel::Rect mapped(const pixel::Rect&, const Mesh& m) {
    double x0 = std::numeric_limits<double>::max(), y0 = x0, x1 = -x0, y1 = -x0;
    for (const Pt q : m.p) { x0 = std::min(x0, q.x); y0 = std::min(y0, q.y); x1 = std::max(x1, q.x); y1 = std::max(y1, q.y); }
    constexpr double kFar = 1 << 20;
    if (x0 < -kFar || y0 < -kFar || x1 > kFar || y1 > kFar) return {};
    const int ix0 = static_cast<int>(std::floor(x0)), iy0 = static_cast<int>(std::floor(y0));
    const int ix1 = static_cast<int>(std::ceil(x1)), iy1 = static_cast<int>(std::ceil(y1));
    return {ix0, iy0, std::max(0, ix1 - ix0), std::max(0, iy1 - iy0)};
}

// ── s075: the presets ──────────────────────────────────────────────────────
const char* warp_style_name(WarpStyle s) {
    switch (s) {
        case WarpStyle::Custom:   return "Custom";
        case WarpStyle::Arc:      return "Arc";
        case WarpStyle::ArcLower: return "Arc Lower";
        case WarpStyle::ArcUpper: return "Arc Upper";
        case WarpStyle::Arch:     return "Arch";
        case WarpStyle::Bulge:    return "Bulge";
        case WarpStyle::Flag:     return "Flag";
        case WarpStyle::Wave:     return "Wave";
        case WarpStyle::Rise:     return "Rise";
        case WarpStyle::Squeeze:  return "Squeeze";
        case WarpStyle::Inflate:  return "Inflate";
    }
    return "";
}

Mesh warp_preset(WarpStyle s, const pixel::Rect& box, double bend_pct, bool vertical) {
    // In a unit square: X across (the bend's run), Y the way it bends (up -).
    // A Bezier's inner control points sit 4/3 past the curve's middle, so an
    // offset of K * t on them moves the curve's middle by t.
    const double b = std::clamp(bend_pct, -100.0, 100.0) / 100;
    constexpr double K = 4.0 / 3;
    double X[4][4], Y[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) { X[r][c] = c / 3.0; Y[r][c] = r / 3.0; }
    const auto bow = [&](int r, double t) { Y[r][1] -= K * t; Y[r][2] -= K * t; };   // row r's middle up by t
    switch (s) {
        case WarpStyle::Custom: break;
        case WarpStyle::Arc:      for (int r = 0; r < 4; ++r) bow(r, 0.5 * b * (1 - r / 6.0)); break;   // concentric: the top bends most
        case WarpStyle::ArcLower: for (int r = 0; r < 4; ++r) bow(r, -0.5 * b * r / 3.0); break;         // the bottom bows out
        case WarpStyle::ArcUpper: for (int r = 0; r < 4; ++r) bow(r, 0.5 * b * (3 - r) / 3.0); break;    // the top bows up
        case WarpStyle::Arch:     for (int r = 0; r < 4; ++r) bow(r, 0.5 * b); break;                     // every row alike
        case WarpStyle::Bulge:    for (int r = 0; r < 4; ++r) bow(r, 0.3 * b * (1.5 - r) / 1.5); break;   // top up, bottom down
        case WarpStyle::Flag:
            for (int r = 0; r < 4; ++r) { Y[r][1] -= 0.4 * b; Y[r][2] += 0.4 * b; }
            break;
        case WarpStyle::Wave:     // a flag whose middle rows ripple less
            for (int r = 0; r < 4; ++r) { const double a = (r == 0 || r == 3) ? 0.4 : 0.2; Y[r][1] -= a * b; Y[r][2] += a * b; }
            break;
        case WarpStyle::Rise:     // an S: the left end down, the right end up
            for (int r = 0; r < 4; ++r) { Y[r][0] += 0.25 * b; Y[r][1] += 0.25 * b; Y[r][2] -= 0.25 * b; Y[r][3] -= 0.25 * b; }
            break;
        case WarpStyle::Squeeze:  // the middle pinched, the ends kept
            Y[0][1] += K * 0.3 * b; Y[0][2] += K * 0.3 * b;
            Y[3][1] -= K * 0.3 * b; Y[3][2] -= K * 0.3 * b;
            break;
        case WarpStyle::Inflate:  // the inner four pushed out from the middle
            for (int r = 1; r <= 2; ++r)
                for (int c = 1; c <= 2; ++c) { X[r][c] = 0.5 + (X[r][c] - 0.5) * (1 + 1.5 * b); Y[r][c] = 0.5 + (Y[r][c] - 0.5) * (1 + 1.5 * b); }
            break;
    }
    Mesh m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            // Vertical: the same shape turned -- its run down the box, its bend across.
            const double ux = vertical ? Y[c][r] : X[r][c], uy = vertical ? X[c][r] : Y[r][c];
            m.p[r * 4 + c] = {box.x + ux * box.w, box.y + uy * box.h};
        }
    return m;
}

namespace {
// The patch cut into triangles (landing points + source points), binned by
// the tile cells they cover; each tile's pixels are looked up in a small map
// made when the tile comes.
class WarpMapper final : public Mapper {
public:
    WarpMapper(const Mesh& m, const pixel::Rect& box, Quality q) : m_box(box), m_best(q == Quality::Best) {
        // Fine enough that a straight piece is within a fraction of a pixel:
        // a step every ~6 px of the control net's size, 8 .. 160 a side.
        double span = 0;
        for (int i = 0; i < 16; ++i)
            for (int j = i + 1; j < 16; ++j) span = std::max(span, std::hypot(m.p[i].x - m.p[j].x, m.p[i].y - m.p[j].y));
        span = std::max({span, double(box.w), double(box.h)});
        const int n = std::clamp(static_cast<int>(std::ceil(span / 6)), 8, 160);
        // A margin of ~2 source px past the box: the edge's pixels sample the
        // source's soft edge (nothing past the box), as the projective's do.
        const double eu = 2.0 / std::max(1, box.w), ev = 2.0 / std::max(1, box.h);
        const int np = n + 1;
        std::vector<Pt> dst(static_cast<std::size_t>(np * np)), src(dst.size());
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i) {
                const double u = -eu + (1 + 2 * eu) * i / n, v = -ev + (1 + 2 * ev) * j / n;
                const auto k = static_cast<std::size_t>(j * np + i);
                dst[k] = m.at(u, v);
                src[k] = {box.x + u * box.w, box.y + v * box.h};
            }
        const auto tri = [&](std::size_t a, std::size_t b, std::size_t c) {
            Tri t{{dst[a], dst[b], dst[c]}, {src[a], src[b], src[c]}, 1, 1};
            // The source -> landing map's columns: how much a source px grows.
            const double s1x = t.s[1].x - t.s[0].x, s1y = t.s[1].y - t.s[0].y, s2x = t.s[2].x - t.s[0].x, s2y = t.s[2].y - t.s[0].y;
            const double d1x = t.d[1].x - t.d[0].x, d1y = t.d[1].y - t.d[0].y, d2x = t.d[2].x - t.d[0].x, d2y = t.d[2].y - t.d[0].y;
            const double det = s1x * s2y - s2x * s1y;
            if (std::abs((d1x * d2y - d2x * d1y)) < 1e-12 || std::abs(det) < 1e-12) return;   // flat: lands nowhere
            if (m_best) {
                const double gx = (d1x * s2y - d2x * s1y) / det, gy = (d1y * s2y - d2y * s1y) / det;     // d dst / d sx
                const double hx = (-d1x * s2x + d2x * s1x) / det, hy = (-d1y * s2x + d2y * s1x) / det;   // d dst / d sy
                t.kx = std::clamp(1.0 / std::max(1e-6, std::hypot(gx, gy)), 1.0, 16.0);
                t.ky = std::clamp(1.0 / std::max(1e-6, std::hypot(hx, hy)), 1.0, 16.0);
            }
            const int idx = static_cast<int>(m_tris.size());
            m_tris.push_back(t);
            const double x0 = std::min({t.d[0].x, t.d[1].x, t.d[2].x}), x1 = std::max({t.d[0].x, t.d[1].x, t.d[2].x});
            const double y0 = std::min({t.d[0].y, t.d[1].y, t.d[2].y}), y1 = std::max({t.d[0].y, t.d[1].y, t.d[2].y});
            for (int row = pixel::cell_of(static_cast<int>(std::floor(y0))); row <= pixel::cell_of(static_cast<int>(std::floor(y1))); ++row)
                for (int col = pixel::cell_of(static_cast<int>(std::floor(x0))); col <= pixel::cell_of(static_cast<int>(std::floor(x1))); ++col)
                    m_bins[{col, row}].push_back(idx);
        };
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const auto a = static_cast<std::size_t>(j * np + i), b = a + 1, c = a + static_cast<std::size_t>(np), d = c + 1;
                tri(a, b, d);
                tri(a, d, c);
            }
    }
    void cell(const pixel::Rect& hit) override {
        m_hit = hit;
        m_map.assign(static_cast<std::size_t>(hit.w) * static_cast<std::size_t>(hit.h), Hit{});
        const auto it = m_bins.find({pixel::cell_of(hit.x), pixel::cell_of(hit.y)});
        if (it == m_bins.end()) return;
        for (const int ti : it->second) {
            const Tri& t = m_tris[static_cast<std::size_t>(ti)];
            const double x0 = std::min({t.d[0].x, t.d[1].x, t.d[2].x}), x1 = std::max({t.d[0].x, t.d[1].x, t.d[2].x});
            const double y0 = std::min({t.d[0].y, t.d[1].y, t.d[2].y}), y1 = std::max({t.d[0].y, t.d[1].y, t.d[2].y});
            const int px0 = std::max(hit.x, static_cast<int>(std::floor(x0 - 0.5))), px1 = std::min(hit.x + hit.w - 1, static_cast<int>(std::ceil(x1 - 0.5)));
            const int py0 = std::max(hit.y, static_cast<int>(std::floor(y0 - 0.5))), py1 = std::min(hit.y + hit.h - 1, static_cast<int>(std::ceil(y1 - 0.5)));
            if (px0 > px1 || py0 > py1) continue;
            const double den = (t.d[1].y - t.d[2].y) * (t.d[0].x - t.d[2].x) + (t.d[2].x - t.d[1].x) * (t.d[0].y - t.d[2].y);
            const double eps = -1e-9 * std::abs(den);
            for (int y = py0; y <= py1; ++y)
                for (int x = px0; x <= px1; ++x) {
                    const double cx = x + 0.5, cy = y + 0.5;
                    double l0 = (t.d[1].y - t.d[2].y) * (cx - t.d[2].x) + (t.d[2].x - t.d[1].x) * (cy - t.d[2].y);
                    double l1 = (t.d[2].y - t.d[0].y) * (cx - t.d[2].x) + (t.d[0].x - t.d[2].x) * (cy - t.d[2].y);
                    if (den < 0) { l0 = -l0; l1 = -l1; }
                    const double ad = std::abs(den);
                    const double l2 = ad - l0 - l1;
                    if (l0 < eps || l1 < eps || l2 < eps) continue;
                    const double w0 = l0 / ad, w1 = l1 / ad, w2 = l2 / ad;
                    Hit& h = m_map[static_cast<std::size_t>(y - hit.y) * static_cast<std::size_t>(hit.w) + static_cast<std::size_t>(x - hit.x)];
                    h.on = true;
                    h.sp = {w0 * t.s[0].x + w1 * t.s[1].x + w2 * t.s[2].x, w0 * t.s[0].y + w1 * t.s[1].y + w2 * t.s[2].y};
                    h.kx = t.kx;
                    h.ky = t.ky;
                }
        }
    }
    bool at(int x, int y, Pt& sp, double& kx, double& ky) override {
        const Hit& h = m_map[static_cast<std::size_t>(y - m_hit.y) * static_cast<std::size_t>(m_hit.w) + static_cast<std::size_t>(x - m_hit.x)];
        if (!h.on) return false;
        sp = h.sp; kx = h.kx; ky = h.ky;
        return true;
    }

private:
    struct Tri { Pt d[3], s[3]; double kx, ky; };
    struct Hit { bool on = false; Pt sp{0, 0}; double kx = 1, ky = 1; };
    pixel::Rect m_box;
    bool m_best;
    std::vector<Tri> m_tris;
    std::map<Cell, std::vector<int>> m_bins;
    pixel::Rect m_hit;
    std::vector<Hit> m_map;
};
}  // namespace

namespace {
class FnMapper final : public Mapper {
public:
    explicit FnMapper(const std::function<bool(int, int, Pt&)>& f) : m_f(f) {}
    bool at(int x, int y, Pt& sp, double& kx, double& ky) override { kx = ky = 1; return m_f(x, y, sp); }
private:
    const std::function<bool(int, int, Pt&)>& m_f;
};
}  // namespace

std::optional<pixel::TileMap> over_mapped(const pixel::TileMap& base, const pixel::TileMap& src, const pixel::Rect& src_box,
                                          const pixel::Rect& land, const std::function<bool(int, int, Pt&)>& source,
                                          Quality q, const std::function<bool()>& cancelled) {
    if (land.empty() || src_box.empty()) return base;
    FnMapper mp(source);
    return resample(base, src, nullptr, src_box, land, q, cancelled, mp);
}

std::optional<pixel::TileMap> over_warped(const pixel::TileMap& base, const pixel::TileMap& src,
                                          const pixel::TileMap* mask, const pixel::Rect& box,
                                          const Mesh& m, Quality q, const std::function<bool()>& cancelled) {
    if (box.empty()) return base;
    const pixel::Rect land = mapped(box, m);
    if (land.empty()) return base;
    WarpMapper mp(m, box, q);
    return resample(base, src, mask, box, land, q, cancelled, mp);
}

// ── plans and verbs ─────────────────────────────────────────────────────────
std::string refusal(const Document& d, const std::vector<Document::Id>& selected, const Document::Id& mask_target) {
    if (auto why = move::refusal(d, selected, mask_target); !why.empty()) return why == "pick a layer to move" ? "pick a layer to transform" : why;
    return {};
}

// s058: where a layer mask HIDES anything (> 0), exactly; empty: it hides nothing.
pixel::Rect hidden_bounds(const pixel::TileMap& m) {
    int x0 = std::numeric_limits<int>::max(), y0 = x0, x1 = std::numeric_limits<int>::min(), y1 = x1;
    const auto take = [&](int x, int y) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); };
    const auto cell = [&](const pixel::TileMap::TileRef& t, int col, int row) {
        if (t->is_uniform()) {
            if (t->get(0, 0).c[0] <= 0.f) return;
            take(col * kTile, row * kTile);
            take(col * kTile + kTile - 1, row * kTile + kTile - 1);
            return;
        }
        for (int y = 0; y < kTile; ++y)
            for (int x = 0; x < kTile; ++x)
                if (t->get(x, y).c[0] > 0.f) take(col * kTile + x, row * kTile + y);
    };
    for (int row = 0; row < m.rows(); ++row)
        for (int col = 0; col < m.cols(); ++col)
            if (const auto t = m.tile(col, row)) cell(t, col, row);
    for (const auto& [c, t] : m.outside_tiles()) cell(t, c.first, c.second);
    if (x1 < x0) return {};
    // Clipped to the image's cells' reach is not needed: a mask beyond the
    // image moves with it too.
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

pixel::Rect box_of(const move::Plan& p) {
    if (p.mask) return canvas::bounds(*p.mask);
    int x0 = std::numeric_limits<int>::max(), y0 = x0, x1 = std::numeric_limits<int>::min(), y1 = x1;
    for (const auto& id : p.mask_only) {   // s058: an adjustment's mask -- what it hides
        const auto r = hidden_bounds(*p.masks.at(id));
        if (r.empty()) continue;
        x0 = std::min(x0, r.x); y0 = std::min(y0, r.y); x1 = std::max(x1, r.x + r.w); y1 = std::max(y1, r.y + r.h);
    }
    for (const auto& [id, px] : p.original) {
        if (!px) continue;
        const auto r = canvas::content_bounds(*px);
        if (r.empty()) continue;
        x0 = std::min(x0, r.x); y0 = std::min(y0, r.y); x1 = std::max(x1, r.x + r.w); y1 = std::max(y1, r.y + r.h);
    }
    if (x1 <= x0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

std::optional<Plan> plan(const Document& d, const std::vector<Document::Id>& selected, const pixel::Px& background,
                         const Document::Id& mask_target) {
    auto mp = move::plan(d, selected, background, mask_target);
    if (!mp) return std::nullopt;
    Plan p{std::move(*mp), {}};
    p.box = box_of(p.p);
    if (p.box.empty()) return std::nullopt;   // nothing there to transform
    return p;
}

std::string projective_refusal(const Plan& p) {
    if (!p.p.texts.empty()) return "a type layer stays type -- Layer > Rasterize Type to distort it (Skew keeps it type)";
    return {};
}

namespace {
// s074: the layers, their masks and the selection under any map `xf` (the
// projective resample, or a warp's). `type_map`: what type is mapped by --
// null: type isn't (a distort or a warp leaves it as it was). `mask_box`:
// a whole layer mask is mapped over its own extent (true -- a projective
// map reaches everywhere) or over the plan's box (false -- a warp's mesh
// lives on the box).
using XfFn = std::function<std::optional<pixel::TileMap>(const pixel::TileMap&, const pixel::TileMap&, const pixel::TileMap*,
                                                         const pixel::Rect&, Quality)>;
std::optional<move::Result> compute_with(const Plan& pl, bool identity, const Affine* type_map, Quality q,
                                         const std::function<bool()>& cancelled, const XfFn& xf, bool mask_extent) {
    const auto& p = pl.p;
    move::Result r;
    const bool flat = type_map != nullptr;
    const Affine m = flat ? *type_map : Affine{};
    if (identity) {
        for (const auto& id : p.ids) r.pixels[id] = p.original.at(id);
        for (const auto& [id, t] : p.texts) r.texts[id] = t;   // s046
        r.mask = p.mask;
        return r;
    }
    // A map that only permutes pixels (flips, quarter turns, whole-pixel
    // moves): sampled exactly, lossless.
    const auto whole = [](double v) { return std::abs(v - std::round(v)) < 1e-9; };
    const auto unit = [&](double v) { return whole(v) && std::abs(v) <= 1; };
    if (flat && unit(m.a) && unit(m.b) && unit(m.c) && unit(m.d) && whole(m.e) && whole(m.f) &&
        std::abs(std::abs(m.a * m.d - m.b * m.c) - 1) < 1e-9)
        q = Quality::Fast;
    const pixel::TileMap blank(p.format, p.width, p.height);
    // s046: TYPE transforms as type -- the map composed into its Spec's
    // matrix and set again, so a scaled, turned or slanted text layer stays
    // live and sharp (and a selection doesn't cut it: it goes whole).
    for (const auto& [id, t] : p.texts) {
        if (cancelled && cancelled()) return std::nullopt;
        if (!flat) { r.pixels[id] = p.original.at(id); r.texts[id] = t; continue; }   // s073: type isn't distorted
        auto mapped = std::make_shared<const text::Spec>(text::mapped(*t, {m.a, m.b, m.c, m.d, m.e, m.f}));
        Image im;
        im.width = p.width;
        im.height = p.height;
        im.format = p.format;
        r.pixels[id] = text_pixels(im, *mapped);
        r.texts[id] = std::move(mapped);
    }
    for (const auto& id : p.ids) {
        if (p.texts.count(id)) continue;   // s046: above
        const auto& o = p.original.at(id);
        if (!o) { r.pixels[id] = p.mask ? p.base.at(id) : nullptr; continue; }
        pixel::TileMap base = blank;
        if (p.mask) { if (const auto& b = p.base.at(id)) base = *b; }
        else if (!p.format.alpha) base = blank.fill({0, 0, p.width, p.height}, p.fill);   // no alpha: the background shows where it left
        auto out = xf(base, *o, p.mask.get(), pl.box, q);
        if (!out) return std::nullopt;
        r.pixels[id] = out->tile_count() ? std::make_shared<const pixel::TileMap>(std::move(*out)) : nullptr;
    }
    // s012: a whole layer's mask is transformed with it (a selection's
    // pixels move alone: the mask stays where it is).
    if (!p.mask)
        for (const auto& [id, lm] : p.masks) {
            int x0 = 0, y0 = 0, x1 = lm->width(), y1 = lm->height();
            for (const auto& [cell, t] : lm->outside_tiles()) {
                x0 = std::min(x0, cell.first * kTile); y0 = std::min(y0, cell.second * kTile);
                x1 = std::max(x1, (cell.first + 1) * kTile); y1 = std::max(y1, (cell.second + 1) * kTile);
            }
            const pixel::TileMap mblank(lm->format(), p.width, p.height);
            auto out = xf(mblank, *lm, nullptr, mask_extent ? pixel::Rect{x0, y0, x1 - x0, y1 - y0} : pl.box, q);
            if (!out) return std::nullopt;
            r.masks[id] = std::make_shared<const pixel::TileMap>(std::move(*out));
        }
    if (p.mask) {
        const pixel::TileMap mblank(p.mask->format(), p.width, p.height);
        auto mm = xf(mblank, *p.mask, nullptr, pl.box, Quality::Fast);
        if (!mm) return std::nullopt;
        auto in = mm->without_outside();   // a selection holds nothing beyond the canvas
        r.mask = select::empty(in) ? nullptr : std::make_shared<const pixel::TileMap>(std::move(in));
    }
    return r;
}

}  // namespace

std::optional<move::Result> compute(const Plan& pl, const Projective& pm, Quality q, const std::function<bool()>& cancelled) {
    const Affine am = pm.to_affine();
    return compute_with(pl, pm.identity(), pm.affine() ? &am : nullptr, q, cancelled,
                        [&](const pixel::TileMap& base, const pixel::TileMap& src, const pixel::TileMap* mask, const pixel::Rect& box, Quality qq) {
                            return over_transformed(base, src, mask, box, pm, qq, cancelled);
                        }, true);
}

std::optional<move::Result> compute_warp(const Plan& pl, const Mesh& mesh, Quality q, const std::function<bool()>& cancelled) {
    return compute_with(pl, mesh.identity_for(pl.box), nullptr, q, cancelled,
                        [&](const pixel::TileMap& base, const pixel::TileMap& src, const pixel::TileMap* mask, const pixel::Rect& box, Quality qq) {
                            return over_warped(base, src, mask, box, mesh, qq, cancelled);   // box: always the plan's
                        }, false);
}

bool apply(DocJournal& j, Document& d, const std::vector<Document::Id>& selected, const Projective& m,
           const std::string& label, const pixel::Px& background) {
    if (m.identity()) return false;
    const auto p = plan(d, selected, background);
    if (!p) return false;
    if (!m.affine() && !projective_refusal(*p).empty()) return false;
    const auto r = compute(*p, m, Quality::Best);
    if (!r) return false;
    return j.run(label, move::step_ids(p->p), [&] { move::put(d, p->p, *r); });
}

bool distort(DocJournal& j, Document& d, const std::vector<Document::Id>& selected, const Quad& q,
             const std::string& label, const pixel::Px& background) {
    const auto p = plan(d, selected, background);
    if (!p) return false;
    const auto m = Projective::box_to_quad(p->box, q);
    if (!m) return false;
    return apply(j, d, selected, *m, label, background);
}

bool warp(DocJournal& j, Document& d, const std::vector<Document::Id>& selected,
          const std::function<Mesh(const pixel::Rect&)>& bend, const std::string& label, const pixel::Px& background) {
    const auto p = plan(d, selected, background);
    if (!p || !projective_refusal(*p).empty()) return false;
    const Mesh m = bend(p->box);
    if (m.identity_for(p->box)) return false;
    const auto r = compute_warp(*p, m, Quality::Best);
    if (!r) return false;
    return j.run(label, move::step_ids(p->p), [&] { move::put(d, p->p, *r); });
}

Affine flip_about(const pixel::Rect& b, bool horizontal) {
    const Pt c{b.x + b.w / 2.0, b.y + b.h / 2.0};
    return Affine::about(c, horizontal ? Affine::scale(-1, 1) : Affine::scale(1, -1));
}

Affine rotate_about(const pixel::Rect& b, double degrees) {
    const Pt c{b.x + b.w / 2.0, b.y + b.h / 2.0};
    Affine m = Affine::about(c, Affine::rotate(degrees * std::numbers::pi / 180));
    // A quarter turn of an odd-by-even box would land on half pixels: shift
    // it so its corner is on a whole one, and every pixel lands on a pixel.
    if (std::fmod(std::abs(degrees), 90.0) == 0) {
        double x0 = std::numeric_limits<double>::max(), y0 = x0;
        for (const Pt p : {Pt{double(b.x), double(b.y)}, Pt{double(b.x + b.w), double(b.y)},
                           Pt{double(b.x), double(b.y + b.h)}, Pt{double(b.x + b.w), double(b.y + b.h)}}) {
            const Pt q = m.apply(p);
            x0 = std::min(x0, q.x); y0 = std::min(y0, q.y);
        }
        m.e += std::round(x0) - x0;
        m.f += std::round(y0) - y0;
    }
    return m;
}

bool flip(DocJournal& j, Document& d, const std::vector<Document::Id>& selected, bool horizontal, const pixel::Px& background) {
    const auto p = plan(d, selected, background);
    if (!p) return false;
    return apply(j, d, selected, flip_about(p->box, horizontal), horizontal ? "Flip Horizontal" : "Flip Vertical", background);
}

bool rotate(DocJournal& j, Document& d, const std::vector<Document::Id>& selected, double degrees, const pixel::Px& background) {
    const auto p = plan(d, selected, background);
    if (!p) return false;
    const char* label = degrees == 180 || degrees == -180 ? "Rotate 180°" : degrees > 0 ? "Rotate 90° Clockwise" : "Rotate 90° Counter Clockwise";
    return apply(j, d, selected, rotate_about(p->box, degrees), label, background);
}

// ── the whole canvas (s011 m3) ──────────────────────────────────────────────
namespace {
// Everything a map holds, as a box (the image, and every outside cell).
pixel::Rect held_box(const pixel::TileMap& m) {
    int x0 = 0, y0 = 0, x1 = m.width(), y1 = m.height();
    for (const auto& [cell, t] : m.outside_tiles()) {
        x0 = std::min(x0, cell.first * kTile); y0 = std::min(y0, cell.second * kTile);
        x1 = std::max(x1, (cell.first + 1) * kTile); y1 = std::max(y1, (cell.second + 1) * kTile);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

// Every layer (and, with keep_mask, the selection) mapped by m onto a w x h
// canvas.
std::optional<canvas::Reshape> reshape(const canvas::Before& b, int w, int h, const Affine& m, Quality q,
                                       const pixel::Px& background, bool keep_mask, const std::function<bool()>& cancelled) {
    if (!b.image || !canvas::size_refusal(w, h).empty()) return std::nullopt;
    Image next = *b.image;
    next.width = w;
    next.height = h;
    // s031 / s032: the zero point maps with the picture; a guide too when the
    // turn keeps it upright or level (90-degree turns, flips) -- otherwise
    // it stays where it was, as Photoshop's do.
    {
        const Pt o = m.apply({b.image->origin_x, b.image->origin_y});
        next.origin_x = o.x;
        next.origin_y = o.y;
        for (auto& g : next.guides) {
            const Pt p0 = m.apply(g.vertical ? Pt{g.at, 0} : Pt{0, g.at});
            const Pt p1 = m.apply(g.vertical ? Pt{g.at, 1} : Pt{1, g.at});
            if (std::abs(p0.x - p1.x) < 1e-9) g = {true, p0.x};
            else if (std::abs(p0.y - p1.y) < 1e-9) g = {false, p0.y};
        }
    }
    canvas::Reshape out;
    out.image = std::make_shared<const Image>(next);
    out.path_map = [m](path::P q) { const Pt t = m.apply({q.x, q.y}); return path::P{t.x, t.y}; };   // s035: any angle -- a Bezier maps exactly
    const pixel::Format f = next.format;
    const pixel::TileMap blank(f, w, h);
    const pixel::TileMap base = f.alpha ? blank : blank.fill({0, 0, w, h}, background);
    for (const auto& [id, px] : b.pixels) {
        if (!px) { out.pixels[id] = nullptr; continue; }
        auto r = over_transformed(base, *px, nullptr, held_box(*px), m, q, cancelled);
        if (!r) return std::nullopt;
        out.pixels[id] = r->tile_count() ? std::make_shared<const pixel::TileMap>(std::move(*r)) : nullptr;
    }
    for (const auto& [id, lm] : b.masks) {   // s012: each layer mask turns with its layer
        const pixel::TileMap mblank(lm->format(), w, h);
        auto r = over_transformed(mblank, *lm, nullptr, held_box(*lm), m, q, cancelled);
        if (!r) return std::nullopt;
        out.masks[id] = std::make_shared<const pixel::TileMap>(std::move(*r));
    }
    if (keep_mask && b.mask) {
        const pixel::TileMap mblank(b.mask->format(), w, h);
        auto r = over_transformed(mblank, *b.mask, nullptr, {0, 0, b.mask->width(), b.mask->height()}, m, Quality::Fast, cancelled);
        if (!r) return std::nullopt;
        auto in = r->without_outside();
        out.mask = select::empty(in) ? nullptr : std::make_shared<const pixel::TileMap>(std::move(in));
    }
    return out;
}
}  // namespace

const char* turn_label(Turn t) {
    switch (t) {
        case Turn::Clockwise:        return "Rotate Canvas 90° Clockwise";
        case Turn::CounterClockwise: return "Rotate Canvas 90° Counter Clockwise";
        case Turn::Half:             return "Rotate Canvas 180°";
        case Turn::FlipHorizontal:   return "Flip Canvas Horizontal";
        case Turn::FlipVertical:     return "Flip Canvas Vertical";
    }
    return "";
}

std::optional<canvas::Reshape> turn_plan(const canvas::Before& b, Turn t, const std::function<bool()>& cancelled) {
    if (!b.image) return std::nullopt;
    const double W = b.image->width, H = b.image->height;
    Affine m;
    int w = b.image->width, h = b.image->height;
    switch (t) {
        case Turn::Clockwise:        m = {0, 1, -1, 0, H, 0}; std::swap(w, h); break;   // (x, y) -> (H - y, x)
        case Turn::CounterClockwise: m = {0, -1, 1, 0, 0, W}; std::swap(w, h); break;   // (x, y) -> (y, W - x)
        case Turn::Half:             m = {-1, 0, 0, -1, W, H}; break;
        case Turn::FlipHorizontal:   m = {-1, 0, 0, 1, W, 0}; break;
        case Turn::FlipVertical:     m = {1, 0, 0, -1, 0, H}; break;
    }
    return reshape(b, w, h, m, Quality::Fast, pixel::transparent(), true, cancelled);
}

std::optional<canvas::Reshape> rotate_plan(const canvas::Before& b, double degrees, const pixel::Px& background,
                                           const std::function<bool()>& cancelled) {
    if (!b.image) return std::nullopt;
    const double r = std::remainder(degrees, 360.0);
    if (std::abs(r) < 1e-9) return std::nullopt;
    const double W = b.image->width, H = b.image->height;
    const Affine turn = Affine::rotate(r * std::numbers::pi / 180);
    const pixel::Rect land = mapped({0, 0, b.image->width, b.image->height}, Affine::about({W / 2, H / 2}, turn));
    const int w = land.w, h = land.h;
    const Affine m = Affine::translate(w / 2.0, h / 2.0) * turn * Affine::translate(-W / 2, -H / 2);
    return reshape(b, w, h, m, Quality::Best, background, true, cancelled);
}

double straighten_angle(Pt p, Pt q) {
    double a = std::atan2(q.y - p.y, q.x - p.x) * 180 / std::numbers::pi;   // the line's angle, y down
    // Nearer vertical: make it upright instead.
    a = std::remainder(a, 180.0);                       // -90 .. 90
    if (a > 45) a -= 90;
    else if (a < -45) a += 90;
    return -a;
}

std::optional<canvas::Reshape> straighten_plan(const canvas::Before& b, Pt p, Pt q, const pixel::Px& background,
                                               const std::function<bool()>& cancelled) {
    if (!b.image || std::hypot(q.x - p.x, q.y - p.y) < 4) return std::nullopt;
    const double deg = straighten_angle(p, q);
    if (std::abs(deg) < 0.01) return std::nullopt;
    const double W = b.image->width, H = b.image->height;
    const double phi = std::abs(deg) * std::numbers::pi / 180, cs = std::cos(phi), sn = std::sin(phi);
    // The largest W:H rectangle, centred, inside the turned image.
    const double s = std::min(W / (W * cs + H * sn), H / (W * sn + H * cs));
    const int w = std::max(1, static_cast<int>(std::floor(W * s))), h = std::max(1, static_cast<int>(std::floor(H * s)));
    const Affine m = Affine::translate(w / 2.0, h / 2.0) * Affine::rotate(deg * std::numbers::pi / 180) *
                     Affine::translate(-W / 2, -H / 2);
    return reshape(b, w, h, m, Quality::Best, background, false, cancelled);
}

bool turn(DocJournal& j, Document& d, Turn t) {
    const auto b = canvas::before(d);
    const auto p = turn_plan(b, t);
    return p && canvas::land(j, d, b, *p, turn_label(t));
}

bool rotate_canvas(DocJournal& j, Document& d, double degrees, const pixel::Px& background) {
    const auto b = canvas::before(d);
    const auto p = rotate_plan(b, degrees, background);
    return p && canvas::land(j, d, b, *p, "Rotate Canvas");
}

bool straighten(DocJournal& j, Document& d, Pt p, Pt q, const pixel::Px& background) {
    const auto b = canvas::before(d);
    const auto r = straighten_plan(b, p, q, background);
    return r && canvas::land(j, d, b, *r, "Straighten");
}

}  // namespace klimt::core::xform
