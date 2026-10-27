#include "core/Composite.hpp"

#include <algorithm>
#include <cmath>

namespace klimt::core {

namespace {
using pixel::kTile;
constexpr std::size_t kPx = static_cast<std::size_t>(kTile) * kTile;

// Klimt's normalised Lab (L 0..1 = 0..100; a, b 0..1 = -128..127), D50, to
// encoded sRGB (Bradford-adapted). Out of gamut clamps.
void lab_to_srgb(float l, float a, float b, float* out) {
    const double L = l * 100.0, A = a * 255.0 - 128.0, B = b * 255.0 - 128.0;
    const double fy = (L + 16) / 116, fx = fy + A / 500, fz = fy - B / 200;
    const auto inv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
    const double X = 0.9642 * inv(fx), Y = inv(fy), Z = 0.8249 * inv(fz);
    const double lin[3] = {3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z,
                           -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z,
                           0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z};
    for (int c = 0; c < 3; ++c) {
        const double v = std::clamp(lin[c], 0.0, 1.0);
        out[c] = static_cast<float>(v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055);
    }
}

// sRGB (encoded) -> Klimt's normalised Lab (D50, Bradford), the inverse of
// lab_to_srgb.
void srgb_to_lab(float r, float g, float b, float* out) {
    const auto lin = [](double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    const double R = lin(r), G = lin(g), B = lin(b);
    const double X = 0.4360747 * R + 0.3850649 * G + 0.1430804 * B;
    const double Y = 0.2225045 * R + 0.7168786 * G + 0.0606169 * B;
    const double Z = 0.0139322 * R + 0.0971045 * G + 0.7141733 * B;
    const auto f = [](double t) { return t > 216.0 / 24389 ? std::cbrt(t) : (24389.0 / 27 * t + 16) / 116; };
    const double fx = f(X / 0.9642), fy = f(Y), fz = f(Z / 0.8249);
    out[0] = static_cast<float>((116 * fy - 16) / 100);
    out[1] = static_cast<float>((500 * (fx - fy) + 128) / 255);
    out[2] = static_cast<float>((200 * (fy - fz) + 128) / 255);
}

struct Walker {
    const Image&  im;
    const Layers& layers;
    int col, row;
    std::map<Document::Id, std::vector<Document::Id>> kids;   // parent -> bottom first

    Walker(const Image& i, const Layers& l, int c, int r) : im(i), layers(l), col(c), row(r) {
        for (const auto& [id, layer] : layers) kids[layer.parent].push_back(id);
        for (auto& [p, v] : kids)
            std::stable_sort(v.begin(), v.end(), [&](const auto& a, const auto& b) {
                return layers.at(a).pos < layers.at(b).pos;
            });
    }

    // The layer's own pixels in this tile, premultiplied RGBA; false if none.
    // s007: Gray, CMYK and Lab layers are shown as RGB through a plain,
    // UNMANAGED conversion (gray -> g,g,g; CMYK -> (1-c)(1-k) ...; Lab D50 ->
    // sRGB) until libs/tint brings profiles. Honest enough to see an opened
    // file; not proofing.
    bool pixels_of(const Layer& l, std::vector<float>& out) const {
        if (!l.pixels) return false;
        const auto t = l.pixels->tile(col, row);
        if (!t) return false;
        std::vector<float> raw;
        t->premultiplied(raw);
        const pixel::Format f = l.pixels->format();
        const int n = f.channels();
        out.assign(kPx * 4, 0.f);
        for (std::size_t i = 0; i < kPx; ++i) {
            const float* s = raw.data() + i * static_cast<std::size_t>(n);
            float* d = out.data() + i * 4;
            const float a = f.alpha ? s[n - 1] : 1.f;
            d[3] = a;
            switch (f.mode) {
                case pixel::Mode::RGB:  d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; break;
                case pixel::Mode::Gray: d[0] = d[1] = d[2] = s[0]; break;
                case pixel::Mode::CMYK: {
                    if (a <= 0.f) break;
                    const float k = 1.f - std::clamp(s[3] / a, 0.f, 1.f);
                    for (int c = 0; c < 3; ++c) d[c] = (1.f - std::clamp(s[c] / a, 0.f, 1.f)) * k * a;
                    break;
                }
                case pixel::Mode::Lab: {
                    if (a <= 0.f) break;
                    float rgb[3];
                    lab_to_srgb(s[0] / a, s[1] / a, s[2] / a, rgb);
                    for (int c = 0; c < 3; ++c) d[c] = rgb[c] * a;
                    break;
                }
            }
        }
        return true;
    }

    void blend_into(pixel::Blend mode, const std::vector<float>& src, std::vector<float>& dst, float opacity) const {
        if (mode == pixel::Blend::PassThrough) mode = pixel::Blend::Normal;
        for (int y = 0; y < kTile; ++y)
            pixel::blend_span(mode, src.data() + static_cast<std::size_t>(y) * kTile * 4,
                              dst.data() + static_cast<std::size_t>(y) * kTile * 4, kTile, opacity,
                              col * kTile, row * kTile + y);
    }

    // The base a clipped layer clips to: the nearest unclipped sibling below.
    const Layer* clip_base(const Document::Id& id) const {
        const auto& sibs = kids.at(layers.at(id).parent);
        auto it = std::find(sibs.begin(), sibs.end(), id);
        while (it != sibs.begin()) {
            --it;
            if (!layers.at(*it).clipped) return &layers.at(*it);
        }
        return nullptr;
    }

    void group(const Document::Id& parent, std::vector<float>& dst) const {
        const auto it = kids.find(parent);
        if (it == kids.end()) return;
        for (const auto& id : it->second) {
            const Layer& l = layers.at(id);
            if (!l.visible) continue;
            const float op = static_cast<float>(l.opacity) / 100.f;
            if (l.kind == Kind::Group) {
                if (l.blend == pixel::Blend::PassThrough) {
                    if (l.opacity >= 100) { group(id, dst); continue; }
                    std::vector<float> tmp = dst;
                    group(id, tmp);
                    for (std::size_t i = 0; i < dst.size(); ++i) dst[i] += (tmp[i] - dst[i]) * op;
                } else {
                    std::vector<float> tmp(kPx * 4, 0.f);
                    group(id, tmp);
                    blend_into(l.blend, tmp, dst, op);
                }
                continue;
            }
            std::vector<float> src;
            if (!pixels_of(l, src)) continue;
            if (l.clipped) {
                if (const Layer* base = clip_base(id)) {
                    if (!base->visible) continue;
                    std::vector<float> mask;
                    if (!pixels_of(*base, mask)) continue;   // nothing to show through
                    for (std::size_t i = 0; i < kPx; ++i)
                        for (int c = 0; c < 4; ++c) src[i * 4 + static_cast<std::size_t>(c)] *= mask[i * 4 + 3];
                }
            }
            blend_into(l.blend, src, dst, op * static_cast<float>(l.fill) / 100.f);
        }
    }
};

std::uint32_t argb(const float* p) {
    const auto q = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    return (q(p[3]) << 24) | (q(p[0]) << 16) | (q(p[1]) << 8) | q(p[2]);
}
}  // namespace

void composite_tile(const Image& im, const Layers& layers, int col, int row, std::vector<float>& out) {
    out.assign(kPx * 4, 0.f);
    Walker w(im, layers, col, row);
    w.group({}, out);
}

Sampled sample_composite(const Image& im, const Layers& layers, int x, int y, int radius) {
    double r = 0, g = 0, b = 0, a = 0;
    int n = 0;
    std::vector<float> tile;
    int tc = -1, tr = -1;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const int sx = x + dx, sy = y + dy;
            if (sx < 0 || sy < 0 || sx >= im.width || sy >= im.height) continue;
            if (sx / kTile != tc || sy / kTile != tr) { tc = sx / kTile; tr = sy / kTile; composite_tile(im, layers, tc, tr, tile); }
            const float* p = tile.data() + (static_cast<std::size_t>(sy % kTile) * kTile + static_cast<std::size_t>(sx % kTile)) * 4;
            r += p[0]; g += p[1]; b += p[2]; a += p[3];
            ++n;
        }
    if (n == 0 || a <= 0) return {};
    return {static_cast<float>(r / a), static_cast<float>(g / a), static_cast<float>(b / a), static_cast<float>(a / n)};
}

pixel::Px from_srgb(const pixel::Format& f, float r, float g, float b) {
    pixel::Px p;
    switch (f.mode) {
        case pixel::Mode::RGB:  p.c = {r, g, b, 0, 0}; break;
        case pixel::Mode::Gray: p.c[0] = 0.2126f * r + 0.7152f * g + 0.0722f * b; break;
        case pixel::Mode::CMYK: {
            const float k = 1.f - std::max({r, g, b});
            const float d = 1.f - k;
            p.c = {d > 0 ? (d - r) / d : 0.f, d > 0 ? (d - g) / d : 0.f, d > 0 ? (d - b) / d : 0.f, k, 0};
            break;
        }
        case pixel::Mode::Lab: { float lab[3]; srgb_to_lab(r, g, b, lab); p.c = {lab[0], lab[1], lab[2], 0, 0}; break; }
    }
    if (f.alpha) p.c[static_cast<std::size_t>(f.channels() - 1)] = 1.f;
    return p;
}

void composite_tile_argb(const Image& im, const Layers& layers, int col, int row,
                         std::vector<std::uint32_t>& out, int& w, int& h) {
    std::vector<float> tile;
    composite_tile(im, layers, col, row, tile);
    w = std::clamp(im.width - col * kTile, 0, kTile);
    h = std::clamp(im.height - row * kTile, 0, kTile);
    out.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] =
                argb(tile.data() + (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * 4);
}

// ── dirty tiles ─────────────────────────────────────────────────────────────
std::size_t TileSet::count() const {
    if (all) return static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);
    return static_cast<std::size_t>(std::count(on.begin(), on.end(), 1));
}

void TileSet::add(int col, int row) {
    if (col < 0 || row < 0 || col >= cols || row >= rows) return;
    on[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(col)] = 1;
}

void TileSet::merge(const TileSet& o) {
    if (o.cols != cols || o.rows != rows || o.all) { all = true; return; }
    for (std::size_t i = 0; i < on.size(); ++i) on[i] = static_cast<char>(on[i] | o.on[i]);
}

TileSet tiles_of(const Image& im) {
    TileSet t;
    t.cols = (im.width + kTile - 1) / kTile;
    t.rows = (im.height + kTile - 1) / kTile;
    t.on.assign(static_cast<std::size_t>(t.cols) * static_cast<std::size_t>(t.rows), 0);
    return t;
}

namespace {
// Every tile a layer's map holds (its footprint).
void add_held(TileSet& t, const Layer* l) {
    if (!l || !l->pixels) return;
    for (int row = 0; row < l->pixels->rows(); ++row)
        for (int col = 0; col < l->pixels->cols(); ++col)
            if (l->pixels->tile(col, row)) t.add(col, row);
}

const Layer* find(const Layers& ls, const Document::Id& id) {
    const auto it = ls.find(id);
    return it == ls.end() ? nullptr : &it->second;
}

// id's contents (all depths) and its clipped siblings, in one snapshot.
void add_related(TileSet& t, const Layers& ls, const Document::Id& id) {
    const Layer* self = find(ls, id);
    if (!self) return;
    for (const auto& [oid, l] : ls) {
        if (oid == id) continue;
        if (l.parent == self->parent && l.clipped) { add_held(t, &l); continue; }
        // A descendant: walk up from l.
        for (std::string p = l.parent; !p.empty();) {
            if (p == id) { add_held(t, &l); break; }
            const Layer* up = find(ls, p);
            if (!up) break;
            p = up->parent;
        }
    }
}
}  // namespace

TileSet changed_tiles(const Image& before_im, const Layers& before, const Image& after_im, const Layers& after) {
    TileSet t = tiles_of(after_im);
    if (before_im.width != after_im.width || before_im.height != after_im.height ||
        !(before_im.format == after_im.format)) {
        t.add_all();
        return t;
    }
    std::vector<Document::Id> ids;
    for (const auto& [id, l] : before) ids.push_back(id);
    for (const auto& [id, l] : after) if (!before.count(id)) ids.push_back(id);
    for (const auto& id : ids) {
        const Layer* a = find(before, id);
        const Layer* b = find(after, id);
        if (a && b && *a == *b) continue;
        if (a && b) {
            Layer x = *a, y = *b;
            x.name = y.name;        // the eye can't see a name
            x.locks = y.locks;      // or a lock
            x.pixels = y.pixels;
            if (x == y) {
                if (a->pixels == b->pixels) continue;   // a rename / lock only
                // Pixels alone: the tiles whose pointers differ.
                if (a->pixels && b->pixels) {
                    for (int row = 0; row < t.rows; ++row)
                        for (int col = 0; col < t.cols; ++col)
                            if (a->pixels->tile(col, row) != b->pixels->tile(col, row)) t.add(col, row);
                } else {
                    add_held(t, a);
                    add_held(t, b);
                }
                // A base whose pixels changed changes its clipped layers --
                // but only inside its own tiles, which are marked already.
                continue;
            }
        }
        add_held(t, a);
        add_held(t, b);
        add_related(t, before, id);
        add_related(t, after, id);
    }
    return t;
}

bool composite_argb(const Image& im, const Layers& layers, std::vector<std::uint32_t>& out,
                    const std::function<bool()>& cancelled, const std::function<void(double)>& progress) {
    out.assign(static_cast<std::size_t>(im.width) * static_cast<std::size_t>(im.height), 0u);
    const int cols = (im.width + kTile - 1) / kTile, rows = (im.height + kTile - 1) / kTile;
    std::vector<float> tile;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            if (cancelled && cancelled()) return false;
            composite_tile(im, layers, c, r, tile);
            for (int y = 0; y < kTile; ++y) {
                const int iy = r * kTile + y;
                if (iy >= im.height) break;
                for (int x = 0; x < kTile; ++x) {
                    const int ix = c * kTile + x;
                    if (ix >= im.width) break;
                    out[static_cast<std::size_t>(iy) * static_cast<std::size_t>(im.width) + static_cast<std::size_t>(ix)] =
                        argb(tile.data() + (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * 4);
                }
            }
            if (progress) progress(static_cast<double>(r * cols + c + 1) / (rows * cols));
        }
    return true;
}

}  // namespace klimt::core
