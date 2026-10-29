// core::heal -- the healing brushes (s015 m3). See core/Heal.hpp.
#include "core/Heal.hpp"
#include "core/File.hpp"
#include "core/Filter.hpp"
#include "core/Selection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace klimt::core::heal {

namespace {
using pixel::kTile;
using pixel::Px;
using pixel::Rect;

// The bounding box of the coverage (exact, from the tiles it holds).
Rect cov_box(const pixel::TileMap& cov) {
    int x0 = std::numeric_limits<int>::max(), y0 = x0, x1 = -1, y1 = -1;
    for (int row = 0; row < cov.rows(); ++row)
        for (int col = 0; col < cov.cols(); ++col) {
            if (!cov.tile(col, row)) continue;
            const Rect cell = Rect{col * kTile, row * kTile, kTile, kTile}.intersect({0, 0, cov.width(), cov.height()});
            for (int y = cell.y; y < cell.y + cell.h; ++y)
                for (int x = cell.x; x < cell.x + cell.w; ++x)
                    if (cov.get(x, y).c[0] > 0.f) {
                        x0 = std::min(x0, x); y0 = std::min(y0, y);
                        x1 = std::max(x1, x); y1 = std::max(y1, y);
                    }
        }
    if (x1 < 0) return {};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// A W x H x n float buffer blurred in place (each channel apart), sigma.
void blur(std::vector<float>& b, int W, int H, int n, double sigma) {
    for (int y = 0; y < H; ++y)
        for (int c = 0; c < n; ++c)
            filter::blur_line(b.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(W)) * static_cast<std::size_t>(n) + static_cast<std::size_t>(c), W, n, sigma);
    for (int x = 0; x < W; ++x)
        for (int c = 0; c < n; ++c)
            filter::blur_line(b.data() + static_cast<std::size_t>(x) * static_cast<std::size_t>(n) + static_cast<std::size_t>(c), H, W * n, sigma);
}

// What both the heal and the spot search need over a region R: the
// destination's smooth colour with the stroke left out ("what the
// surroundings predict"), from `ref` and the weights 1 - coverage.
struct Region {
    Rect R;
    int cc = 0;                 // colour channels
    std::vector<float> cov;     // R.w * R.h
    std::vector<float> low_d;   // R.w * R.h * cc
};

double sigma_of(double diameter) { return std::max(2.0, 0.75 * diameter); }

int reach_of(double sigma) { return static_cast<int>(std::ceil(3 * sigma)) + 2; }

// `ref` pixel's straight colour and its weight (alpha).
float colour_at(const pixel::TileMap& ref, int x, int y, int cc, float* out) {
    if (x < 0 || y < 0 || x >= ref.width() || y >= ref.height()) {
        x = std::clamp(x, 0, ref.width() - 1);
        y = std::clamp(y, 0, ref.height() - 1);
    }
    const Px p = ref.get(x, y);
    for (int c = 0; c < cc; ++c) out[c] = p.c[static_cast<std::size_t>(c)];
    return ref.format().alpha ? p.c[static_cast<std::size_t>(cc)] : 1.f;
}

// The masked smooth part of `ref` at offset (dx, dy) over R, the weights
// (1 - cov) x alpha. Out: R.w * R.h * cc.
std::vector<float> masked_low(const pixel::TileMap& ref, int dx, int dy, const Region& g, double sigma) {
    const int W = g.R.w, H = g.R.h, cc = g.cc;
    std::vector<float> num(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * static_cast<std::size_t>(cc));
    std::vector<float> den(static_cast<std::size_t>(W) * static_cast<std::size_t>(H));
    std::array<float, pixel::kMaxChannels> c{};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
            const float a = colour_at(ref, g.R.x + x + dx, g.R.y + y + dy, cc, c.data());
            const float w = (1.f - g.cov[i]) * a;
            den[i] = w;
            for (int k = 0; k < cc; ++k) num[i * static_cast<std::size_t>(cc) + static_cast<std::size_t>(k)] = c[static_cast<std::size_t>(k)] * w;
        }
    blur(num, W, H, cc, sigma);
    blur(den, W, H, 1, sigma);
    for (std::size_t i = 0; i < den.size(); ++i)
        for (int k = 0; k < cc; ++k) {
            auto& v = num[i * static_cast<std::size_t>(cc) + static_cast<std::size_t>(k)];
            v = den[i] > 1e-5f ? v / den[i] : 0.f;
        }
    return num;
}

std::optional<Region> region_of(const pixel::TileMap& ref, const pixel::TileMap& cov, double sigma) {
    const auto& f = ref.format();
    if (f.mode != pixel::Mode::RGB && f.mode != pixel::Mode::Gray) return std::nullopt;
    const Rect box = cov_box(cov);
    if (box.empty()) return std::nullopt;
    const int pad = reach_of(sigma);
    Region g;
    g.R = Rect{box.x - pad, box.y - pad, box.w + 2 * pad, box.h + 2 * pad}.intersect({0, 0, ref.width(), ref.height()});
    g.cc = f.colour_channels();
    g.cov.resize(static_cast<std::size_t>(g.R.w) * static_cast<std::size_t>(g.R.h));
    for (int y = 0; y < g.R.h; ++y)
        for (int x = 0; x < g.R.w; ++x)
            g.cov[static_cast<std::size_t>(y) * static_cast<std::size_t>(g.R.w) + static_cast<std::size_t>(x)] =
                std::clamp(cov.get(g.R.x + x, g.R.y + y).c[0], 0.f, 1.f);
    return g;
}
}  // namespace

TileSet covered(const Image& im, const pixel::TileMap& cov) {
    TileSet t = tiles_of(im);
    for (int row = 0; row < std::min(t.rows, cov.rows()); ++row)
        for (int col = 0; col < std::min(t.cols, cov.cols()); ++col)
            if (cov.tile(col, row)) t.add(col, row);
    return t;
}

std::optional<pixel::TileMap> healing_source(const pixel::TileMap& ref, int dx, int dy, const pixel::TileMap& cov,
                                             double diameter, const std::function<bool()>& cancelled) {
    const double sigma = sigma_of(diameter);
    auto g = region_of(ref, cov, sigma);
    if (!g) return ref.format().mode == pixel::Mode::RGB || ref.format().mode == pixel::Mode::Gray
                       ? std::optional<pixel::TileMap>(pixel::TileMap(ref.format(), ref.width(), ref.height()))
                       : std::nullopt;
    if (cancelled && cancelled()) return std::nullopt;
    const auto low_d = masked_low(ref, 0, 0, *g, sigma);
    if (cancelled && cancelled()) return std::nullopt;
    const auto low_s = masked_low(ref, dx, dy, *g, sigma);
    if (cancelled && cancelled()) return std::nullopt;
    const auto& f = ref.format();
    const int cc = g->cc;
    const bool clamp01 = f.depth != pixel::Depth::F32;
    pixel::TileMap out(f, ref.width(), ref.height());
    std::array<float, pixel::kMaxChannels> s{};
    out = out.paint(g->R, [&](int x, int y, const Px& o) {
        const std::size_t i = static_cast<std::size_t>(y - g->R.y) * static_cast<std::size_t>(g->R.w) + static_cast<std::size_t>(x - g->R.x);
        if (g->cov[i] <= 0.f) return o;
        const float a = colour_at(ref, x + dx, y + dy, cc, s.data());
        Px p{};
        for (int k = 0; k < cc; ++k) {
            const std::size_t q = i * static_cast<std::size_t>(cc) + static_cast<std::size_t>(k);
            const float v = s[static_cast<std::size_t>(k)] + (low_d[q] - low_s[q]);
            p.c[static_cast<std::size_t>(k)] = clamp01 ? std::clamp(v, 0.f, 1.f) : std::max(v, 0.f);
        }
        if (f.alpha) p.c[static_cast<std::size_t>(cc)] = a;
        return p;
    });
    return out;
}

std::optional<Offset> spot_offset(const pixel::TileMap& ref, const pixel::TileMap& cov, double diameter) {
    const double sigma = sigma_of(diameter);
    auto g = region_of(ref, cov, sigma);
    if (!g) return std::nullopt;
    const Rect box = cov_box(cov);
    const auto low_d = masked_low(ref, 0, 0, *g, sigma);
    const int cc = g->cc;
    const int gapx = box.w + static_cast<int>(diameter / 2) + 2, gapy = box.h + static_cast<int>(diameter / 2) + 2;
    std::optional<Offset> best;
    double best_e = std::numeric_limits<double>::max();
    std::array<float, pixel::kMaxChannels> s{};
    for (int ky = -1; ky <= 1; ++ky)
        for (int kx = -1; kx <= 1; ++kx) {
            if (!kx && !ky) continue;
            const Offset o{kx * gapx, ky * gapy};
            // The source box must be inside the picture.
            const Rect sb{box.x + o.dx, box.y + o.dy, box.w, box.h};
            if (sb.x < 0 || sb.y < 0 || sb.x + sb.w > ref.width() || sb.y + sb.h > ref.height()) continue;
            double e = 0, wsum = 0;
            for (int y = 0; y < g->R.h; y += 2)
                for (int x = 0; x < g->R.w; x += 2) {
                    const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(g->R.w) + static_cast<std::size_t>(x);
                    const float w = g->cov[i];
                    if (w <= 0.f) continue;
                    colour_at(ref, g->R.x + x + o.dx, g->R.y + y + o.dy, cc, s.data());
                    for (int k = 0; k < cc; ++k) {
                        const double d = s[static_cast<std::size_t>(k)] - low_d[i * static_cast<std::size_t>(cc) + static_cast<std::size_t>(k)];
                        e += w * d * d;
                    }
                    wsum += w;
                }
            if (wsum <= 0) continue;
            e /= wsum;
            if (e < best_e) { best_e = e; best = o; }
        }
    return best;
}

bool stroke(DocJournal& j, Document& d, const Document::Id& id, const std::vector<paint::Dab>& path, paint::Brush b,
            bool spot, int dx, int dy, bool all) {
    if (path.empty() || !paint::refusal(d, id, b).empty()) return false;
    std::string why;
    const auto ref = paint::clone_source(d, id, all, why);
    if (!ref) return false;
    const auto l0 = *d.get(id);
    b.alpha_lock = (l0.locks & lock::Alpha) != 0;
    b.source = nullptr;
    const pixel::TileMap original = l0.pixels ? *l0.pixels : d.blank();
    pixel::TileMap cov = paint::new_coverage(original.width(), original.height());
    double carry = 1e9;
    paint::Jitter jit;   // s028
    for (std::size_t i = 0; i < path.size(); ++i) {
        TileSet touched = tiles_of(d.image());
        cov = paint::add_pen_dabs(cov, b, jit.apply(paint::dabs_along(path[i == 0 ? 0 : i - 1], path[i], b, carry), b), touched);
    }
    if (spot) {
        const auto o = spot_offset(*ref, cov, b.size);
        if (!o) return false;
        dx = o->dx;
        dy = o->dy;
    }
    auto src = healing_source(*ref, dx, dy, cov, b.size);
    if (!src) return false;
    b.source = std::make_shared<const pixel::TileMap>(std::move(*src));
    b.dx = b.dy = 0;
    const auto out = paint::apply(original, original, cov, d.selection(), b, covered(d.image(), cov));
    return layers::set_pixels_if(j, d, id, l0.pixels, out, spot ? "Spot Healing" : "Healing Brush");
}

pixel::TileMap coverage_of(const pixel::TileMap& sel) {
    pixel::TileMap cov = paint::new_coverage(sel.width(), sel.height());
    const Rect e = select::extent(sel).intersect({0, 0, sel.width(), sel.height()});
    if (e.empty()) return cov;
    return cov.paint(e, [&](int x, int y, const Px& o) {
        const float k = select::coverage(sel, x, y);
        if (k <= 0.f) return o;
        Px p{};
        p.c[0] = k;
        return p;
    });
}

std::optional<pixel::TileMap> patched(const Image& im, const pixel::TileMap& original, const pixel::TileMap& ref,
                                      const pixel::TileMap& sel, int dx, int dy, bool alpha_lock,
                                      const std::function<bool()>& cancelled) {
    const auto cov = coverage_of(sel);
    const Rect box = cov_box(cov);
    if (box.empty()) return original;
    // The blur bridges the patch's narrow way across.
    const double across = std::max(8, std::min(box.w, box.h));
    auto healed = healing_source(ref, dx, dy, cov, across, cancelled);
    if (!healed) return std::nullopt;
    paint::Brush b;
    b.opacity = 1;
    b.alpha_lock = alpha_lock;
    b.source = std::make_shared<const pixel::TileMap>(std::move(*healed));
    return paint::apply(original, original, cov, nullptr, b, covered(im, cov));
}

bool patch(DocJournal& j, Document& d, const Document::Id& id, int dx, int dy, bool all) {
    const auto* sel = d.selection();
    if (!sel || (dx == 0 && dy == 0)) return false;
    paint::Brush probe;
    if (!paint::refusal(d, id, probe).empty()) return false;
    std::string why;
    const auto ref = paint::clone_source(d, id, all, why);
    if (!ref) return false;
    const auto l0 = *d.get(id);
    const pixel::TileMap original = l0.pixels ? *l0.pixels : d.blank();
    auto out = patched(d.image(), original, *ref, *sel, dx, dy, (l0.locks & lock::Alpha) != 0);
    if (!out) return false;
    return layers::set_pixels_if(j, d, id, l0.pixels, std::move(*out), "Patch");
}

}  // namespace klimt::core::heal
