#include "core/Composite.hpp"
#include "core/Adjust.hpp"
#include "core/Live.hpp"   // s018

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
    // s010 m3: a scratch buffer per thread for the tile's own channels.
    bool pixels_of(const Layer& l, std::vector<float>& out) const {
        if (!l.pixels) return false;
        const auto t = l.pixels->tile(col, row);
        if (!t) return false;
        const pixel::Format f = l.pixels->format();
        const int n = f.channels();
        // A uniform tile of nothing shows nothing.
        if (t->is_uniform() && f.alpha && t->get(0, 0).c[static_cast<std::size_t>(n - 1)] <= 0.f) return false;
        // s012: the mask's hide amount for this tile (nullptr: hides nothing).
        pixel::TileMap::TileRef mt;
        if (l.mask && l.mask_on) {
            mt = l.mask->tile(col, row);
            if (mt && mt->is_uniform() && mt->get(0, 0).c[0] >= 1.f) return false;   // wholly hidden
        }
        const auto masked = [&]() {
            if (!mt) return;
            thread_local std::vector<float> hide;
            mt->premultiplied(hide);   // one channel: the hide amount
            for (std::size_t i = 0; i < kPx; ++i) {
                const float k = 1.f - std::clamp(hide[i], 0.f, 1.f);
                float* d = out.data() + i * 4;
                d[0] *= k; d[1] *= k; d[2] *= k; d[3] *= k;
            }
        };
        if (f.mode == pixel::Mode::RGB && f.alpha) {   // already the compositor's layout
            t->premultiplied(out);
            masked();
            return true;
        }
        thread_local std::vector<float> raw;
        t->premultiplied(raw);
        out.resize(kPx * 4);
        for (std::size_t i = 0; i < kPx; ++i) {
            const float* s = raw.data() + i * static_cast<std::size_t>(n);
            float* d = out.data() + i * 4;
            const float a = f.alpha ? s[n - 1] : 1.f;
            d[0] = d[1] = d[2] = 0.f;
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
        masked();
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

    // s013: an ADJUSTMENT LAYER changes what is already in dst (what's below
    // it here), seen through its mask and clip, at strength k (opacity x
    // fill), in its blend mode (the adjusted colour blended onto the
    // original). Alpha never changes.
    void adjust_into(const Document::Id& id, const Layer& l, std::vector<float>& dst, float k) const {
        if (k <= 0.f || adjust::identity(l.adjust)) return;
        pixel::TileMap::TileRef mt;
        if (l.mask && l.mask_on) {
            mt = l.mask->tile(col, row);
            if (mt && mt->is_uniform() && mt->get(0, 0).c[0] >= 1.f) return;   // wholly hidden here
        }
        thread_local std::vector<float> hide, clip, adj, mixed;
        const float* hp = nullptr;
        if (mt) { mt->premultiplied(hide); hp = hide.data(); }
        const float* cp = nullptr;
        if (l.clipped) {
            if (const Layer* base = clip_base(id)) {
                if (!base->visible || !pixels_of(*base, clip)) return;   // nothing to show through
                cp = clip.data();
            }
        }
        adj = dst;
        adjust::run(*adjust::compile(l.adjust), adj.data(), kPx);
        const float* res = adj.data();
        if (l.blend != pixel::Blend::Normal && l.blend != pixel::Blend::PassThrough) {
            mixed = dst;
            blend_into(l.blend, adj, mixed, 1.f);
            res = mixed.data();
        }
        float* d = dst.data();
        for (std::size_t i = 0; i < kPx; ++i, d += 4) {
            float w = k;
            if (hp) w *= 1.f - std::clamp(hp[i], 0.f, 1.f);
            if (cp) w *= cp[i * 4 + 3];
            if (w <= 0.f) continue;
            const float* r = res + i * 4;
            d[0] += (r[0] - d[0]) * w;
            d[1] += (r[1] - d[1]) * w;
            d[2] += (r[2] - d[2]) * w;
        }
    }

    void group(const Document::Id& parent, std::vector<float>& dst) const {
        const auto it = kids.find(parent);
        if (it == kids.end()) return;
        for (const auto& id : it->second) {
            const Layer& l = layers.at(id);
            if (!l.visible) continue;
            const float op = static_cast<float>(l.opacity) / 100.f;
            if (l.kind == Kind::Group) {
                // s024: a GROUP MASK -- how much of the group shows, per pixel.
                pixel::TileMap::TileRef mt;
                if (l.mask && l.mask_on) {
                    mt = l.mask->tile(col, row);
                    if (mt && mt->is_uniform() && mt->get(0, 0).c[0] >= 1.f) continue;   // wholly hidden here
                }
                thread_local std::vector<float> ghide;
                if (mt) mt->premultiplied(ghide);
                const auto shows = [&](std::size_t px) { return mt ? 1.f - std::clamp(ghide[px], 0.f, 1.f) : 1.f; };
                if (l.blend == pixel::Blend::PassThrough) {
                    if (l.opacity >= 100 && !mt) { group(id, dst); continue; }
                    // What the group does to what's below, mixed in by opacity x mask.
                    std::vector<float> tmp = dst;
                    group(id, tmp);
                    for (std::size_t i = 0; i < dst.size(); ++i) dst[i] += (tmp[i] - dst[i]) * op * shows(i / 4);
                } else {
                    std::vector<float> tmp(kPx * 4, 0.f);
                    group(id, tmp);
                    if (mt)
                        for (std::size_t px = 0; px < kPx; ++px) {
                            const float k = shows(px);
                            for (int c = 0; c < 4; ++c) tmp[px * 4 + static_cast<std::size_t>(c)] *= k;
                        }
                    blend_into(l.blend, tmp, dst, op);
                }
                continue;
            }
            if (l.kind == Kind::Adjustment) {
                adjust_into(id, l, dst, op * static_cast<float>(l.fill) / 100.f);
                continue;
            }
            if (l.kind == Kind::Filter) continue;   // s030: drawn as it looks (live::effective), else nothing
            if (l.kind == Kind::Path) continue;     // s035: a path layer draws nothing (its mask: live::effective)
            // s010 m3: one opaque colour at full strength, Normal, unclipped:
            // it simply IS what's below from here up.
            if (l.blend == pixel::Blend::Normal && !l.clipped && l.opacity >= 100 && l.fill >= 100 && l.pixels &&
                !(l.mask && l.mask_on && l.mask->tile(col, row)) &&
                l.pixels->format().mode == pixel::Mode::RGB) {
                const auto t = l.pixels->tile(col, row);
                if (t && t->is_uniform()) {
                    const pixel::Px p = t->get(0, 0);
                    const pixel::Format f = l.pixels->format();
                    if (!f.alpha || p.c[3] >= 1.f) {
                        const float v[4] = {p.c[0], p.c[1], p.c[2], 1.f};
                        for (std::size_t i = 0; i < kPx; ++i) std::copy(v, v + 4, dst.data() + i * 4);
                        continue;
                    }
                }
            }
            // Reused, not allocated (a megabyte a layer a tile, before s010).
            thread_local std::vector<float> src_buf;
            std::vector<float>& src = src_buf;
            if (!pixels_of(l, src)) continue;
            if (l.clipped) {
                if (const Layer* base = clip_base(id)) {
                    if (!base->visible) continue;
                    thread_local std::vector<float> mask;
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
    // s018: layers with live filters are drawn as they LOOK (core/Live.hpp).
    if (live::any(layers)) {
        const Layers eff = live::effective(layers, &im);   // s024: styled groups need the image
        Walker w(im, eff, col, row);
        w.group({}, out);
        return;
    }
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

void argb_half(const std::vector<std::uint32_t>& in, int w, int h, std::vector<std::uint32_t>& out, int& ow, int& oh) {
    ow = (w + 1) / 2;
    oh = (h + 1) / 2;
    out.resize(static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh));
    for (int y = 0; y < oh; ++y)
        for (int x = 0; x < ow; ++x) {
            std::uint32_t sum[4] = {0, 0, 0, 0};
            int n = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = 2 * x + dx, sy = 2 * y + dy;
                    if (sx >= w || sy >= h) continue;
                    const std::uint32_t p = in[static_cast<std::size_t>(sy) * static_cast<std::size_t>(w) + static_cast<std::size_t>(sx)];
                    for (int c = 0; c < 4; ++c) sum[c] += (p >> (8 * c)) & 0xffu;
                    ++n;
                }
            std::uint32_t q = 0;
            for (int c = 0; c < 4; ++c) q |= ((sum[c] + static_cast<std::uint32_t>(n) / 2) / static_cast<std::uint32_t>(n)) << (8 * c);
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(ow) + static_cast<std::size_t>(x)] = q;
        }
}

void mask_tile_argb(const pixel::TileMap& mask, int col, int row, std::vector<std::uint32_t>& out, int& w, int& h) {
    w = std::clamp(mask.width() - col * kTile, 0, kTile);
    h = std::clamp(mask.height() - row * kTile, 0, kTile);
    out.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0xffffffffu);
    const auto t = mask.tile(col, row);
    if (!t) return;
    std::vector<float> hide;
    t->premultiplied(hide);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = 1.f - std::clamp(hide[static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)], 0.f, 1.f);
            const auto g = static_cast<std::uint32_t>(v * 255.f + 0.5f);
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = 0xff000000u | (g << 16) | (g << 8) | g;
        }
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
    if (l && (l->kind == Kind::Adjustment || l->kind == Kind::Filter)) { t.add_all(); return; }   // s013, s030: what's below, anywhere
    if (!l || !l->pixels) return;
    for (int row = 0; row < l->pixels->rows(); ++row)
        for (int col = 0; col < l->pixels->cols(); ++col)
            if (l->pixels->tile(col, row)) t.add(col, row);
}

const Layer* find(const Layers& ls, const Document::Id& id) {
    const auto it = ls.find(id);
    return it == ls.end() ? nullptr : &it->second;
}

// s024: id is a styled group, or inside one -- its effects reach past any
// one layer's tiles.
bool styled_around(const Layers& ls, const Document::Id& id) {
    for (Document::Id at = id; !at.empty();) {
        const Layer* l = find(ls, at);
        if (!l) return false;
        if (l->kind == Kind::Group && l->styles.any()) return true;
        at = l->parent;
    }
    return false;
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
    // s030: a filter layer reads everything below it (and a blur spreads):
    // with one in either picture, any change redraws everything.
    const auto has_filter = [](const Layers& ls) {
        for (const auto& [id, l] : ls) if (l.kind == Kind::Filter) return true;
        return false;
    };
    const bool filters = has_filter(before) || has_filter(after);
    // s035: a path layer masks its neighbour below -- any change to one (or
    // to the layers next to it) redraws everything.
    const auto masking = [](const Layers& ls) {
        for (const auto& [id, l] : ls) if (l.kind == Kind::Path && l.path_mask) return true;
        return false;
    };
    const bool path_masks = masking(before) || masking(after);
    std::vector<Document::Id> ids;
    for (const auto& [id, l] : before) ids.push_back(id);
    for (const auto& [id, l] : after) if (!before.count(id)) ids.push_back(id);
    for (const auto& id : ids) {
        const Layer* a = find(before, id);
        const Layer* b = find(after, id);
        if (a && b && *a == *b) continue;
        // s035: a path layer's outline / switches -- nothing to redraw when it
        // masks nothing; else the tiles under the old and the new outline
        // (all of them when a filter layer could read the masked layer).
        if (a && b && a->kind == Kind::Path && b->kind == Kind::Path) {
            Layer x = *a;
            x.path = b->path; x.hints = b->hints; x.path_select = b->path_select; x.name = b->name; x.locks = b->locks;
            if (x == *b) {   // only the outline / Selection switch / name / locks changed
                if (!a->path_mask || !a->visible) continue;   // it masks nothing (and still doesn't)
                bool reads = false;   // a filter layer above it, in its scope or an outer one
                for (const auto& [fid, f] : after)
                    if (f.kind == Kind::Filter && (f.parent != b->parent || f.pos > b->pos)) reads = true;
                if (!reads) {
                    for (const auto* pp : {a->path.get(), b->path.get()}) {
                        if (!pp || pp->empty()) continue;
                        const auto bx = path::box(*pp);
                        const double pad = 2 + 2 * b->edges.feather;   // s036: a feathered outline reaches further
                        const int c0 = std::max(0, static_cast<int>(std::floor(bx.x0 - pad)) / kTile), c1 = std::min(t.cols - 1, static_cast<int>(std::ceil(bx.x1 + pad)) / kTile);
                        const int r0 = std::max(0, static_cast<int>(std::floor(bx.y0 - pad)) / kTile), r1 = std::min(t.rows - 1, static_cast<int>(std::ceil(bx.y1 + pad)) / kTile);
                        for (int r = r0; r <= r1; ++r)
                            for (int c = c0; c <= c1; ++c) t.add(c, r);
                    }
                    continue;
                }
            } else if (!a->path_mask && !b->path_mask) {
                // switches / visibility of a path that masks nothing before or after: nothing shows
                Layer y = *a;
                y.path = b->path; y.hints = b->hints; y.path_select = b->path_select; y.name = b->name; y.locks = b->locks; y.visible = b->visible;
                if (y == *b) continue;
            }
        }
        if ((a && a->kind == Kind::Path && !a->path_mask && !b) || (b && b->kind == Kind::Path && !b->path_mask && !a)) continue;   // a path layer that masks nothing came or went
        // s035: with a path masking its neighbour, a change in the stack's
        // shape (a layer in, out or moved) or to a path layer may move the
        // mask: everything. Other changes: their own tiles, as ever.
        if (path_masks && (!a || !b || a->kind == Kind::Path || b->kind == Kind::Path || a->pos != b->pos || a->parent != b->parent)) {
            t.add_all();
            return t;
        }
        if (filters) {   // s030 -- unless only a name or a lock changed
            if (a && b) {
                Layer x = *a;
                x.name = b->name;
                x.locks = b->locks;
                if (x == *b) continue;
            }
            t.add_all();
            return t;
        }
        // s036: a feathered mask spreads past its own tiles (and a defringe
        // reads its neighbours): any change, everything.
        const auto spreads = [](const Layer* l) { return l && l->mask && (l->edges.feather > 0 || l->edges.defringe > 0); };
        if (spreads(a) || spreads(b)) { t.add_all(); return t; }
        // s018: a layer with live filters reaches past its own tiles (a blur
        // spreads): any change to it, or to its stack, redraws everything.
        if ((a && live::active(*a)) || (b && live::active(*b))) { t.add_all(); return t; }
        if (styled_around(before, id) || styled_around(after, id)) { t.add_all(); return t; }   // s024
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

Layers below(const Layers& all, const Document::Id& id) {
    Layers out = all;
    // s030: the layer itself and its later siblings; at each group above it,
    // only the group's LATER siblings -- never the group itself (hiding it
    // hid everything under it too: a filter layer in a Pass Through group
    // with the Background inside saw nothing, Scott, s030; and an
    // adjustment's histogram there came out empty).
    for (Document::Id at = id; !at.empty();) {
        const auto it = all.find(at);
        if (it == all.end()) break;
        const Layer& me = it->second;
        for (auto& [oid, l] : out)
            if (l.parent == me.parent && ((oid == at && at == id) || l.pos > me.pos)) l.visible = false;
        at = me.parent;
    }
    return out;
}

adjust::Histogram histogram_of(const Image& im, const Layers& layers, int max_tiles, const std::function<bool()>& cancelled) {
    adjust::Histogram h{};
    const int cols = (im.width + kTile - 1) / kTile, rows = (im.height + kTile - 1) / kTile, n = cols * rows;
    if (n <= 0) return h;
    const int step = std::max(1, n / std::max(1, max_tiles));
    std::vector<std::uint32_t> px;
    for (int i = 0; i < n; i += step) {
        if (cancelled && cancelled()) return h;
        int w = 0, ht = 0;
        composite_tile_argb(im, layers, i % cols, i / cols, px, w, ht);
        const auto one = adjust::histogram(px);
        for (std::size_t c = 0; c < h.size(); ++c)
            for (std::size_t b = 0; b < 256; ++b) h[c][b] += one[c][b];
    }
    return h;
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
