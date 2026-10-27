#include "core/Composite.hpp"

#include <algorithm>

namespace klimt::core {

namespace {
using pixel::kTile;
constexpr std::size_t kPx = static_cast<std::size_t>(kTile) * kTile;

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
    bool pixels_of(const Layer& l, std::vector<float>& out) const {
        if (!l.pixels) return false;
        const auto t = l.pixels->tile(col, row);
        if (!t) return false;
        std::vector<float> raw;
        t->premultiplied(raw);
        const int n = l.pixels->format().channels();
        out.assign(kPx * 4, 0.f);
        for (std::size_t i = 0; i < kPx; ++i) {
            const float* s = raw.data() + i * static_cast<std::size_t>(n);
            float* d = out.data() + i * 4;
            if (n >= 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
            else { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 1.f; }   // no alpha: opaque
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
