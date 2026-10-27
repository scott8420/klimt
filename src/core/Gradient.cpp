#include "core/Gradient.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace klimt::core::gradient {

namespace {
// A fixed, cheap hash per pixel: the dither.
float noise(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffffu) / 65535.f - 0.5f;   // -0.5 .. 0.5
}
}  // namespace

double position(const Spec& s, double x, double y) {
    const double px = x + 0.5 - s.a.x, py = y + 0.5 - s.a.y;
    const double dx = s.b.x - s.a.x, dy = s.b.y - s.a.y, len2 = dx * dx + dy * dy;
    if (len2 <= 0) return 0;
    if (s.shape == Shape::Radial) return std::clamp(std::sqrt((px * px + py * py) / len2), 0.0, 1.0);
    return std::clamp((px * dx + py * dy) / len2, 0.0, 1.0);
}

std::optional<pixel::TileMap> over(const pixel::TileMap& target, const pixel::TileMap* mask, const Spec& s,
                                   const std::function<bool()>& cancelled) {
    const auto& f = target.format();
    const int cc = f.colour_channels();
    const auto ai = static_cast<std::size_t>(f.channels() - 1);
    const float op = static_cast<float>(std::clamp(s.opacity, 0.0, 1.0));
    // One step of the stored depth: the dither's size.
    const float q = f.depth == pixel::Depth::U8 ? 1.f / 255 : f.depth == pixel::Depth::U16 ? 1.f / 65535 : 0.f;
    pixel::Rect r{0, 0, target.width(), target.height()};
    if (mask) {
        r = select::extent(*mask);
        if (r.empty()) return target;
    }
    // Row by row so a cancel is heard; each band of tiles is one paint().
    pixel::TileMap out = target;
    for (int y0 = r.y; y0 < r.y + r.h; y0 += pixel::kTile) {
        if (cancelled && cancelled()) return std::nullopt;
        const pixel::Rect band = pixel::Rect{r.x, y0, r.w, pixel::kTile}.intersect(r);
        out = out.paint(band, [&](int x, int y, const pixel::Px& o) {
            float a = op;
            if (mask) a *= select::coverage(*mask, x, y);
            if (a <= 0.f) return o;
            const float t = static_cast<float>(position(s, x, y));
            pixel::Px g;
            for (int k = 0; k < f.channels(); ++k) {
                const auto i = static_cast<std::size_t>(k);
                g.c[i] = s.from.c[i] + (s.to.c[i] - s.from.c[i]) * t;
            }
            if (q > 0) for (int k = 0; k < cc; ++k) g.c[static_cast<std::size_t>(k)] += noise(x, y) * q;
            pixel::Px p = o;
            if (!f.alpha || s.alpha_lock) {
                if (f.alpha && o.c[ai] <= 0.f) return o;
                const float ga = f.alpha ? g.c[ai] * a : a;
                for (int k = 0; k < cc; ++k) { const auto i = static_cast<std::size_t>(k); p.c[i] = o.c[i] + (g.c[i] - o.c[i]) * ga; }
                return p;
            }
            const float ga = std::clamp(g.c[ai], 0.f, 1.f) * a, oa = o.c[ai], outa = ga + oa * (1 - ga);
            for (int k = 0; k < cc; ++k) {
                const auto i = static_cast<std::size_t>(k);
                p.c[i] = outa > 0.f ? (g.c[i] * ga + o.c[i] * oa * (1 - ga)) / outa : 0.f;
            }
            p.c[ai] = outa;
            return p;
        });
    }
    return out;
}

bool apply(DocJournal& j, Document& d, const Document::Id& id, Spec s, bool on_mask) {
    const auto l = d.get(id);
    if (!l || l->kind != Kind::Pixel || d.locked_all(id)) return false;
    if (on_mask) {
        if (!l->mask) return false;
        s.alpha_lock = false;
        auto m = over(*l->mask, d.selection(), s);
        return m && layers::set_mask_if(j, d, id, l->mask, std::move(*m), "Gradient on Mask");
    }
    if (l->locks & lock::Pixels) return false;
    s.alpha_lock = (l->locks & lock::Alpha) != 0;
    const pixel::TileMap src = l->pixels ? *l->pixels : d.blank();
    auto m = over(src, d.selection(), s);
    return m && layers::set_pixels_if(j, d, id, l->pixels, std::move(*m), "Gradient");
}

}  // namespace klimt::core::gradient
