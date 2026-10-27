#include "formats/Formats.hpp"

#include <algorithm>
#include <cstring>

namespace klimt::formats {

using pixel::kTile;

BandBuilder::BandBuilder(const pixel::Format& f, int width, int height)
    : m_f(f), m_w(width), m_h(height),
      m_row(static_cast<std::size_t>(width) * static_cast<std::size_t>(f.bytes_per_pixel())) {
    const int cols = (width + kTile - 1) / kTile, rows = (height + kTile - 1) / kTile;
    m_grid.resize(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows));
    m_band.assign(m_row * kTile, 0);
}

std::uint8_t* BandBuilder::row(int y) {
    return m_band.data() + static_cast<std::size_t>(y - m_band0) * m_row;
}

void BandBuilder::done(int y) {
    m_filled = y - m_band0 + 1;
    if (m_filled == kTile || y == m_h - 1) cut();
}

// The band's rows become one row of tiles. The part of an edge tile beyond
// the image is padded with the tile's first pixel, so an edge tile whose live
// part is all one colour still collapses to a uniform tile.
void BandBuilder::cut() {
    if (m_filled <= 0) return;
    const auto bpp = static_cast<std::size_t>(m_f.bytes_per_pixel());
    const int cols = (m_w + kTile - 1) / kTile, row = m_band0 / kTile;
    for (int col = 0; col < cols; ++col) {
        std::vector<std::uint8_t> t(static_cast<std::size_t>(kTile) * kTile * bpp);
        const int x0 = col * kTile, n = std::min(kTile, m_w - x0);
        const std::uint8_t* first = m_band.data() + static_cast<std::size_t>(x0) * bpp;
        for (int y = 0; y < kTile; ++y) {
            std::uint8_t* d = t.data() + static_cast<std::size_t>(y) * kTile * bpp;
            int live = 0;
            if (y < m_filled) {
                std::memcpy(d, m_band.data() + static_cast<std::size_t>(y) * m_row + static_cast<std::size_t>(x0) * bpp,
                            static_cast<std::size_t>(n) * bpp);
                live = n;
            }
            for (int x = live; x < kTile; ++x) std::memcpy(d + static_cast<std::size_t>(x) * bpp, first, bpp);
        }
        m_grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(col)] =
            pixel::Tile::from_bytes(m_f, std::move(t));
    }
    m_band0 += kTile;
    m_filled = 0;
    std::fill(m_band.begin(), m_band.end(), 0);
}

pixel::TileMap BandBuilder::take() {
    cut();
    return pixel::TileMap::from_tiles(m_f, m_w, m_h, std::move(m_grid));
}

void unpremultiply_row(const pixel::Format& f, std::uint8_t* row, int width) {
    if (!f.alpha) return;
    const int n = f.channels(), colours = f.colour_channels();
    for (int x = 0; x < width; ++x) {
        switch (f.depth) {
            case pixel::Depth::U8: {
                std::uint8_t* p = row + static_cast<std::size_t>(x) * static_cast<std::size_t>(n);
                const int a = p[n - 1];
                for (int k = 0; k < colours; ++k)
                    p[k] = a == 0 ? 0 : static_cast<std::uint8_t>(std::min(255, (p[k] * 255 + a / 2) / a));
                break;
            }
            case pixel::Depth::U16: {
                auto* p = reinterpret_cast<std::uint16_t*>(row) + static_cast<std::size_t>(x) * static_cast<std::size_t>(n);
                const std::uint32_t a = p[n - 1];
                for (int k = 0; k < colours; ++k)
                    p[k] = a == 0 ? 0 : static_cast<std::uint16_t>(std::min<std::uint32_t>(65535, (p[k] * 65535u + a / 2) / a));
                break;
            }
            case pixel::Depth::F32: {
                auto* p = reinterpret_cast<float*>(row) + static_cast<std::size_t>(x) * static_cast<std::size_t>(n);
                const float a = p[n - 1];
                for (int k = 0; k < colours; ++k) p[k] = a > 0.f ? p[k] / a : 0.f;
                break;
            }
        }
    }
}

}  // namespace klimt::formats
