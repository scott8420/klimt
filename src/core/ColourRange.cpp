// core::select -- Color Range (s024). See core/ColourRange.hpp.
#include "core/ColourRange.hpp"

#include <algorithm>
#include <cmath>

namespace klimt::core::select {

float colour_weight(const ColourRange& r, float red, float green, float blue) {
    if (r.samples.empty()) return r.invert ? 1.f : 0.f;
    const float fuzz = static_cast<float>(std::clamp(r.fuzziness, 0, 200));
    float best = 1e9f;
    for (const auto& s : r.samples) {
        const float dr = (red - s[0]) * 255.f, dg = (green - s[1]) * 255.f, db = (blue - s[2]) * 255.f;
        best = std::min(best, std::sqrt(dr * dr + dg * dg + db * db));
    }
    float w;
    if (fuzz <= 0.f) w = best < 0.5f ? 1.f : 0.f;
    else w = std::clamp((fuzz - best) / (fuzz * 0.5f), 0.f, 1.f);   // whole within fuzz / 2, none at fuzz
    return r.invert ? 1.f - w : w;
}

std::optional<pixel::TileMap> colour_range_mask(const Image& im, const Layers& layers, const ColourRange& r,
                                                const pixel::Format& mf, const std::function<bool()>& stop) {
    pixel::TileMap out(mf, im.width, im.height);
    std::vector<pixel::TileMap::TileRef> grid;
    grid.reserve(static_cast<std::size_t>(out.cols()) * static_cast<std::size_t>(out.rows()));
    std::vector<float> px;
    for (int row = 0; row < out.rows(); ++row)
        for (int col = 0; col < out.cols(); ++col) {
            if (stop && stop()) return std::nullopt;
            composite_tile(im, layers, col, row, px);
            pixel::TileWriter w(mf, pixel::Px{});
            bool any = false;
            for (int y = 0; y < pixel::kTile; ++y)
                for (int x = 0; x < pixel::kTile; ++x) {
                    const float* p = px.data() + (static_cast<std::size_t>(y) * pixel::kTile + static_cast<std::size_t>(x)) * 4;
                    const float a = p[3];
                    if (a <= 0.f) continue;   // nothing there: never selected
                    const float k = colour_weight(r, p[0] / a, p[1] / a, p[2] / a) * std::min(1.f, a);
                    if (k <= 0.f) continue;
                    pixel::Px v{};
                    v.c[0] = k;
                    w.set(x, y, v);
                    any = true;
                }
            grid.push_back(any ? w.freeze() : nullptr);
        }
    return pixel::TileMap::from_tiles(mf, im.width, im.height, std::move(grid));
}

bool colour_range(DocJournal& j, Document& d, const ColourRange& r, Op op) {
    const auto m = colour_range_mask(d.image(), d.snapshot(), r, d.mask_format());
    if (!m) return false;
    return set(j, d, combine(d.selection(), *m, op), "Color Range");
}

}  // namespace klimt::core::select
