#include "core/Canvas.hpp"
#include "core/Selection.hpp"
#include "formats/Formats.hpp"   // kMaxSide

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace klimt::core::canvas {

namespace {
using pixel::kTile;
constexpr std::size_t kPx = static_cast<std::size_t>(kTile) * kTile;

std::vector<std::uint8_t> pixel_bytes(const pixel::Format& f, const pixel::Px& p) {
    const auto t = pixel::Tile::uniform(f, p);
    const auto n = static_cast<std::size_t>(f.bytes_per_pixel());
    if (!t) return std::vector<std::uint8_t>(n, 0);
    return std::vector<std::uint8_t>(t->data(), t->data() + n);
}

// An absent tile reads as zero bytes in every format, so a tile of all
// zeros is no tile (from_bytes keeps a no-alpha one as uniform black).
pixel::TileMap::TileRef zero_is_none(pixel::TileMap::TileRef t) {
    if (!t || !t->is_uniform()) return t;
    const auto n = static_cast<std::size_t>(t->format().bytes_per_pixel());
    for (std::size_t i = 0; i < n; ++i) if (t->data()[i]) return t;
    return nullptr;
}

MapPtr held(pixel::TileMap m) {
    return m.tile_count() ? std::make_shared<const pixel::TileMap>(std::move(m)) : nullptr;
}

// ── the resampler's taps: for each output index, the source indices it
// reads (contiguous from `start`) and their weights (summing to 1).
struct Taps {
    int start = 0;
    std::vector<float> w;
};

double mitchell(double x) {   // B = C = 1/3
    constexpr double B = 1.0 / 3, C = 1.0 / 3;
    x = std::abs(x);
    if (x < 1) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6;
    if (x < 2) return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6;
    return 0;
}

std::vector<Taps> make_taps(int src_n, int dst_n, Filter f) {
    std::vector<Taps> out(static_cast<std::size_t>(dst_n));
    const double scale = static_cast<double>(src_n) / dst_n;   // source pixels per output pixel
    for (int i = 0; i < dst_n; ++i) {
        Taps& t = out[static_cast<std::size_t>(i)];
        if (f == Filter::Nearest) {
            t.start = std::clamp(static_cast<int>(std::floor((i + 0.5) * scale)), 0, src_n - 1);
            t.w.assign(1, 1.f);   // (not = {1.f}: GCC 13 + sanitizers warn falsely)
            continue;
        }
        const double centre = (i + 0.5) * scale - 0.5;
        const double fs = std::max(1.0, scale);   // shrinking: a wider kernel (area-correct)
        const double support = 2 * fs;
        const int lo = static_cast<int>(std::ceil(centre - support)), hi = static_cast<int>(std::floor(centre + support));
        const int a = std::clamp(lo, 0, src_n - 1), b = std::clamp(hi, 0, src_n - 1);
        t.start = a;
        std::vector<double> w(static_cast<std::size_t>(b - a + 1), 0.0);
        double sum = 0;
        for (int j = lo; j <= hi; ++j) {
            const double k = mitchell((j - centre) / fs);
            w[static_cast<std::size_t>(std::clamp(j, 0, src_n - 1) - a)] += k;   // the edge pixel repeats
            sum += k;
        }
        t.w.resize(w.size());
        for (std::size_t k = 0; k < w.size(); ++k) t.w[k] = static_cast<float>(sum != 0 ? w[k] / sum : 0);
    }
    return out;
}

// A source map read row by row as PREMULTIPLIED float, a tile row decoded
// at a time and dropped once the rows below it are past.
class RowReader {
public:
    explicit RowReader(const pixel::TileMap& m) : m_m(m), m_ch(static_cast<std::size_t>(m.format().channels())) {}
    // Row y, all W pixels, ch floats each.
    void row(int y, std::vector<float>& out) {
        const int ty = y / kTile;
        auto& tiles = decoded(ty);
        out.assign(static_cast<std::size_t>(m_m.width()) * m_ch, 0.f);
        const auto ry = static_cast<std::size_t>(y - ty * kTile);
        for (int c = 0; c < m_m.cols(); ++c) {
            const auto& t = tiles[static_cast<std::size_t>(c)];
            if (t.empty()) continue;
            const int x0 = c * kTile, n = std::min(kTile, m_m.width() - x0);
            std::memcpy(out.data() + static_cast<std::size_t>(x0) * m_ch, t.data() + ry * kTile * m_ch,
                        static_cast<std::size_t>(n) * m_ch * sizeof(float));
        }
    }
    void drop_below(int y) {
        const int ty = y / kTile;
        for (auto it = m_rows.begin(); it != m_rows.end();)
            it = it->first < ty ? m_rows.erase(it) : std::next(it);
    }

private:
    std::vector<std::vector<float>>& decoded(int ty) {
        auto it = m_rows.find(ty);
        if (it != m_rows.end()) return it->second;
        auto& v = m_rows[ty];
        v.resize(static_cast<std::size_t>(m_m.cols()));
        for (int c = 0; c < m_m.cols(); ++c)
            if (const auto t = m_m.tile(c, ty)) t->premultiplied(v[static_cast<std::size_t>(c)]);
        return v;
    }
    const pixel::TileMap& m_m;
    std::size_t m_ch;
    std::map<int, std::vector<std::vector<float>>> m_rows;
};

Reshape same_layers(const Before& b) {
    Reshape r;
    r.pixels = b.pixels;
    r.mask = b.mask;
    r.masks = b.masks;
    return r;
}
}  // namespace

const char* filter_name(Filter f) { return f == Filter::Nearest ? "Nearest" : "Smooth (bicubic)"; }

namespace {
// Copy n pixels of `src` starting at (sx, sy) along x into dst: inside the
// image from the grid (absent: zero bytes), outside it from the outside
// tiles (absent: dst left as it is -- the fill). `got` is set when a held
// tile gave pixels.
void sample_run(const pixel::TileMap& src, int sx, int sy, int n, std::uint8_t* dst, std::size_t bpp, bool& got) {
    const int sw = src.width(), sh = src.height();
    const bool row_in = sy >= 0 && sy < sh;
    while (n > 0) {
        const bool in = row_in && sx >= 0 && sx < sw;
        const int col = pixel::cell_of(sx), row = pixel::cell_of(sy);
        int end = (col + 1) * kTile;                       // the cell's right edge
        if (row_in) {                                      // and the image's edges
            if (sx < 0) end = std::min(end, 0);
            else if (sx < sw) end = std::min(end, sw);
        }
        const int m = std::min(n, end - sx);
        const auto t = in ? src.tile(col, row) : src.outside(col, row);
        if (t) {
            got = true;
            if (t->is_uniform()) {
                for (int k = 0; k < m; ++k) std::memcpy(dst + static_cast<std::size_t>(k) * bpp, t->data(), bpp);
            } else {
                const std::size_t off = (static_cast<std::size_t>(sy - row * kTile) * kTile + static_cast<std::size_t>(sx - col * kTile)) * bpp;
                std::memcpy(dst, t->data() + off, static_cast<std::size_t>(m) * bpp);
            }
        } else if (in) {
            std::memset(dst, 0, static_cast<std::size_t>(m) * bpp);   // transparent, inside the image
        }
        dst += static_cast<std::size_t>(m) * bpp;
        sx += m;
        n -= m;
    }
}
}  // namespace

pixel::TileMap reframed(const pixel::TileMap& src, int w, int h, int ox, int oy, const pixel::Px& fill, bool keep_outside) {
    const auto& f = src.format();
    const int sw = src.width(), sh = src.height();
    if (w == sw && h == sh && ox == 0 && oy == 0) return keep_outside ? src : src.without_outside();
    const auto bpp = static_cast<std::size_t>(f.bytes_per_pixel());
    const auto fb = pixel_bytes(f, fill);
    const bool fill_zero = std::all_of(fb.begin(), fb.end(), [](std::uint8_t b) { return b == 0; });
    const pixel::Rect image{0, 0, w, h};
    const pixel::TileMap shape(f, w, h);

    // The cells to make: the new image's, and (keeping) every cell a held
    // source tile lands on.
    std::set<pixel::TileMap::Cell> cells;
    if (keep_outside) {
        const auto add = [&](int col, int row) {
            const int x0 = col * kTile + ox, y0 = row * kTile + oy;
            for (int r = pixel::cell_of(y0); r <= pixel::cell_of(y0 + kTile - 1); ++r)
                for (int c = pixel::cell_of(x0); c <= pixel::cell_of(x0 + kTile - 1); ++c) cells.insert({c, r});
        };
        for (int row = 0; row < src.rows(); ++row)
            for (int col = 0; col < src.cols(); ++col)
                if (src.tile(col, row)) add(col, row);
        for (const auto& [cell, t] : src.outside_tiles()) add(cell.first, cell.second);
    }
    for (int row = 0; row < shape.rows(); ++row)
        for (int col = 0; col < shape.cols(); ++col) cells.insert({col, row});

    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(shape.cols()) * static_cast<std::size_t>(shape.rows()));
    pixel::TileMap::Outside outside;
    std::vector<std::uint8_t> buf;
    for (const auto& [col, row] : cells) {
        const pixel::Rect cell{col * kTile, row * kTile, kTile, kTile};
        const pixel::Rect live = cell.intersect(image);          // the part inside the new image
        const bool in_grid = col >= 0 && row >= 0 && col < shape.cols() && row < shape.rows();
        buf.assign(kPx * bpp, 0);
        if (!fill_zero)
            for (std::size_t i = 0; i < kPx; ++i) std::memcpy(buf.data() + i * bpp, fb.data(), bpp);
        bool got_out = false;   // a held tile gave pixels to the part OUTSIDE the image
        for (int y = 0; y < kTile; ++y) {
            const int iy = cell.y + y;
            const bool row_live = iy >= 0 && iy < h;
            std::uint8_t* d = buf.data() + static_cast<std::size_t>(y) * kTile * bpp;
            if (!row_live || live.empty()) {
                if (keep_outside) sample_run(src, cell.x - ox, iy - oy, kTile, d, bpp, got_out);
                continue;
            }
            // Left of the image, the image, right of it.
            const int a = live.x - cell.x, b = a + live.w;
            bool got = false;
            if (keep_outside && a > 0) sample_run(src, cell.x - ox, iy - oy, a, d, bpp, got_out);
            sample_run(src, live.x - ox, iy - oy, live.w, d + static_cast<std::size_t>(a) * bpp, bpp, got);
            if (keep_outside && b < kTile) sample_run(src, cell.x + b - ox, iy - oy, kTile - b, d + static_cast<std::size_t>(b) * bpp, bpp, got_out);
        }
        if (in_grid) {
            // The grid tile: its part beyond the image means nothing -- the
            // fill there, so an edge tile can still be uniform or absent.
            std::vector<std::uint8_t> g = buf;
            if (live.w < kTile || live.h < kTile)
                for (int y = 0; y < kTile; ++y)
                    for (int x = 0; x < kTile; ++x)
                        if (y < live.y - cell.y || y >= live.y - cell.y + live.h || x < live.x - cell.x || x >= live.x - cell.x + live.w)
                            std::memcpy(g.data() + (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * bpp, fb.data(), bpp);
            auto t = zero_is_none(pixel::Tile::from_bytes(f, std::move(g)));
            grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(shape.cols()) + static_cast<std::size_t>(col)] = t;
        }
        if (keep_outside && got_out) {
            // The outside tile: the part inside the image means nothing --
            // zero it, so a tile with nothing outside collapses to none.
            if (!live.empty())
                for (int y = live.y - cell.y; y < live.y - cell.y + live.h; ++y)
                    std::memset(buf.data() + (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(live.x - cell.x)) * bpp, 0,
                                static_cast<std::size_t>(live.w) * bpp);
            if (auto t = zero_is_none(pixel::Tile::from_bytes(f, std::move(buf)))) outside[{col, row}] = t;
            buf = {};
        }
    }
    return pixel::TileMap::from_tiles(f, w, h, std::move(grid)).with_outside(std::move(outside));
}

std::optional<pixel::TileMap> resampled(const pixel::TileMap& src, int w, int h, Filter flt,
                                        const std::function<bool()>& cancelled,
                                        const std::function<void(double)>& progress) {
    const auto& f = src.format();
    if (w == src.width() && h == src.height()) return src;
    const pixel::TileMap shape(f, w, h);
    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(shape.cols()) * static_cast<std::size_t>(shape.rows()));
    if (src.tile_count() == 0 || w <= 0 || h <= 0) return pixel::TileMap::from_tiles(f, w, h, std::move(grid));
    const auto ch = static_cast<std::size_t>(f.channels());
    const bool alpha = f.alpha, fl = f.depth == pixel::Depth::F32;
    const auto hx = make_taps(src.width(), w, flt), vy = make_taps(src.height(), h, flt);
    RowReader reader(src);
    std::vector<float> srow, hbuf, tile;
    for (int band = 0; band < shape.rows(); ++band) {
        if (cancelled && cancelled()) return std::nullopt;
        if (progress) progress(static_cast<double>(band) / shape.rows());
        const int y0 = band * kTile, y1 = std::min(h, y0 + kTile);
        int ymin = std::numeric_limits<int>::max(), ymax = -1;
        for (int y = y0; y < y1; ++y) {
            const auto& t = vy[static_cast<std::size_t>(y)];
            ymin = std::min(ymin, t.start);
            ymax = std::max(ymax, t.start + static_cast<int>(t.w.size()) - 1);
        }
        reader.drop_below(ymin);
        // Horizontal pass: the source rows this band reads, at the new width.
        const auto rows = static_cast<std::size_t>(ymax - ymin + 1);
        const std::size_t stride = static_cast<std::size_t>(w) * ch;
        hbuf.assign(rows * stride, 0.f);
        for (int sy = ymin; sy <= ymax; ++sy) {
            if (cancelled && cancelled()) return std::nullopt;
            reader.row(sy, srow);
            float* out = hbuf.data() + static_cast<std::size_t>(sy - ymin) * stride;
            for (int x = 0; x < w; ++x) {
                const auto& t = hx[static_cast<std::size_t>(x)];
                float acc[pixel::kMaxChannels] = {};
                for (std::size_t k = 0; k < t.w.size(); ++k) {
                    const float* p = srow.data() + (static_cast<std::size_t>(t.start) + k) * ch;
                    for (std::size_t c = 0; c < ch; ++c) acc[c] += p[c] * t.w[k];
                }
                std::memcpy(out + static_cast<std::size_t>(x) * ch, acc, ch * sizeof(float));
            }
        }
        // Vertical pass, a tile at a time.
        for (int col = 0; col < shape.cols(); ++col) {
            const int x0 = col * kTile, x1 = std::min(w, x0 + kTile);
            tile.assign(kPx * ch, 0.f);
            bool any = false;
            for (int y = y0; y < y1; ++y) {
                const auto& t = vy[static_cast<std::size_t>(y)];
                for (int x = x0; x < x1; ++x) {
                    float acc[pixel::kMaxChannels] = {};
                    for (std::size_t k = 0; k < t.w.size(); ++k) {
                        const float* p = hbuf.data() + (static_cast<std::size_t>(t.start - ymin) + k) * stride + static_cast<std::size_t>(x) * ch;
                        for (std::size_t c = 0; c < ch; ++c) acc[c] += p[c] * t.w[k];
                    }
                    // The bicubic's lobes overshoot: keep it a real pixel.
                    if (alpha) {
                        const float a = std::clamp(acc[ch - 1], 0.f, 1.f);
                        acc[ch - 1] = a;
                        for (std::size_t c = 0; c + 1 < ch; ++c) acc[c] = fl ? std::max(0.f, acc[c]) : std::clamp(acc[c], 0.f, a);
                    } else {
                        for (std::size_t c = 0; c < ch; ++c) acc[c] = fl ? std::max(0.f, acc[c]) : std::clamp(acc[c], 0.f, 1.f);
                    }
                    float* o = tile.data() + (static_cast<std::size_t>(y - y0) * kTile + static_cast<std::size_t>(x - x0)) * ch;
                    std::memcpy(o, acc, ch * sizeof(float));
                    if (!any) for (std::size_t c = 0; c < ch; ++c) if (acc[c] != 0.f) { any = true; break; }
                }
            }
            if (!any && alpha) continue;   // all transparent: no tile
            pixel::TileWriter wr(f, pixel::transparent());
            wr.set_premultiplied(tile);
            auto t = wr.freeze();
            if (t && !t->is_uniform())   // one colour -> uniform; all clear -> none
                t = pixel::Tile::from_bytes(f, std::vector<std::uint8_t>(t->data(), t->data() + t->bytes()));
            grid[static_cast<std::size_t>(band) * static_cast<std::size_t>(shape.cols()) + static_cast<std::size_t>(col)] = t;
        }
    }
    if (progress) progress(1);
    return pixel::TileMap::from_tiles(f, w, h, std::move(grid));
}

std::optional<pixel::TileMap> resampled_layer(const pixel::TileMap& src, int w, int h, Filter f,
                                              const std::function<bool()>& cancelled,
                                              const std::function<void(double)>& progress) {
    if (!src.has_outside()) return resampled(src, w, h, f, cancelled, progress);
    // The whole layer -- the image and every outside cell -- as one plain
    // map, resampled by the same scale, then put back with what lies beyond
    // the new image kept outside it.
    int x0 = 0, y0 = 0, x1 = src.width(), y1 = src.height();
    for (const auto& [cell, t] : src.outside_tiles()) {
        x0 = std::min(x0, cell.first * kTile); y0 = std::min(y0, cell.second * kTile);
        x1 = std::max(x1, (cell.first + 1) * kTile); y1 = std::max(y1, (cell.second + 1) * kTile);
    }
    const double sx = static_cast<double>(w) / src.width(), sy = static_cast<double>(h) / src.height();
    const auto plain = reframed(src, x1 - x0, y1 - y0, -x0, -y0, pixel::transparent(), false);
    const int pw = std::max(1, static_cast<int>(std::lround((x1 - x0) * sx)));
    const int ph = std::max(1, static_cast<int>(std::lround((y1 - y0) * sy)));
    auto q = resampled(plain, pw, ph, f, cancelled, progress);
    if (!q) return std::nullopt;
    return reframed(*q, w, h, static_cast<int>(std::lround(x0 * sx)), static_cast<int>(std::lround(y0 * sy)),
                    pixel::transparent(), true);
}

pixel::Rect content_bounds(const pixel::TileMap& m) {
    const auto& f = m.format();
    const int w = m.width(), h = m.height();
    const pixel::Rect image{0, 0, w, h};
    int x0 = std::numeric_limits<int>::max(), y0 = x0, x1 = std::numeric_limits<int>::min(), y1 = x1;
    const auto take = [&](int x, int y) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); };
    const auto ai = static_cast<std::size_t>(f.channels() - 1);
    const auto shows = [&](const pixel::Px& p) { return !f.alpha || p.c[ai] > 0.f; };
    // One tile: the part of its cell that counts (inside the image for a grid
    // tile, outside it for an outside tile).
    const auto scan = [&](const pixel::TileMap::TileRef& t, int col, int row, bool inside) {
        const pixel::Rect cell{col * kTile, row * kTile, kTile, kTile};
        const pixel::Rect live = cell.intersect(image);
        if (t->is_uniform()) {
            if (!shows(t->get(0, 0))) return;
            if (inside) { take(live.x, live.y); take(live.x + live.w - 1, live.y + live.h - 1); return; }
        }
        for (int y = 0; y < kTile; ++y)
            for (int x = 0; x < kTile; ++x) {
                const int ix = cell.x + x, iy = cell.y + y;
                const bool in = ix >= 0 && iy >= 0 && ix < w && iy < h;
                if (in != inside) continue;
                if (shows(t->get(x, y))) take(ix, iy);
            }
    };
    for (int row = 0; row < m.rows(); ++row)
        for (int col = 0; col < m.cols(); ++col)
            if (const auto t = m.tile(col, row)) scan(t, col, row, true);
    for (const auto& [cell, t] : m.outside_tiles()) scan(t, cell.first, cell.second, false);
    if (x1 < x0) return {};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

std::optional<Reshape> reveal_plan(const Before& b) {
    if (!b.image) return std::nullopt;
    const Image& im = *b.image;
    int x0 = 0, y0 = 0, x1 = im.width, y1 = im.height;
    for (const auto& [id, p] : b.pixels) {
        if (!p || !p->has_outside()) continue;
        const auto r = content_bounds(*p);
        if (r.empty()) continue;
        x0 = std::min(x0, r.x); y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w); y1 = std::max(y1, r.y + r.h);
    }
    if (x0 == 0 && y0 == 0 && x1 == im.width && y1 == im.height) return std::nullopt;   // nothing beyond
    if (!size_refusal(x1 - x0, y1 - y0).empty()) return std::nullopt;
    auto out = crop_plan(b, {x0, y0, x1 - x0, y1 - y0}, pixel::transparent(), false);
    if (out && b.mask) {   // unlike a crop, the selection stays where it was on the pixels
        auto m = reframed(*b.mask, x1 - x0, y1 - y0, -x0, -y0, pixel::transparent(), false);
        out->mask = select::empty(m) ? nullptr : std::make_shared<const pixel::TileMap>(std::move(m));
    }
    return out;
}

bool reveal_all(DocJournal& j, Document& d) {
    const auto b = before(d);
    const auto p = reveal_plan(b);
    return p && land(j, d, b, *p, "Reveal All");
}

int anchor_offset(int old_size, int new_size, int anchor) {
    if (anchor <= 0) return 0;
    if (anchor >= 2) return new_size - old_size;
    return (new_size - old_size) / 2;
}

Before before(const Document& d) {
    Before b;
    b.image = d.image_ptr();
    b.mask = d.selection_ptr();
    for (const auto& [id, l] : d.snapshot()) {
        if (l.kind == Kind::Pixel) b.pixels[id] = l.pixels;
        if (l.mask) b.masks[id] = l.mask;   // s013: an adjustment layer's mask too
    }
    for (const auto& [id, l] : d.snapshot())   // s018 m3: a filter mask goes as a layer mask does
        if (l.filter_mask) b.masks[id + kFilterMaskKey] = l.filter_mask;
    for (const auto& [id, c] : d.channels())   // s016: saved selections go as masks do
        if (c.mask) b.masks[id] = c.mask;
    return b;
}

pixel::Rect bounds(const pixel::TileMap& mask) {
    int x0 = std::numeric_limits<int>::max(), y0 = x0, x1 = -1, y1 = -1;
    for (int row = 0; row < mask.rows(); ++row)
        for (int col = 0; col < mask.cols(); ++col) {
            const auto t = mask.tile(col, row);
            if (!t) continue;
            const pixel::Rect cell = pixel::Rect{col * kTile, row * kTile, kTile, kTile}.intersect({0, 0, mask.width(), mask.height()});
            if (t->is_uniform()) {
                if (t->get(0, 0).c[0] <= 0.f) continue;
                x0 = std::min(x0, cell.x); y0 = std::min(y0, cell.y);
                x1 = std::max(x1, cell.x + cell.w - 1); y1 = std::max(y1, cell.y + cell.h - 1);
                continue;
            }
            for (int y = 0; y < cell.h; ++y)
                for (int x = 0; x < cell.w; ++x)
                    if (t->get(x, y).c[0] > 0.f) {
                        x0 = std::min(x0, cell.x + x); y0 = std::min(y0, cell.y + y);
                        x1 = std::max(x1, cell.x + x); y1 = std::max(y1, cell.y + y);
                    }
        }
    if (x1 < 0) return {};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

std::string size_refusal(int w, int h) {
    if (w < 1 || h < 1) return "the image must be at least 1 x 1 pixel";
    if (w > formats::kMaxSide || h > formats::kMaxSide)
        return "the largest side is " + std::to_string(formats::kMaxSide) + " pixels";
    return {};
}

std::optional<Reshape> crop_plan(const Before& b, const pixel::Rect& r, const pixel::Px& background, bool delete_cropped) {
    if (!b.image || !size_refusal(r.w, r.h).empty()) return std::nullopt;
    const Image& im = *b.image;
    if (r == pixel::Rect{0, 0, im.width, im.height}) return std::nullopt;   // the whole canvas: nothing to do
    Image next = im;
    next.width = r.w;
    next.height = r.h;
    next.origin_x = im.origin_x - r.x;   // s031: the zero point stays on the picture
    next.origin_y = im.origin_y - r.y;
    for (auto& g : next.guides) g.at -= g.vertical ? r.x : r.y;   // s032: and the guides

    const pixel::Px fill = im.format.alpha ? pixel::transparent() : background;
    Reshape out;
    out.image = std::make_shared<const Image>(next);
    for (const auto& [id, p] : b.pixels)
        out.pixels[id] = p ? held(reframed(*p, r.w, r.h, -r.x, -r.y, fill, !delete_cropped)) : nullptr;
    for (const auto& [id, m] : b.masks)   // s012: a mask goes with its layer (beyond it: shows)
        out.masks[id] = std::make_shared<const pixel::TileMap>(reframed(*m, r.w, r.h, -r.x, -r.y, pixel::transparent(), !delete_cropped));
    out.mask = nullptr;   // a crop deselects (Photoshop)
    out.path_map = [dx = r.x, dy = r.y](path::P q) { return path::P{q.x - dx, q.y - dy}; };   // s035: the paths move with the picture
    return out;
}

std::optional<Reshape> canvas_plan(const Before& b, int w, int h, int anchor_x, int anchor_y, const pixel::Px& background) {
    if (!b.image || !size_refusal(w, h).empty()) return std::nullopt;
    const Image& im = *b.image;
    if (w == im.width && h == im.height) return std::nullopt;
    const int ox = anchor_offset(im.width, w, anchor_x), oy = anchor_offset(im.height, h, anchor_y);
    Image next = im;
    next.width = w;
    next.height = h;
    next.origin_x = im.origin_x + ox;   // s031
    next.origin_y = im.origin_y + oy;
    for (auto& g : next.guides) g.at += g.vertical ? ox : oy;   // s032

    const pixel::Px fill = im.format.alpha ? pixel::transparent() : background;
    Reshape out;
    out.image = std::make_shared<const Image>(next);
    for (const auto& [id, p] : b.pixels)
        out.pixels[id] = p ? held(reframed(*p, w, h, ox, oy, fill)) : nullptr;
    for (const auto& [id, m] : b.masks)
        out.masks[id] = std::make_shared<const pixel::TileMap>(reframed(*m, w, h, ox, oy, pixel::transparent()));
    out.path_map = [ox, oy](path::P q) { return path::P{q.x + ox, q.y + oy}; };   // s035
    if (b.mask) {
        auto m = reframed(*b.mask, w, h, ox, oy, pixel::transparent(), false);
        out.mask = select::empty(m) ? nullptr : std::make_shared<const pixel::TileMap>(std::move(m));
    }
    return out;
}

std::optional<Reshape> size_plan(const Before& b, int w, int h, double dpi, Filter f,
                                 const std::function<bool()>& cancelled,
                                 const std::function<void(double)>& progress) {
    if (!b.image || !size_refusal(w, h).empty() || !(dpi > 0)) return std::nullopt;
    const Image& im = *b.image;
    if (w == im.width && h == im.height && dpi == im.dpi) return std::nullopt;
    Image next = im;
    next.width = w;
    next.height = h;
    next.dpi = dpi;
    next.origin_x = im.origin_x * w / std::max(1, im.width);   // s031: scaled with the picture
    next.origin_y = im.origin_y * h / std::max(1, im.height);
    for (auto& g : next.guides) g.at = g.at * (g.vertical ? w : h) / std::max(1, g.vertical ? im.width : im.height);   // s032

    if (w == im.width && h == im.height) {   // the resolution alone: no pixel changes
        Reshape out = same_layers(b);
        out.image = std::make_shared<const Image>(next);
        return out;   // the resolution alone: the paths stay
    }
    Reshape out;
    out.image = std::make_shared<const Image>(next);
    {   // s035: the paths scale with the picture
        const double sx = static_cast<double>(w) / std::max(1, im.width), sy = static_cast<double>(h) / std::max(1, im.height);
        out.path_map = [sx, sy](path::P q) { return path::P{q.x * sx, q.y * sy}; };
    }
    const double n = static_cast<double>(b.pixels.size() + (b.mask ? 1 : 0));
    double done = 0;
    const auto part = [&](double base) {
        return [&progress, base, n](double p) { if (progress) progress((base + p) / std::max(1.0, n)); };
    };
    for (const auto& [id, p] : b.pixels) {
        if (p) {
            auto m = resampled_layer(*p, w, h, f, cancelled, part(done));
            if (!m) return std::nullopt;
            out.pixels[id] = held(std::move(*m));
        } else {
            out.pixels[id] = nullptr;
        }
        done += 1;
    }
    for (const auto& [id, lm] : b.masks) {   // s012: layer masks scale with their layers
        auto m = resampled_layer(*lm, w, h, f, cancelled);
        if (!m) return std::nullopt;
        out.masks[id] = std::make_shared<const pixel::TileMap>(std::move(*m));
    }
    if (b.mask) {
        auto m = resampled(*b.mask, w, h, f, cancelled, part(done));
        if (!m) return std::nullopt;
        out.mask = select::empty(*m) ? nullptr : std::make_shared<const pixel::TileMap>(std::move(*m));
    }
    return out;
}

bool land(DocJournal& j, Document& d, const Before& expect, const Reshape& r, const std::string& label) {
    if (!r.image) return false;
    const Before now = before(d);
    if (now.image != expect.image || now.mask != expect.mask || now.pixels != expect.pixels || now.masks != expect.masks)
        return false;
    std::vector<Id> ids{kImageId};
    for (const auto& [id, p] : r.pixels) ids.push_back(id);
    for (const auto& [id, m] : r.masks)   // s013: a mask on a layer with no pixels
        if (filter_mask_key(id)) {   // s018 m3: the layer the filter mask belongs to
            const Id base = id.substr(0, id.size() - std::char_traits<char>::length(kFilterMaskKey));
            if (!r.pixels.count(base) && std::find(ids.begin(), ids.end(), base) == ids.end()) ids.push_back(base);
        } else if (!r.pixels.count(id)) ids.push_back(id);
    ids.push_back(kSelectionId);
    std::vector<Id> path_ids;   // s035
    if (r.path_map)
        for (const auto& [id, l] : d.snapshot())
            if (l.kind == Kind::Path && (l.path || l.hints)) { path_ids.push_back(id); ids.push_back(id); }
    return j.run(label, ids, [&] {
        for (const auto& id : path_ids) {   // s035: the outlines go with the picture
            auto l = d.get(id);
            if (!l) continue;
            if (l->path) l->path = std::make_shared<const path::Path>(path::mapped(*l->path, r.path_map));
            if (l->hints) l->hints = std::make_shared<const path::Hints>(path::mapped(*l->hints, r.path_map));   // s037: the strokes too
            d.put(id, *l);
        }
        Layer im;
        im.name = "Image";
        im.image = r.image;
        d.put(kImageId, im);
        for (const auto& [id, p] : r.pixels) {
            auto l = d.get(id);
            if (!l) continue;
            l->pixels = p;
            if (const auto m = r.masks.find(id); m != r.masks.end()) l->mask = m->second;   // s012
            d.put(id, *l);
        }
        for (const auto& [key, m] : r.masks) {   // s018 m3: filter masks, after the layer's own write
            if (!filter_mask_key(key)) continue;
            const Id id = key.substr(0, key.size() - std::char_traits<char>::length(kFilterMaskKey));
            auto l = d.get(id);
            if (!l) continue;
            l->filter_mask = m;
            d.put(id, *l);
        }
        for (const auto& [id, m] : r.masks) {   // s013: adjustment layers' masks
            if (r.pixels.count(id) || filter_mask_key(id)) continue;
            auto l = d.get(id);
            if (!l) continue;
            l->mask = m;
            d.put(id, *l);
        }
        if (r.mask) {
            Layer s;
            s.name = "Selection";
            s.pixels = r.mask;
            d.put(kSelectionId, s);
        } else if (d.get(kSelectionId)) {
            d.put(kSelectionId, std::nullopt);
        }
    });
}

bool crop(DocJournal& j, Document& d, const pixel::Rect& r, const pixel::Px& background, bool delete_cropped) {
    const auto b = before(d);
    const auto p = crop_plan(b, r, background, delete_cropped);
    return p && land(j, d, b, *p, "Crop");
}

bool canvas_size(DocJournal& j, Document& d, int w, int h, int anchor_x, int anchor_y, const pixel::Px& background) {
    const auto b = before(d);
    const auto p = canvas_plan(b, w, h, anchor_x, anchor_y, background);
    return p && land(j, d, b, *p, "Canvas Size");
}

bool image_size(DocJournal& j, Document& d, int w, int h, double dpi, Filter f) {
    const auto b = before(d);
    const auto p = size_plan(b, w, h, dpi, f);
    return p && land(j, d, b, *p, "Image Size");
}

}  // namespace klimt::core::canvas
