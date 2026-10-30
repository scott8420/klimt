// core::matte -- a cut-out's edge (s036). docs/masks.md.
#include "core/Matte.hpp"
#include "core/Selection.hpp"

#include <algorithm>
#include <cmath>

namespace klimt::core::matte {

namespace {
using pixel::kTile;
constexpr std::size_t kPx = static_cast<std::size_t>(kTile) * kTile;

// A tile's samples as floats: premultiplied colour + alpha (n per pixel), or
// all zero for an absent tile (transparent / hides nothing).
void tile_floats(const pixel::TileMap& m, int col, int row, std::vector<float>& out) {
    const auto t = m.tile(col, row);
    if (!t) { out.assign(kPx * static_cast<std::size_t>(m.format().channels()), 0.f); return; }
    t->premultiplied(out);
}
}  // namespace

void Field::at(double x, double y, float* out) const {
    const double u = std::clamp((x + 0.5) / cell - 0.5, 0.0, static_cast<double>(W - 1));
    const double w = std::clamp((y + 0.5) / cell - 0.5, 0.0, static_cast<double>(H - 1));
    const int x0 = static_cast<int>(u), y0 = static_cast<int>(w);
    const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
    const float fx = static_cast<float>(u - x0), fy = static_cast<float>(w - y0);
    const auto p = [&](int cx, int cy) { return v.data() + (static_cast<std::size_t>(cy) * static_cast<std::size_t>(W) + static_cast<std::size_t>(cx)) * static_cast<std::size_t>(ch); };
    const float *a = p(x0, y0), *b = p(x1, y0), *c = p(x0, y1), *d = p(x1, y1);
    for (int k = 0; k < ch; ++k)
        out[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}

Field filled(int width, int height, int ch, int cell,
             const std::function<void(const std::function<void(int, int, const float*, float)>&)>& feed) {
    Field f;
    f.cell = std::max(1, cell);
    f.ch = ch;
    f.W = std::max(1, (width + f.cell - 1) / f.cell);
    f.H = std::max(1, (height + f.cell - 1) / f.cell);
    // Level 0: weighted sums (ch colours, then the weight) per cell.
    struct Level { int W, H; std::vector<float> s; };
    const auto stride = static_cast<std::size_t>(ch + 1);
    std::vector<Level> lv;
    lv.push_back({f.W, f.H, std::vector<float>(static_cast<std::size_t>(f.W) * static_cast<std::size_t>(f.H) * stride, 0.f)});
    bool any = false;
    feed([&](int x, int y, const float* c, float wt) {
        if (wt <= 0 || x < 0 || y < 0 || x >= width || y >= height) return;
        float* s = lv[0].s.data() + (static_cast<std::size_t>(y / f.cell) * static_cast<std::size_t>(f.W) + static_cast<std::size_t>(x / f.cell)) * stride;
        for (int k = 0; k < ch; ++k) s[k] += c[k] * wt;
        s[ch] += wt;
        any = true;
    });
    if (!any) return Field{};
    // PUSH: each coarser cell sums its (up to) four children.
    while (lv.back().W > 1 || lv.back().H > 1) {
        const Level& a = lv.back();
        Level b{(a.W + 1) / 2, (a.H + 1) / 2, {}};
        b.s.assign(static_cast<std::size_t>(b.W) * static_cast<std::size_t>(b.H) * stride, 0.f);
        for (int y = 0; y < a.H; ++y)
            for (int x = 0; x < a.W; ++x) {
                const float* s = a.s.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.W) + static_cast<std::size_t>(x)) * stride;
                float* d = b.s.data() + (static_cast<std::size_t>(y / 2) * static_cast<std::size_t>(b.W) + static_cast<std::size_t>(x / 2)) * stride;
                for (std::size_t k = 0; k < stride; ++k) d[k] += s[k];
            }
        lv.push_back(std::move(b));
    }
    // PULL: from the top, each cell its own average where it has weight
    // (a weight of 1 or more: wholly its own), the rest from its parent.
    std::vector<float> parent(static_cast<std::size_t>(ch), 0.f), cur;
    {
        const float* s = lv.back().s.data();
        for (int k = 0; k < ch; ++k) parent[static_cast<std::size_t>(k)] = s[ch] > 0 ? s[k] / s[ch] : 0.f;
    }
    std::vector<float> up = parent;   // the level above, resolved (W x H x ch)
    for (int li = static_cast<int>(lv.size()) - 2; li >= 0; --li) {
        const Level& a = lv[static_cast<std::size_t>(li)];
        const Level& p = lv[static_cast<std::size_t>(li) + 1];
        cur.assign(static_cast<std::size_t>(a.W) * static_cast<std::size_t>(a.H) * static_cast<std::size_t>(ch), 0.f);
        for (int y = 0; y < a.H; ++y)
            for (int x = 0; x < a.W; ++x) {
                const float* s = a.s.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.W) + static_cast<std::size_t>(x)) * stride;
                const float* q = up.data() + (static_cast<std::size_t>(y / 2) * static_cast<std::size_t>(p.W) + static_cast<std::size_t>(x / 2)) * static_cast<std::size_t>(ch);
                float* o = cur.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.W) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(ch);
                const float w = std::min(1.f, s[ch]);
                for (int k = 0; k < ch; ++k) o[k] = (s[ch] > 0 ? w * (s[k] / s[ch]) : 0.f) + (1 - w) * q[k];
            }
        up.swap(cur);
    }
    f.v = std::move(up);
    return f;
}

std::optional<pixel::TileMap> defringed(const pixel::TileMap& pixels, const pixel::TileMap& hide, double width,
                                        const std::function<bool()>& cancelled) {
    const int W = pixels.width(), H = pixels.height();
    if (width <= 0 || W <= 0 || H <= 0 || hide.width() != W || hide.height() != H || !pixels.tile_count()) return pixels;
    const auto fmt = pixels.format();
    const int C = fmt.colour_channels(), n = fmt.channels();
    const bool alpha = fmt.alpha;
    const auto WW = static_cast<std::size_t>(W), N = static_cast<std::size_t>(n);
    const int cols = pixels.cols(), rows = pixels.rows();
    const auto stop = [&] { return cancelled && cancelled(); };
    // 1. How much each pixel shows (mask x alpha) and how much the mask
    //    hides it -- a byte each.
    std::vector<std::uint8_t> show(WW * static_cast<std::size_t>(H)), hid(show.size());
    std::vector<float> px, mk;
    bool shown = false, gone = false;
    for (int row = 0; row < rows; ++row) {
        if (stop()) return std::nullopt;
        for (int col = 0; col < cols; ++col) {
            tile_floats(pixels, col, row, px);
            tile_floats(hide, col, row, mk);
            for (int y = 0; y < kTile; ++y) {
                const int iy = row * kTile + y;
                if (iy >= H) break;
                for (int x = 0; x < kTile; ++x) {
                    const int ix = col * kTile + x;
                    if (ix >= W) break;
                    const std::size_t i = static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x);
                    const float a = alpha ? px[i * N + N - 1] : (pixels.tile(col, row) ? 1.f : 0.f);
                    const float h = std::clamp(mk[i], 0.f, 1.f);
                    const float s = (1 - h) * a;
                    const std::size_t g = static_cast<std::size_t>(iy) * WW + static_cast<std::size_t>(ix);
                    show[g] = static_cast<std::uint8_t>(std::lround(s * 255));
                    hid[g] = static_cast<std::uint8_t>(std::lround(h * 255));
                    shown = shown || s >= 0.5f;
                    gone = gone || s < 0.5f;
                }
            }
        }
    }
    if (!shown || !gone) return pixels;   // no edge anywhere
    // 2. The distance of each pixel from the nearest one that shows less than half.
    std::vector<std::uint8_t> feature(show.size());
    for (std::size_t i = 0; i < show.size(); ++i) feature[i] = show[i] < 128;
    const auto dist = select::distance_field(feature, WW, static_cast<std::size_t>(H), cancelled);
    if (dist.empty()) return std::nullopt;
    feature = {};
    // 3. The estimates: the FIGURE's colour from pixels well inside (beyond
    //    the band, fully shown); the BACKGROUND's from pixels the mask hides
    //    (that still have colour).
    const float wf = static_cast<float>(width);
    const auto feed_from = [&](bool figure) {
        return [&, figure](const std::function<void(int, int, const float*, float)>& add) {
            std::vector<float> t, c(static_cast<std::size_t>(C));
            for (int row = 0; row < rows; ++row) {
                if (stop()) return;
                for (int col = 0; col < cols; ++col) {
                    if (!pixels.tile(col, row)) continue;
                    pixels.tile(col, row)->premultiplied(t);
                    for (int y = 0; y < kTile; ++y) {
                        const int iy = row * kTile + y;
                        if (iy >= H) break;
                        for (int x = 0; x < kTile; ++x) {
                            const int ix = col * kTile + x;
                            if (ix >= W) break;
                            const std::size_t g = static_cast<std::size_t>(iy) * WW + static_cast<std::size_t>(ix);
                            const std::size_t i = (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * N;
                            const float a = alpha ? t[i + N - 1] : 1.f;
                            if (a < 0.5f) continue;
                            const bool take = figure ? (show[g] >= 250 && dist[g] > wf) : (hid[g] >= 250);
                            if (!take) continue;
                            for (int k = 0; k < C; ++k) c[static_cast<std::size_t>(k)] = t[i + static_cast<std::size_t>(k)] / a;
                            add(ix, iy, c.data(), 1.f);
                        }
                    }
                }
            }
        };
    };
    const Field F = filled(W, H, C, 4, feed_from(true));
    if (stop()) return std::nullopt;
    const Field B = filled(W, H, C, 4, feed_from(false));
    if (stop()) return std::nullopt;
    if (F.empty() || B.empty()) return pixels;   // no figure to take colour from, or no background to take out
    // 4. The band: each pixel within `width` of the edge that shows at all,
    //    its background share taken out.
    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows));
    float fv[pixel::kMaxChannels], bv[pixel::kMaxChannels];
    for (int row = 0; row < rows; ++row) {
        if (stop()) return std::nullopt;
        for (int col = 0; col < cols; ++col) {
            auto& slot = grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(col)];
            slot = pixels.tile(col, row);
            if (!slot) continue;
            std::optional<pixel::TileWriter> w;
            for (int y = 0; y < kTile; ++y) {
                const int iy = row * kTile + y;
                if (iy >= H) break;
                for (int x = 0; x < kTile; ++x) {
                    const int ix = col * kTile + x;
                    if (ix >= W) break;
                    const std::size_t g = static_cast<std::size_t>(iy) * WW + static_cast<std::size_t>(ix);
                    if (show[g] == 0 || dist[g] > wf) continue;
                    // Full strength over the first half of the width, fading to nothing at it.
                    const float tmax = std::clamp((wf - dist[g]) / (0.5f * wf), 0.f, 1.f);
                    if (tmax <= 0) continue;
                    F.at(ix, iy, fv);
                    B.at(ix, iy, bv);
                    if (!w) w.emplace(slot);
                    pixel::Px p = w->get(x, y);
                    double num = 0, den = 0;
                    for (int k = 0; k < C; ++k) {
                        const double dk = bv[k] - fv[k];
                        num += (p.c[static_cast<std::size_t>(k)] - fv[k]) * dk;
                        den += dk * dk;
                    }
                    if (den < 1e-4) continue;   // figure and background alike: nothing to tell apart
                    float t = std::clamp(static_cast<float>(num / den), 0.f, tmax);
                    if (t <= 0) continue;
                    // A colour OFF the figure-background line (a detail of
                    // another colour) is no mix of the two: leave it be, the
                    // further off the line, the more.
                    double off = 0;
                    for (int k = 0; k < C; ++k) {
                        const double on = fv[k] + (num / den) * (bv[k] - fv[k]);
                        off += (p.c[static_cast<std::size_t>(k)] - on) * (p.c[static_cast<std::size_t>(k)] - on);
                    }
                    t *= static_cast<float>(std::max(0.0, 1.0 - std::sqrt(off / den) * 2));
                    if (t <= 0.002f) continue;
                    // The background's share swapped for the figure's colour:
                    // I + t (F - B) -- a pixel on the line comes out as F
                    // exactly, a wholly background one (t = 1) as the figure.
                    for (int k = 0; k < C; ++k) {
                        auto& v = p.c[static_cast<std::size_t>(k)];
                        v = std::clamp(v + t * (fv[k] - bv[k]), 0.f, 1.f);
                    }
                    w->set(x, y, p);
                }
            }
            if (w) slot = w->freeze();
        }
    }
    return pixels.with_grid(std::move(grid));
}

std::optional<pixel::TileMap> soft_edge(const pixel::TileMap& picture, const pixel::TileMap& cover, double radius,
                                        const std::function<bool()>& cancelled) {
    const int W = cover.width(), H = cover.height();
    if (radius <= 0 || W <= 0 || H <= 0 || picture.width() != W || picture.height() != H) return cover;
    const auto stop = [&] { return cancelled && cancelled(); };
    const auto fmt = picture.format();
    const int C = fmt.colour_channels(), n = fmt.channels();
    const bool alpha = fmt.alpha;
    const auto WW = static_cast<std::size_t>(W), N = static_cast<std::size_t>(n), S = WW * static_cast<std::size_t>(H);
    const int cols = cover.cols(), rows = cover.rows();
    // 1. The hard cut, a byte a pixel; which side each pixel is on.
    std::vector<std::uint8_t> cut(S), in(S), out(S);
    std::vector<float> mk;
    bool any_in = false, any_out = false;
    for (int row = 0; row < rows; ++row) {
        if (stop()) return std::nullopt;
        for (int col = 0; col < cols; ++col) {
            tile_floats(cover, col, row, mk);
            for (int y = 0; y < kTile; ++y) {
                const int iy = row * kTile + y;
                if (iy >= H) break;
                for (int x = 0; x < kTile; ++x) {
                    const int ix = col * kTile + x;
                    if (ix >= W) break;
                    const std::size_t g = static_cast<std::size_t>(iy) * WW + static_cast<std::size_t>(ix);
                    const float c = std::clamp(mk[static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)], 0.f, 1.f);
                    cut[g] = static_cast<std::uint8_t>(std::lround(c * 255));
                    in[g] = c >= 0.5f;
                    out[g] = c < 0.5f;
                    any_in = any_in || in[g];
                    any_out = any_out || out[g];
                }
            }
        }
    }
    if (!any_in || !any_out) return cover;
    // 2. How far each pixel is from the other side.
    const auto to_out = select::distance_field(out, WW, static_cast<std::size_t>(H), cancelled);   // inside pixels: from the edge
    if (to_out.empty()) return std::nullopt;
    const auto to_in = select::distance_field(in, WW, static_cast<std::size_t>(H), cancelled);     // outside pixels: from the edge
    if (to_in.empty()) return std::nullopt;
    in = {};
    out = {};
    const float r = static_cast<float>(radius);
    // 3. The estimates: the figure's colour from pixels inside beyond the
    //    band, the background's from pixels outside beyond it.
    const auto feed_from = [&](bool figure) {
        return [&, figure](const std::function<void(int, int, const float*, float)>& add) {
            std::vector<float> t, c(static_cast<std::size_t>(C));
            for (int row = 0; row < rows; ++row) {
                if (stop()) return;
                for (int col = 0; col < cols; ++col) {
                    if (!picture.tile(col, row)) continue;
                    picture.tile(col, row)->premultiplied(t);
                    for (int y = 0; y < kTile; ++y) {
                        const int iy = row * kTile + y;
                        if (iy >= H) break;
                        for (int x = 0; x < kTile; ++x) {
                            const int ix = col * kTile + x;
                            if (ix >= W) break;
                            const std::size_t g = static_cast<std::size_t>(iy) * WW + static_cast<std::size_t>(ix);
                            const bool take = figure ? (cut[g] >= 250 && to_out[g] > r) : (cut[g] <= 5 && to_in[g] > r);
                            if (!take) continue;
                            const std::size_t i = (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * N;
                            const float a = alpha ? t[i + N - 1] : 1.f;
                            if (a < 0.5f) continue;
                            for (int k = 0; k < C; ++k) c[static_cast<std::size_t>(k)] = t[i + static_cast<std::size_t>(k)] / a;
                            add(ix, iy, c.data(), 1.f);
                        }
                    }
                }
            }
        };
    };
    const Field F = filled(W, H, C, 4, feed_from(true));
    if (stop()) return std::nullopt;
    const Field B = filled(W, H, C, 4, feed_from(false));
    if (stop()) return std::nullopt;
    if (F.empty() || B.empty()) return cover;
    // 4. The band, pixel by pixel: alpha from the colour.
    const pixel::Format mf = cover.format();
    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows));
    float fv[pixel::kMaxChannels], bv[pixel::kMaxChannels];
    std::vector<float> pt;
    pixel::Px one;
    for (int row = 0; row < rows; ++row) {
        if (stop()) return std::nullopt;
        for (int col = 0; col < cols; ++col) {
            auto& slot = grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(col)];
            slot = cover.tile(col, row);
            const auto ptile = picture.tile(col, row);
            if (ptile) ptile->premultiplied(pt);
            std::optional<pixel::TileWriter> w;
            for (int y = 0; y < kTile; ++y) {
                const int iy = row * kTile + y;
                if (iy >= H) break;
                for (int x = 0; x < kTile; ++x) {
                    const int ix = col * kTile + x;
                    if (ix >= W) break;
                    const std::size_t g = static_cast<std::size_t>(iy) * WW + static_cast<std::size_t>(ix);
                    const float d = cut[g] >= 128 ? to_out[g] : to_in[g];
                    if (d > r || !ptile) continue;
                    const std::size_t i = (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * N;
                    const float a = alpha ? pt[i + N - 1] : 1.f;
                    if (a < 0.02f) continue;
                    F.at(ix, iy, fv);
                    B.at(ix, iy, bv);
                    double num = 0, den = 0;
                    for (int k = 0; k < C; ++k) {
                        const double dk = bv[k] - fv[k];
                        num += (pt[i + static_cast<std::size_t>(k)] / a - fv[k]) * dk;
                        den += dk * dk;
                    }
                    if (den < 4e-3) continue;   // figure and background alike: the hard edge stands
                    const double tt = std::clamp(num / den, 0.0, 1.0);
                    double off = 0;
                    for (int k = 0; k < C; ++k) {
                        const double on = fv[k] + tt * (bv[k] - fv[k]);
                        const double e = pt[i + static_cast<std::size_t>(k)] / a - on;
                        off += e * e;
                    }
                    const double trust = std::max(0.0, 1.0 - std::sqrt(off / den) * 2);
                    const float hard = cut[g] / 255.f;
                    const float v = static_cast<float>(trust * (1 - tt) + (1 - trust) * hard);
                    if (std::abs(v - hard) < 1.f / 255) continue;
                    if (!w) w.emplace(slot ? slot : pixel::Tile::uniform(mf, pixel::Px{}));
                    one.c[0] = std::clamp(v, 0.f, 1.f);
                    w->set(x, y, one);
                }
            }
            if (w) slot = w->freeze();
        }
    }
    return cover.with_grid(std::move(grid));
}

}  // namespace klimt::core::matte
