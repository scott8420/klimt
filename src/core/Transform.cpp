#include "core/Transform.hpp"
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
std::optional<pixel::TileMap> over_transformed(const pixel::TileMap& base, const pixel::TileMap& src,
                                               const pixel::TileMap* mask, const pixel::Rect& box,
                                               const Affine& m, Quality q,
                                               const std::function<bool()>& cancelled) {
    if (box.empty()) return base;
    const pixel::Format f = base.format();
    const int ch = f.channels();
    const auto chs = static_cast<std::size_t>(ch);
    const bool alpha = f.alpha, fl = f.depth == pixel::Depth::F32;
    const int w = base.width(), h = base.height();
    const Affine inv = m.inverse();
    // Where the box lands, a pixel of slack for the filter's reach.
    pixel::Rect land = mapped(box, m);
    land = {land.x - 2, land.y - 2, land.w + 4, land.h + 4};
    if (land.empty()) return base;

    Sampler s(src, box);
    const int sn = s.out_channels();   // ch (alpha) or ch + 1
    std::optional<Sampler> ms;
    if (mask) ms.emplace(*mask, pixel::Rect{0, 0, mask->width(), mask->height()});

    // The filter: bilinear, or bicubic widened along a source axis the map shrinks.
    const double scale_x = std::hypot(m.a, m.b), scale_y = std::hypot(m.c, m.d);
    const double kx = q == Quality::Best ? std::max(1.0, 1.0 / std::max(1e-6, scale_x)) : 1.0;
    const double ky = q == Quality::Best ? std::max(1.0, 1.0 / std::max(1e-6, scale_y)) : 1.0;
    const double reach_x = q == Quality::Best ? 2 * kx : 1, reach_y = q == Quality::Best ? 2 * ky : 1;

    float acc[pixel::kMaxChannels + 1], tap[pixel::kMaxChannels + 1];
    const auto sample = [&](double u, double v, float* out) {
        std::fill_n(out, sn, 0.f);
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
        const int x0 = static_cast<int>(std::ceil(cu - reach_x)), x1 = static_cast<int>(std::floor(cu + reach_x));
        const int y0 = static_cast<int>(std::ceil(cv - reach_y)), y1 = static_cast<int>(std::floor(cv + reach_y));
        double sum = 0;
        for (int yy = y0; yy <= y1; ++yy) {
            const double wy = mitchell((yy - cv) / ky);
            if (wy == 0) continue;
            for (int xx = x0; xx <= x1; ++xx) {
                const double wgt = wy * mitchell((xx - cu) / kx);
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
            for (int y = hit.y; y < hit.y + hit.h; ++y)
                for (int x = hit.x; x < hit.x + hit.w; ++x) {
                    const Pt sp = inv.apply({x + 0.5, y + 0.5});
                    if (sp.x < box.x - reach_x - 1 || sp.y < box.y - reach_y - 1 || sp.x > box.x + box.w + reach_x + 1 ||
                        sp.y > box.y + box.h + reach_y + 1)
                        continue;
                    sample(sp.x, sp.y, acc);
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

// ── plans and verbs ─────────────────────────────────────────────────────────
std::string refusal(const Document& d, const std::vector<Document::Id>& selected) {
    if (auto why = move::refusal(d, selected); !why.empty()) return why == "pick a layer to move" ? "pick a layer to transform" : why;
    return {};
}

pixel::Rect box_of(const move::Plan& p) {
    if (p.mask) return canvas::bounds(*p.mask);
    int x0 = std::numeric_limits<int>::max(), y0 = x0, x1 = std::numeric_limits<int>::min(), y1 = x1;
    for (const auto& [id, px] : p.original) {
        if (!px) continue;
        const auto r = canvas::content_bounds(*px);
        if (r.empty()) continue;
        x0 = std::min(x0, r.x); y0 = std::min(y0, r.y); x1 = std::max(x1, r.x + r.w); y1 = std::max(y1, r.y + r.h);
    }
    if (x1 <= x0) return {};
    return {x0, y0, x1 - x0, y1 - y0};
}

std::optional<Plan> plan(const Document& d, const std::vector<Document::Id>& selected, const pixel::Px& background) {
    auto mp = move::plan(d, selected, background);
    if (!mp) return std::nullopt;
    Plan p{std::move(*mp), {}};
    p.box = box_of(p.p);
    if (p.box.empty()) return std::nullopt;   // nothing there to transform
    return p;
}

std::optional<move::Result> compute(const Plan& pl, const Affine& m, Quality q, const std::function<bool()>& cancelled) {
    const auto& p = pl.p;
    move::Result r;
    if (m.identity()) {
        for (const auto& id : p.ids) r.pixels[id] = p.original.at(id);
        r.mask = p.mask;
        return r;
    }
    // A map that only permutes pixels (flips, quarter turns, whole-pixel
    // moves): sampled exactly, lossless.
    const auto whole = [](double v) { return std::abs(v - std::round(v)) < 1e-9; };
    const auto unit = [&](double v) { return whole(v) && std::abs(v) <= 1; };
    if (unit(m.a) && unit(m.b) && unit(m.c) && unit(m.d) && whole(m.e) && whole(m.f) &&
        std::abs(std::abs(m.a * m.d - m.b * m.c) - 1) < 1e-9)
        q = Quality::Fast;
    const pixel::TileMap blank(p.format, p.width, p.height);
    for (const auto& id : p.ids) {
        const auto& o = p.original.at(id);
        if (!o) { r.pixels[id] = p.mask ? p.base.at(id) : nullptr; continue; }
        pixel::TileMap base = blank;
        if (p.mask) { if (const auto& b = p.base.at(id)) base = *b; }
        else if (!p.format.alpha) base = blank.fill({0, 0, p.width, p.height}, p.fill);   // no alpha: the background shows where it left
        auto out = over_transformed(base, *o, p.mask.get(), pl.box, m, q, cancelled);
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
            auto out = over_transformed(mblank, *lm, nullptr, {x0, y0, x1 - x0, y1 - y0}, m, q, cancelled);
            if (!out) return std::nullopt;
            r.masks[id] = std::make_shared<const pixel::TileMap>(std::move(*out));
        }
    if (p.mask) {
        const pixel::TileMap mblank(p.mask->format(), p.width, p.height);
        auto mm = over_transformed(mblank, *p.mask, nullptr, pl.box, m, Quality::Fast, cancelled);
        if (!mm) return std::nullopt;
        auto in = mm->without_outside();   // a selection holds nothing beyond the canvas
        r.mask = select::empty(in) ? nullptr : std::make_shared<const pixel::TileMap>(std::move(in));
    }
    return r;
}

bool apply(DocJournal& j, Document& d, const std::vector<Document::Id>& selected, const Affine& m,
           const std::string& label, const pixel::Px& background) {
    if (m.identity()) return false;
    const auto p = plan(d, selected, background);
    if (!p) return false;
    const auto r = compute(*p, m, Quality::Best);
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
