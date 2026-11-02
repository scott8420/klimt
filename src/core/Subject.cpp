// core/Subject -- Select > Subject (s086). See core/Subject.hpp.
#include "core/Subject.hpp"
#include "core/Selection.hpp"
#include "core/Transfer.hpp"
#include "seg/Seg.hpp"

#include <algorithm>
#include <vector>

namespace klimt::core::subject {

namespace P = pixel;

P::Rect look_box(const P::TileMap* within, int width, int height) {
    const P::Rect all{0, 0, width, height};
    if (!within) return all;
    const P::Rect b = select::bounds(*within);
    if (b.empty()) return all;
    const int pad = std::max(16, std::max(b.w, b.h) / 10);
    const int x0 = std::max(0, b.x - pad), y0 = std::max(0, b.y - pad);
    const int x1 = std::min(width, b.x + b.w + pad), y1 = std::min(height, b.y + b.h + pad);
    return {x0, y0, x1 - x0, y1 - y0};
}

std::optional<P::TileMap> mask(const P::TileMap& picture, const P::TileMap* within, const P::Format& mask_format,
                               const std::string& model_path, int side, std::string& error, const std::function<bool()>& cancelled) {
    const int W = picture.width(), H = picture.height();
    const P::Rect box = look_box(within, W, H);
    if (box.w < 2 || box.h < 2) { error = "the selection is too small to look in"; return std::nullopt; }
    // The box's pixels as 8-bit RGB (what the model takes).
    const P::Format f8{P::Depth::U8, P::Mode::RGB, true};
    const P::TileMap m8 = picture.format() == f8 ? picture : transfer::converted(picture, f8);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(W) * 4);
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(box.w) * box.h * 3);
    for (int y = 0; y < box.h; ++y) {
        m8.row_bytes(box.y + y, row.data());
        for (int x = 0; x < box.w; ++x) {
            const std::uint8_t* s = row.data() + static_cast<std::size_t>(box.x + x) * 4;
            std::uint8_t* d = rgb.data() + (static_cast<std::size_t>(y) * box.w + x) * 3;
            // Transparent pixels are seen over white (as a page shows them).
            const unsigned a = s[3];
            for (int c = 0; c < 3; ++c) d[c] = static_cast<std::uint8_t>((s[c] * a + 255 * (255 - a) + 127) / 255);
        }
        if ((y & 63) == 0 && cancelled && cancelled()) { error = "cancelled"; return std::nullopt; }
    }
    auto cov = seg::salient(model_path, side, rgb.data(), box.w, box.h, error, cancelled);
    if (!cov) return std::nullopt;
    // Into the selection's format at the picture's size, cut to the selection.
    P::TileMap out(mask_format, W, H);
    out = out.paint(box, [&](int x, int y, const P::Px&) {
        float v = (*cov)[static_cast<std::size_t>(y - box.y) * box.w + (x - box.x)];
        if (within) v *= within->get(x, y).c[0];
        P::Px p{};
        p.c[0] = std::clamp(v, 0.f, 1.f);
        return p;
    });
    if (select::bounds(out).empty()) { error = "nothing stands out there"; return std::nullopt; }
    return out;
}

}  // namespace klimt::core::subject
