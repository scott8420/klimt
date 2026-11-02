// core/Fill -- Content-Aware Fill (s088). See core/Fill.hpp.
#include "core/Fill.hpp"
#include "core/Selection.hpp"
#include "core/Transfer.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace klimt::core::fill {

namespace P = pixel;

P::Rect context_box(const P::Rect& hole, int W, int H) {
    const int grow = std::max(32, std::max(hole.w, hole.h));
    const int x0 = std::max(0, hole.x - grow), y0 = std::max(0, hole.y - grow);
    const int x1 = std::min(W, hole.x + hole.w + grow), y1 = std::min(H, hole.y + hole.h + grow);
    return {x0, y0, x1 - x0, y1 - y0};
}

std::optional<P::TileMap> content_aware(const P::TileMap& picture, const P::TileMap& hole, const P::Format& out_format,
                                        const seg::Model& model, const std::string& model_path, std::string& error,
                                        const std::function<bool()>& cancelled) {
    const int W = picture.width(), H = picture.height();
    const P::Rect hb = select::bounds(hole);
    if (hb.empty()) { error = "nothing is selected"; return std::nullopt; }
    const P::Rect box = context_box(hb, W, H);
    const int S = model.side;
    const double k = static_cast<double>(S) / std::max(box.w, box.h);   // picture px -> model px
    // The context as 8-bit RGB floats, and the hole's coverage, cut to the box.
    const P::Format f8{P::Depth::U8, P::Mode::RGB, true};
    const P::TileMap m8 = picture.format() == f8 ? picture : transfer::converted(picture, f8);
    std::vector<float> ctx(static_cast<std::size_t>(box.w) * box.h * 3), cov(static_cast<std::size_t>(box.w) * box.h);
    {
        std::vector<std::uint8_t> row(static_cast<std::size_t>(W) * 4);
        for (int y = 0; y < box.h; ++y) {
            m8.row_bytes(box.y + y, row.data());
            for (int x = 0; x < box.w; ++x) {
                const std::uint8_t* s = row.data() + static_cast<std::size_t>(box.x + x) * 4;
                const float a = s[3] / 255.f;   // transparency seen over white
                for (int c = 0; c < 3; ++c) ctx[(static_cast<std::size_t>(y) * box.w + x) * 3 + c] = (s[c] / 255.f) * a + (1 - a);
                cov[static_cast<std::size_t>(y) * box.w + x] = hole.get(box.x + x, box.y + y).c[0];
            }
            if ((y & 63) == 0 && cancelled && cancelled()) { error = "cancelled"; return std::nullopt; }
        }
    }
    const auto ctx_at = [&](double x, double y, int c) {   // bilinear, edges repeated
        x = std::clamp(x, 0.0, box.w - 1.0);
        y = std::clamp(y, 0.0, box.h - 1.0);
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, box.w - 1), y1 = std::min(y0 + 1, box.h - 1);
        const double fx = x - x0, fy = y - y0;
        const auto v = [&](int xx, int yy) { return static_cast<double>(ctx[(static_cast<std::size_t>(yy) * box.w + xx) * 3 + c]); };
        return static_cast<float>((v(x0, y0) * (1 - fx) + v(x1, y0) * fx) * (1 - fy) + (v(x0, y1) * (1 - fx) + v(x1, y1) * fx) * fy);
    };
    // Into the model's square: the box scaled (aspect kept), edges repeated
    // past it; the hole grown two model pixels (its rim is half-known).
    std::vector<float> rgb(static_cast<std::size_t>(S) * S * 3), holes(static_cast<std::size_t>(S) * S, 0.f);
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            const double px = (x + 0.5) / k - 0.5, py = (y + 0.5) / k - 0.5;
            for (int c = 0; c < 3; ++c) rgb[(static_cast<std::size_t>(y) * S + x) * 3 + c] = ctx_at(px, py, c);
            const int ix = static_cast<int>(std::lround(px)), iy = static_cast<int>(std::lround(py));
            if (ix >= 0 && iy >= 0 && ix < box.w && iy < box.h && cov[static_cast<std::size_t>(iy) * box.w + ix] > 0.5f)
                holes[static_cast<std::size_t>(y) * S + x] = 1.f;
        }
    {
        std::vector<float> grown = holes;
        for (int y = 0; y < S; ++y)
            for (int x = 0; x < S; ++x) {
                if (holes[static_cast<std::size_t>(y) * S + x] == 0.f) continue;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx >= 0 && yy >= 0 && xx < S && yy < S) grown[static_cast<std::size_t>(yy) * S + xx] = 1.f;
                    }
            }
        holes.swap(grown);
    }
    auto filled = seg::inpaint(model, model_path, rgb, holes, error, cancelled);
    if (!filled) return std::nullopt;
    const auto fill_at = [&](double x, double y, int c) {   // picture px (in the box) -> the model's paint
        const double mx = std::clamp((x + 0.5) * k - 0.5, 0.0, S - 1.0), my = std::clamp((y + 0.5) * k - 0.5, 0.0, S - 1.0);
        const int x0 = static_cast<int>(mx), y0 = static_cast<int>(my);
        const int x1 = std::min(x0 + 1, S - 1), y1 = std::min(y0 + 1, S - 1);
        const double fx = mx - x0, fy = my - y0;
        const auto v = [&](int xx, int yy) { return static_cast<double>((*filled)[(static_cast<std::size_t>(yy) * S + xx) * 3 + c]); };
        return static_cast<float>((v(x0, y0) * (1 - fx) + v(x1, y0) * fx) * (1 - fy) + (v(x0, y1) * (1 - fx) + v(x1, y1) * fx) * fy);
    };
    // LaMa's colours drift a little from the picture's (often a touch more
    // saturated): over the ring just outside the hole, where both show the
    // picture, each channel's best gain and offset (least squares) is found
    // and put on what it painted, so no tone step shows at the rim.
    float gain[3] = {1, 1, 1}, shift[3] = {0, 0, 0};
    {
        double sf[3] = {0, 0, 0}, so[3] = {0, 0, 0}, sff[3] = {0, 0, 0}, sfo[3] = {0, 0, 0};
        long n = 0;
        for (int y = std::max(box.y, hb.y - 8); y < std::min(box.y + box.h, hb.y + hb.h + 8); ++y)
            for (int x = std::max(box.x, hb.x - 8); x < std::min(box.x + box.w, hb.x + hb.w + 8); ++x) {
                const int bx = x - box.x, by = y - box.y;
                if (cov[static_cast<std::size_t>(by) * box.w + bx] > 0) continue;
                bool near = false;   // within 8 px of the hole
                for (int dy = -8; dy <= 8 && !near; dy += 4)
                    for (int dx = -8; dx <= 8 && !near; dx += 4) {
                        const int xx = bx + dx, yy = by + dy;
                        near = xx >= 0 && yy >= 0 && xx < box.w && yy < box.h && cov[static_cast<std::size_t>(yy) * box.w + xx] > 0.5f;
                    }
                if (!near) continue;
                for (int c = 0; c < 3; ++c) {
                    const double f = fill_at(bx, by, c), o = ctx[(static_cast<std::size_t>(by) * box.w + bx) * 3 + c];
                    sf[c] += f; so[c] += o; sff[c] += f * f; sfo[c] += f * o;
                }
                ++n;
            }
        for (int c = 0; c < 3 && n > 8; ++c) {
            const double mf = sf[c] / n, mo = so[c] / n, var = sff[c] / n - mf * mf;
            double g = var > 1e-4 ? (sfo[c] / n - mf * mo) / var : 1.0;   // a flat ring: the mean only
            g = std::clamp(g, 0.5, 2.0);
            gain[c] = static_cast<float>(g);
            shift[c] = static_cast<float>(mo - g * mf);
        }
    }
    // Back: the hole (grown 2 px, then softened) takes the paint.
    const P::Rect area{std::max(0, hb.x - 3), std::max(0, hb.y - 3), 0, 0};
    const int ax1 = std::min(W, hb.x + hb.w + 3), ay1 = std::min(H, hb.y + hb.h + 3);
    const P::Rect rim{area.x, area.y, ax1 - area.x, ay1 - area.y};
    const auto cov_at = [&](int x, int y) {
        if (x < box.x || y < box.y || x >= box.x + box.w || y >= box.y + box.h) return 0.f;
        return cov[static_cast<std::size_t>(y - box.y) * box.w + (x - box.x)];
    };
    std::vector<float> grown(static_cast<std::size_t>(rim.w) * rim.h);
    for (int y = 0; y < rim.h; ++y)
        for (int x = 0; x < rim.w; ++x) {
            float m = 0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) m = std::max(m, cov_at(rim.x + x + dx, rim.y + y + dy));
            grown[static_cast<std::size_t>(y) * rim.w + x] = m;
        }
    P::TileMap out(out_format, W, H);
    out = out.paint(rim, [&](int x, int y, const P::Px&) {
        float a = 0;
        int n = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int xx = x - rim.x + dx, yy = y - rim.y + dy;
                if (xx < 0 || yy < 0 || xx >= rim.w || yy >= rim.h) continue;
                a += grown[static_cast<std::size_t>(yy) * rim.w + xx];
                ++n;
            }
        a = n ? a / n : 0;
        if (a <= 0) return P::transparent();
        const auto ch = [&](int c) { return std::clamp(gain[c] * fill_at(x - box.x, y - box.y, c) + shift[c], 0.f, 1.f); };
        return P::rgba(ch(0), ch(1), ch(2), a);
    });
    return out;
}

}  // namespace klimt::core::fill
