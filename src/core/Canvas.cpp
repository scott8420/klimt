#include "core/Canvas.hpp"
#include "core/Selection.hpp"
#include "formats/Formats.hpp"   // kMaxSide

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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
            t.w = {1.f};
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
    return r;
}
}  // namespace

const char* filter_name(Filter f) { return f == Filter::Nearest ? "Nearest" : "Smooth (bicubic)"; }

pixel::TileMap reframed(const pixel::TileMap& src, int w, int h, int ox, int oy, const pixel::Px& fill) {
    const auto& f = src.format();
    const int sw = src.width(), sh = src.height();
    if (w == sw && h == sh && ox == 0 && oy == 0) return src;
    const auto bpp = static_cast<std::size_t>(f.bytes_per_pixel());
    const auto fb = pixel_bytes(f, fill);
    const bool fill_zero = std::all_of(fb.begin(), fb.end(), [](std::uint8_t b) { return b == 0; });
    const pixel::TileMap::TileRef fill_tile = fill_zero ? nullptr : pixel::Tile::uniform(f, fill);
    const pixel::TileMap shape(f, w, h);
    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(shape.cols()) * static_cast<std::size_t>(shape.rows()));
    std::vector<std::uint8_t> buf;
    for (int row = 0; row < shape.rows(); ++row)
        for (int col = 0; col < shape.cols(); ++col) {
            auto& slot = grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(shape.cols()) + static_cast<std::size_t>(col)];
            const pixel::Rect cell = pixel::Rect{col * kTile, row * kTile, kTile, kTile}.intersect({0, 0, w, h});
            // The part of this cell that comes from inside the source, in source coordinates.
            const pixel::Rect from = pixel::Rect{cell.x - ox, cell.y - oy, cell.w, cell.h}.intersect({0, 0, sw, sh});
            if (from.empty()) { slot = fill_tile; continue; }
            buf.assign(kPx * bpp, 0);
            if (!fill_zero)
                for (std::size_t i = 0; i < kPx; ++i) std::memcpy(buf.data() + i * bpp, fb.data(), bpp);
            for (int sy = from.y; sy < from.y + from.h; ++sy) {
                const int ty = sy + oy - cell.y;   // row in the destination tile
                int sx = from.x;
                while (sx < from.x + from.w) {
                    const int scol = sx / kTile, srow = sy / kTile;
                    const int run_end = std::min(from.x + from.w, (scol + 1) * kTile);
                    const int n = run_end - sx;
                    std::uint8_t* dst = buf.data() + (static_cast<std::size_t>(ty) * kTile + static_cast<std::size_t>(sx + ox - cell.x)) * bpp;
                    const auto t = src.tile(scol, srow);
                    if (!t) {
                        std::memset(dst, 0, static_cast<std::size_t>(n) * bpp);
                    } else if (t->is_uniform()) {
                        for (int k = 0; k < n; ++k) std::memcpy(dst + static_cast<std::size_t>(k) * bpp, t->data(), bpp);
                    } else {
                        const std::size_t off = (static_cast<std::size_t>(sy - srow * kTile) * kTile + static_cast<std::size_t>(sx - scol * kTile)) * bpp;
                        std::memcpy(dst, t->data() + off, static_cast<std::size_t>(n) * bpp);
                    }
                    sx = run_end;
                }
            }
            slot = pixel::Tile::from_bytes(f, std::move(buf));
        }
    return pixel::TileMap::from_tiles(f, w, h, std::move(grid));
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

int anchor_offset(int old_size, int new_size, int anchor) {
    if (anchor <= 0) return 0;
    if (anchor >= 2) return new_size - old_size;
    return (new_size - old_size) / 2;
}

Before before(const Document& d) {
    Before b;
    b.image = d.image_ptr();
    b.mask = d.selection_ptr();
    for (const auto& [id, l] : d.snapshot())
        if (l.kind == Kind::Pixel) b.pixels[id] = l.pixels;
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

std::optional<Reshape> crop_plan(const Before& b, const pixel::Rect& r, const pixel::Px& background) {
    if (!b.image || !size_refusal(r.w, r.h).empty()) return std::nullopt;
    const Image& im = *b.image;
    if (r == pixel::Rect{0, 0, im.width, im.height}) return std::nullopt;   // the whole canvas: nothing to do
    Image next = im;
    next.width = r.w;
    next.height = r.h;
    const pixel::Px fill = im.format.alpha ? pixel::transparent() : background;
    Reshape out;
    out.image = std::make_shared<const Image>(next);
    for (const auto& [id, p] : b.pixels)
        out.pixels[id] = p ? held(reframed(*p, r.w, r.h, -r.x, -r.y, fill)) : nullptr;
    out.mask = nullptr;   // a crop deselects (Photoshop)
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
    const pixel::Px fill = im.format.alpha ? pixel::transparent() : background;
    Reshape out;
    out.image = std::make_shared<const Image>(next);
    for (const auto& [id, p] : b.pixels)
        out.pixels[id] = p ? held(reframed(*p, w, h, ox, oy, fill)) : nullptr;
    if (b.mask) {
        auto m = reframed(*b.mask, w, h, ox, oy, pixel::transparent());
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
    if (w == im.width && h == im.height) {   // the resolution alone: no pixel changes
        Reshape out = same_layers(b);
        out.image = std::make_shared<const Image>(next);
        return out;
    }
    Reshape out;
    out.image = std::make_shared<const Image>(next);
    const double n = static_cast<double>(b.pixels.size() + (b.mask ? 1 : 0));
    double done = 0;
    const auto part = [&](double base) {
        return [&progress, base, n](double p) { if (progress) progress((base + p) / std::max(1.0, n)); };
    };
    for (const auto& [id, p] : b.pixels) {
        if (p) {
            auto m = resampled(*p, w, h, f, cancelled, part(done));
            if (!m) return std::nullopt;
            out.pixels[id] = held(std::move(*m));
        } else {
            out.pixels[id] = nullptr;
        }
        done += 1;
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
    if (now.image != expect.image || now.mask != expect.mask || now.pixels != expect.pixels) return false;
    std::vector<Id> ids{kImageId};
    for (const auto& [id, p] : r.pixels) ids.push_back(id);
    ids.push_back(kSelectionId);
    return j.run(label, ids, [&] {
        Layer im;
        im.name = "Image";
        im.image = r.image;
        d.put(kImageId, im);
        for (const auto& [id, p] : r.pixels) {
            auto l = d.get(id);
            if (!l) continue;
            l->pixels = p;
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

bool crop(DocJournal& j, Document& d, const pixel::Rect& r, const pixel::Px& background) {
    const auto b = before(d);
    const auto p = crop_plan(b, r, background);
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
