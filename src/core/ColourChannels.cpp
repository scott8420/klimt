// core::select -- the colour channels (s026). See core/ColourChannels.hpp.
#include "core/ColourChannels.hpp"

#include <algorithm>

namespace klimt::core::select {

std::vector<std::string> colour_channels(const pixel::Format& f) {
    switch (f.mode) {
        case pixel::Mode::RGB:  return {"Red", "Green", "Blue"};
        case pixel::Mode::Gray: return {"Gray"};
        case pixel::Mode::CMYK: return {"Cyan", "Magenta", "Yellow", "Black"};
        case pixel::Mode::Lab:  return {"Lightness"};
    }
    return {};
}

float channel_grey(pixel::Mode m, int k, float r, float g, float b) {
    r = std::clamp(r, 0.f, 1.f); g = std::clamp(g, 0.f, 1.f); b = std::clamp(b, 0.f, 1.f);
    switch (m) {
        case pixel::Mode::RGB: return k == 0 ? r : k == 1 ? g : b;
        case pixel::Mode::Gray:
        case pixel::Mode::Lab: return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        case pixel::Mode::CMYK: {
            const float kk = 1.f - std::max({r, g, b});
            float ink = kk;
            if (k < 3) {
                const float c = k == 0 ? r : k == 1 ? g : b;
                ink = kk >= 1.f ? 0.f : (1.f - c - kk) / (1.f - kk);
            }
            return 1.f - std::clamp(ink, 0.f, 1.f);   // a plate: ink shows dark
        }
    }
    return 0.f;
}

std::optional<pixel::TileMap> colour_channel_mask(const Image& im, const Layers& layers, int k, const pixel::Format& mf,
                                                  const std::function<bool()>& stop) {
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
                    const float a = std::clamp(p[3], 0.f, 1.f);   // over white where it is transparent
                    const float v = channel_grey(im.format.mode, k, p[0] + 1.f - a, p[1] + 1.f - a, p[2] + 1.f - a);
                    if (v <= 0.f) continue;
                    pixel::Px q{};
                    q.c[0] = v;
                    w.set(x, y, q);
                    any = true;
                }
            grid.push_back(any ? w.freeze() : nullptr);
        }
    return pixel::TileMap::from_tiles(mf, im.width, im.height, std::move(grid));
}

bool load_colour_channel(DocJournal& j, Document& d, int k, Op op) {
    const auto names = colour_channels(d.image().format);
    if (k < 0 || k >= static_cast<int>(names.size())) return false;
    const auto m = colour_channel_mask(d.image(), d.snapshot(), k, d.mask_format());
    if (!m) return false;
    return set(j, d, combine(d.selection(), *m, op), "Load " + names[static_cast<std::size_t>(k)] + " Channel");
}

Document::Id duplicate_colour_channel(DocJournal& j, Document& d, int k) {
    const auto names = colour_channels(d.image().format);
    if (k < 0 || k >= static_cast<int>(names.size())) return {};
    auto m = colour_channel_mask(d.image(), d.snapshot(), k, d.mask_format());
    if (!m) return {};
    const auto& name = names[static_cast<std::size_t>(k)];
    return save_channel_mask(j, d, std::make_shared<const pixel::TileMap>(std::move(*m)), name + " copy", "Duplicate " + name + " Channel");
}

}  // namespace klimt::core::select
