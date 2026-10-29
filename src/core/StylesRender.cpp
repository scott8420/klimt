// core::styles -- drawing the layer styles (s019). See core/Styles.hpp.
#include "core/StylesRender.hpp"
#include "core/Patterns.hpp"   // s028 m4
#include "core/Composite.hpp"   // from_srgb
#include "core/Filter.hpp"      // blur_line
#include "pixel/Blend.hpp"     // s025

#include <algorithm>
#include <cmath>
#include <limits>

namespace klimt::core::styles {

namespace {
using pixel::kTile;
using pixel::Rect;

double sigma_of(int size) { return std::max(0.0, size / 2.0); }
int blur_reach(int size) { return size > 0 ? static_cast<int>(std::ceil(3 * sigma_of(size))) + 1 : 0; }

void blur(std::vector<float>& a, int W, int H, double sigma) {
    if (sigma <= 0.05) return;
    for (int y = 0; y < H; ++y) filter::blur_line(a.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(W), W, 1, sigma);
    for (int x = 0; x < W; ++x) filter::blur_line(a.data() + x, H, W, sigma);
}

// One dimension of the squared distance transform (Felzenszwalb &
// Huttenlocher): d[q] = min over p of (q - p)^2 + f[p].
void edt1(const double* f, int n, double* d, int* v, double* z) {
    constexpr double kInf = 1e30;
    int k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (int q = 1; q < n; ++q) {
        const auto cross = [&](int p) { return ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * q - 2.0 * p); };
        double s = cross(v[k]);
        while (s <= z[k]) { --k; s = cross(v[k]); }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        d[q] = double(q - v[k]) * double(q - v[k]) + f[v[k]];
    }
}
}  // namespace

std::vector<float> distance(const std::vector<char>& seed, int w, int h) {
    constexpr double kFar = 1e20;   // no seed yet (stays far when there is none at all)
    std::vector<double> g(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
    for (std::size_t i = 0; i < g.size(); ++i) g[i] = seed[i] ? 0.0 : kFar;
    const int n = std::max(w, h);
    std::vector<double> f(static_cast<std::size_t>(n)), d(static_cast<std::size_t>(n)), z(static_cast<std::size_t>(n) + 1);
    std::vector<int> v(static_cast<std::size_t>(n));
    for (int x = 0; x < w; ++x) {   // columns
        for (int y = 0; y < h; ++y) f[static_cast<std::size_t>(y)] = g[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
        edt1(f.data(), h, d.data(), v.data(), z.data());
        for (int y = 0; y < h; ++y) g[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] = d[static_cast<std::size_t>(y)];
    }
    std::vector<float> out(g.size());
    for (int y = 0; y < h; ++y) {   // rows
        const std::size_t r = static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        std::copy(g.begin() + static_cast<std::ptrdiff_t>(r), g.begin() + static_cast<std::ptrdiff_t>(r + static_cast<std::size_t>(w)), f.begin());
        edt1(f.data(), w, d.data(), v.data(), z.data());
        for (int x = 0; x < w; ++x) out[r + static_cast<std::size_t>(x)] = static_cast<float>(std::sqrt(d[static_cast<std::size_t>(x)]));
    }
    return out;
}

int reach(const Styles& s) {
    int r = 0;
    if (s.drop.on) r = std::max(r, s.drop.distance + blur_reach(s.drop.size) + s.drop.size);
    if (s.outer.on) r = std::max(r, blur_reach(s.outer.size) + s.outer.size);
    if (s.stroke.on) r = std::max(r, s.stroke.size + 2);
    if (s.inner.on) r = std::max(r, s.inner.distance + blur_reach(s.inner.size) + 1);
    if (s.bevel.on) r = std::max(r, blur_reach(s.bevel.size) + blur_reach(s.bevel.soften) + 2);   // s021
    if (s.inner_glow.on) r = std::max(r, blur_reach(s.inner_glow.size) + 1);   // s023
    if (s.satin.on) r = std::max(r, s.satin.distance + blur_reach(s.satin.size) + 1);   // s026
    return r;
}

pixel::TileMap drawn(const pixel::TileMap& content, const Styles& s, int fill) {
    const auto& fmt = content.format();
    if (!fmt.alpha || !s.any()) return content;
    const int C = fmt.channels(), cc = fmt.colour_channels();
    const int IW = content.width(), IH = content.height();
    // The content's extent (its held tiles), and the region the effects can reach.
    int x0 = IW, y0 = IH, x1 = 0, y1 = 0;
    for (int row = 0; row < content.rows(); ++row)
        for (int col = 0; col < content.cols(); ++col)
            if (content.tile(col, row)) {
                x0 = std::min(x0, col * kTile); y0 = std::min(y0, row * kTile);
                x1 = std::max(x1, std::min(IW, (col + 1) * kTile)); y1 = std::max(y1, std::min(IH, (row + 1) * kTile));
            }
    if (x1 <= x0 || y1 <= y0) return content;
    const int pad = reach(s);
    const Rect R = Rect{x0 - pad, y0 - pad, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad}.intersect({0, 0, IW, IH});
    const int W = R.w, H = R.h;
    const std::size_t N = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    // The content, premultiplied, and its alpha.
    std::vector<float> P(N * static_cast<std::size_t>(C), 0.f), A(N, 0.f);
    {
        std::vector<float> t;
        for (int tr = R.y / kTile; tr <= (R.y + R.h - 1) / kTile; ++tr)
            for (int tc = R.x / kTile; tc <= (R.x + R.w - 1) / kTile; ++tc) {
                const auto tile = content.tile(tc, tr);
                if (!tile) continue;
                tile->premultiplied(t);
                const Rect cell = Rect{tc * kTile, tr * kTile, kTile, kTile}.intersect(R);
                for (int y = cell.y; y < cell.y + cell.h; ++y)
                    for (int x = cell.x; x < cell.x + cell.w; ++x) {
                        const float* from = t.data() + (static_cast<std::size_t>(y - tr * kTile) * kTile + static_cast<std::size_t>(x - tc * kTile)) * static_cast<std::size_t>(C);
                        const std::size_t i = static_cast<std::size_t>(y - R.y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x - R.x);
                        std::copy(from, from + C, P.data() + i * static_cast<std::size_t>(C));
                        A[i] = from[cc];
                    }
            }
    }
    const auto at = [&](const std::vector<float>& b, int x, int y) {
        return (x < 0 || y < 0 || x >= W || y >= H) ? 0.f : b[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)];
    };
    const auto shifted = [&](int dx, int dy) {   // A seen from (x - dx, y - dy)
        std::vector<float> o(N, 0.f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) o[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)] = at(A, x - dx, y - dy);
        return o;
    };
    const auto offset_of = [](const ShadowFx& f, int& dx, int& dy) {   // away from the light
        const double a = f.angle * M_PI / 180;
        dx = static_cast<int>(std::lround(-std::cos(a) * f.distance));
        dy = static_cast<int>(std::lround(std::sin(a) * f.distance));
    };
    const auto spread_up = [](std::vector<float>& m, int spread) {
        const float k = 1.f / std::max(0.02f, 1.f - static_cast<float>(std::clamp(spread, 0, 100)) / 100.f);
        for (auto& v : m) v = std::min(1.f, v * k);
    };
    // The result, premultiplied: effects under, the content, effects over.
    std::vector<float> O(N * static_cast<std::size_t>(C), 0.f);
    const auto colour_of = [&](const Rgb& c) { return from_srgb(fmt, c.r, c.g, c.b); };
    // s025: an effect's BLEND MODE -- RGB with alpha (the compositor's own
    // layout), else Normal. `mix` a straight colour into a straight colour;
    // `over_mode` a flat colour at alpha a onto the result so far (O).
    const bool modes = fmt.mode == pixel::Mode::RGB && C == 4;
    const auto mix = [&](float* col, const pixel::Px& fx, float t, pixel::Blend mode, std::size_t i) {
        if (t <= 0.f) return;
        if (!modes || mode == pixel::Blend::Normal) {
            for (int k = 0; k < cc; ++k) col[k] += (fx.c[static_cast<std::size_t>(k)] - col[k]) * t;
            return;
        }
        float src[4] = {fx.c[0] * t, fx.c[1] * t, fx.c[2] * t, t};
        float dst[4] = {col[0], col[1], col[2], 1.f};
        const int x = R.x + static_cast<int>(i % static_cast<std::size_t>(W)), y = R.y + static_cast<int>(i / static_cast<std::size_t>(W));
        pixel::blend_span(mode, src, dst, 1, 1.f, x, y);
        for (int k = 0; k < 3; ++k) col[k] = dst[k];
    };
    const auto over = [&](std::size_t i, const pixel::Px& col, float a) {   // a flat colour at alpha a, over
        if (a <= 0.f) return;
        float* o = O.data() + i * static_cast<std::size_t>(C);
        for (int k = 0; k < cc; ++k) o[k] = col.c[static_cast<std::size_t>(k)] * a + o[k] * (1 - a);
        o[cc] = a + o[cc] * (1 - a);
    };
    const auto over_mode = [&](std::size_t i, const pixel::Px& col, float a, pixel::Blend mode) {
        if (a <= 0.f) return;
        if (!modes || mode == pixel::Blend::Normal) { over(i, col, a); return; }
        float src[4] = {col.c[0] * a, col.c[1] * a, col.c[2] * a, a};
        const int x = R.x + static_cast<int>(i % static_cast<std::size_t>(W)), y = R.y + static_cast<int>(i / static_cast<std::size_t>(W));
        pixel::blend_span(mode, src, O.data() + i * 4, 1, 1.f, x, y);
    };
    if (s.drop.on) {
        int dx = 0, dy = 0;
        offset_of(s.drop, dx, dy);
        auto m = shifted(dx, dy);
        blur(m, W, H, sigma_of(s.drop.size));
        spread_up(m, s.drop.spread);
        const auto col = colour_of(s.drop.colour);
        const float op = static_cast<float>(s.drop.opacity) / 100.f;
        // Photoshop's "Layer Knocks Out Drop Shadow" (on by default): none
        // under the content itself -- so a fill of 0 leaves the shadow
        // around the shape, not inside it.
        for (std::size_t i = 0; i < N; ++i) over(i, col, m[i] * op * (1 - A[i]));
    }
    if (s.outer.on) {
        auto m = A;
        blur(m, W, H, sigma_of(s.outer.size));
        spread_up(m, std::max(s.outer.spread, 50));   // a glow reaches its edge at full strength
        const auto col = colour_of(s.outer.colour);
        const float op = static_cast<float>(s.outer.opacity) / 100.f;
        for (std::size_t i = 0; i < N; ++i) over(i, col, m[i] * op);
    }
    // The content, at its fill -- with Color Overlay and Inner Shadow on it.
    const float fl = static_cast<float>(std::clamp(fill, 0, 100)) / 100.f;
    std::vector<float> inner;
    if (s.inner.on) {   // where the shifted inverse falls inside the content
        int dx = 0, dy = 0;
        offset_of(s.inner, dx, dy);
        inner.assign(N, 0.f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) inner[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)] = 1.f - at(A, x - dx, y - dy);
        blur(inner, W, H, sigma_of(s.inner.size));
        spread_up(inner, s.inner.spread);
    }
    // s023: Inner Glow -- the outside softened into the content (Edge), or
    // the inside softened from it (Center: the glow from the middle out).
    std::vector<float> iglow;
    if (s.inner_glow.on) {
        iglow.assign(N, 0.f);
        for (std::size_t i = 0; i < N; ++i) iglow[i] = 1.f - A[i];
        blur(iglow, W, H, sigma_of(s.inner_glow.size));
        spread_up(iglow, std::max(s.inner_glow.choke, 50));   // at full strength at the edge, as the outer glow
        if (s.inner_glow.center) for (auto& v : iglow) v = 1.f - v;
    }
    // s025: Gradient Overlay -- across the content's own box (alpha > 0).
    std::vector<float> grad_t;
    pixel::Px g_from{}, g_to{};
    if (s.gradient.on) {
        int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (A[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)] > 0.f) {
                    bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y);
                }
        if (bx1 >= bx0) {
            const auto& gf = s.gradient;
            const double cx = (bx0 + bx1) / 2.0, cy = (by0 + by1) / 2.0;
            const double hw = (bx1 - bx0 + 1) / 2.0, hh = (by1 - by0 + 1) / 2.0;
            const double sc = std::clamp(gf.scale, 10, 150) / 100.0;
            const double a = gf.angle * M_PI / 180;
            const double ux = std::cos(a), uy = -std::sin(a);   // image y runs down: 90 points up
            const double half = std::max(1.0, std::abs(ux) * hw + std::abs(uy) * hh) * sc;   // the box's extent along it
            const double radius = std::max(1.0, std::sqrt(hw * hw + hh * hh)) * sc;
            grad_t.assign(N, 0.f);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const double dx = x - cx, dy = y - cy;
                    double t = gf.style == 1 ? std::sqrt(dx * dx + dy * dy) / radius : (dx * ux + dy * uy) / (2 * half) + 0.5;
                    t = std::clamp(t, 0.0, 1.0);
                    if (gf.reverse) t = 1 - t;
                    grad_t[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)] = static_cast<float>(t);
                }
            g_from = colour_of(gf.from);
            g_to = colour_of(gf.to);
        }
    }
    const float gro = static_cast<float>(s.gradient.opacity) / 100.f;
    // s026: Satin -- |A shifted one way - A shifted the other|, softened.
    std::vector<float> satin;
    if (s.satin.on) {
        const double a = s.satin.angle * M_PI / 180;
        const int dx = static_cast<int>(std::lround(std::cos(a) * s.satin.distance));
        const int dy = static_cast<int>(std::lround(-std::sin(a) * s.satin.distance));
        satin.assign(N, 0.f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                satin[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)] =
                    std::abs(at(A, x - dx, y - dy) - at(A, x + dx, y + dy));
        blur(satin, W, H, sigma_of(s.satin.size));
        if (s.satin.invert) for (auto& v : satin) v = 1.f - std::clamp(v, 0.f, 1.f);
    }
    // s028 m4: Pattern Overlay -- the pattern's grey at each pixel, as the
    // document's colour (256 levels, made once).
    std::vector<std::uint8_t> pat;
    std::vector<pixel::Px> pat_lut;
    std::vector<pixel::Px> upat;      // s033: YOUR pattern -- each pixel's colour, its alpha in upat_a
    std::vector<float> upat_a;
    if (s.pattern.on && s.pattern.image) {
        const auto& pf = s.pattern;
        upat.assign(N, pixel::Px{});
        upat_a.assign(N, 0.f);
        float px[4];
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
                if (A[i] <= 0.f) continue;
                pattern::sample(*pf.image, R.x + x, R.y + y, std::clamp(pf.scale, 10, 1000) / 100.0, pf.x, pf.y, px);
                upat[i] = colour_of(Rgb{px[0], px[1], px[2]});
                upat_a[i] = px[3];
            }
    } else if (s.pattern.on) {
        const auto& pf = s.pattern;
        pat.assign(N, 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
                if (A[i] <= 0.f) continue;
                const float g = pattern::grey(pf.pattern, R.x + x, R.y + y, std::clamp(pf.scale, 10, 1000) / 100.0, pf.x, pf.y);
                pat[i] = static_cast<std::uint8_t>(std::lround(std::clamp(g, 0.f, 1.f) * 255));
            }
        pat_lut.resize(256);
        for (int k = 0; k < 256; ++k) pat_lut[static_cast<std::size_t>(k)] = colour_of(Rgb{k / 255.f, k / 255.f, k / 255.f});
    }
    const float pao = static_cast<float>(s.pattern.opacity) / 100.f;
    const auto sac = colour_of(s.satin.colour);
    const float sao = static_cast<float>(s.satin.opacity) / 100.f;
    const auto igc = colour_of(s.inner_glow.colour);
    const float igo = static_cast<float>(s.inner_glow.opacity) / 100.f;
    const auto ov = colour_of(s.overlay.colour), ish = colour_of(s.inner.colour);
    const float ovo = static_cast<float>(s.overlay.opacity) / 100.f, iso = static_cast<float>(s.inner.opacity) / 100.f;
    for (std::size_t i = 0; i < N; ++i) {
        const float a = A[i];
        if (a <= 0.f) continue;
        // The content's own straight colour, overlaid, inner-shadowed.
        float col[pixel::kMaxChannels];
        for (int k = 0; k < cc; ++k) col[k] = P[i * static_cast<std::size_t>(C) + static_cast<std::size_t>(k)] / a;
        if (!satin.empty())   // s026: under the overlays (Photoshop's order)
            mix(col, sac, std::clamp(satin[i], 0.f, 1.f) * sao, s.satin.mode, i);
        if (!pat.empty())   // s028 m4: under the Gradient and Color Overlays (Photoshop's order)
            mix(col, pat_lut[pat[i]], pao, s.pattern.mode, i);
        else if (!upat.empty() && upat_a[i] > 0.f)   // s033: yours, through its own alpha
            mix(col, upat[i], pao * upat_a[i], s.pattern.mode, i);
        if (!grad_t.empty()) {   // s025: under Color Overlay (Photoshop's order)
            pixel::Px gc{};
            for (int k = 0; k < cc; ++k)
                gc.c[static_cast<std::size_t>(k)] = g_from.c[static_cast<std::size_t>(k)] + (g_to.c[static_cast<std::size_t>(k)] - g_from.c[static_cast<std::size_t>(k)]) * grad_t[i];
            mix(col, gc, gro, s.gradient.mode, i);
        }
        if (s.overlay.on)
            mix(col, ov, ovo, s.overlay.mode, i);
        if (s.inner.on) {
            const float t = inner[i] * iso;
            mix(col, ish, t, s.inner.mode, i);
        }
        if (s.inner_glow.on) {   // s023
            const float t = std::clamp(iglow[i], 0.f, 1.f) * igo;
            mix(col, igc, t, s.inner_glow.mode, i);
        }
        const float ca = a * fl;
        float* o = O.data() + i * static_cast<std::size_t>(C);
        for (int k = 0; k < cc; ++k) o[k] = col[k] * ca + o[k] * (1 - ca);
        o[cc] = ca + o[cc] * (1 - ca);
    }
    // s021: Bevel & Emboss -- the content's alpha, softened over `size`, as a
    // height field; its slope lit from (angle, altitude). Over the content,
    // under the Stroke.
    if (s.bevel.on) {
        const auto& b = s.bevel;
        std::vector<float> h = A;
        const double sig = std::max(0.5, b.size / 2.0);
        blur(h, W, H, sig);
        // Slopes ~1 at a depth of 100 %: a blurred edge rises 1 over ~2.5 sigma.
        const float k = static_cast<float>((b.up ? 1.0 : -1.0) * std::clamp(b.depth, 1, 1000) / 100.0 * 2.5 * sig);
        const double ang = b.angle * M_PI / 180, alt = std::clamp(b.altitude, 0, 90) * M_PI / 180;
        const float lx = static_cast<float>(std::cos(alt) * std::cos(ang)), ly = static_cast<float>(-std::cos(alt) * std::sin(ang));
        const float lz = static_cast<float>(std::sin(alt));
        std::vector<float> hi(N, 0.f), lo(N, 0.f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
                const float gx = (at(h, x + 1, y) - at(h, x - 1, y)) * 0.5f * k, gy = (at(h, x, y + 1) - at(h, x, y - 1)) * 0.5f * k;
                if (gx == 0.f && gy == 0.f) continue;
                const float nl = std::sqrt(gx * gx + gy * gy + 1.f);
                const float shade = (-gx * lx - gy * ly + lz) / nl;   // the normal (-gx, -gy, 1) . the light
                const float dlt = shade - lz;
                if (dlt > 0) hi[i] = std::min(1.f, dlt / std::max(0.05f, 1.f - lz));
                else lo[i] = std::min(1.f, -dlt / std::max(0.05f, lz));
            }
        if (b.soften > 0) { blur(hi, W, H, b.soften / 2.0); blur(lo, W, H, b.soften / 2.0); }
        const auto hc = colour_of(b.highlight), sc = colour_of(b.shadow);
        const float ho = static_cast<float>(b.highlight_opacity) / 100.f, so = static_cast<float>(b.shadow_opacity) / 100.f;
        for (std::size_t i = 0; i < N; ++i) {
            const float where = b.style == 1 ? 1.f : A[i];   // Inner Bevel: only on the content
            over_mode(i, sc, lo[i] * so * where, b.shadow_mode);
            over_mode(i, hc, hi[i] * ho * where, b.highlight_mode);
        }
    }
    if (s.stroke.on) {   // exact distances to the edge (alpha >= 1/2), anti-aliased
        std::vector<char> in(N), out(N);
        for (std::size_t i = 0; i < N; ++i) { in[i] = A[i] >= 0.5f; out[i] = !in[i]; }
        const auto d_out = distance(in, W, H);    // from outside to the shape
        const auto d_in = distance(out, W, H);    // from inside to the outside
        const float sz = static_cast<float>(std::max(1, s.stroke.size));
        const auto col = colour_of(s.stroke.colour);
        const float op = static_cast<float>(s.stroke.opacity) / 100.f;
        for (std::size_t i = 0; i < N; ++i) {
            float a = 0;
            switch (s.stroke.position) {
                // d runs centre to centre; the edge lies half a pixel inside
                // it, so a pixel is covered while d - 1/2 < size (anti-aliased).
                case 1:  a = in[i] ? std::clamp(sz + 1.f - d_in[i], 0.f, 1.f) : 0.f; break;   // inside
                case 2:  a = std::clamp(sz / 2 + 1.f - (in[i] ? d_in[i] : d_out[i]), 0.f, 1.f); break;   // centre
                default: a = in[i] ? 0.f : std::clamp(sz + 1.f - d_out[i], 0.f, 1.f); break;   // outside
            }
            over_mode(i, col, a * op, s.stroke.mode);
        }
    }
    // Back to tiles: every tile the region touches, made afresh (the content
    // outside the region is none -- the region holds all of it).
    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(content.cols()) * static_cast<std::size_t>(content.rows()));
    for (int row = 0; row < content.rows(); ++row)
        for (int col = 0; col < content.cols(); ++col)
            grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(content.cols()) + static_cast<std::size_t>(col)] = content.tile(col, row);
    for (int tr = R.y / kTile; tr <= (R.y + R.h - 1) / kTile; ++tr)
        for (int tc = R.x / kTile; tc <= (R.x + R.w - 1) / kTile; ++tc) {
            const Rect cell = Rect{tc * kTile, tr * kTile, kTile, kTile}.intersect(R);
            pixel::TileWriter w(fmt, pixel::transparent());
            bool any = false;
            for (int y = cell.y; y < cell.y + cell.h; ++y)
                for (int x = cell.x; x < cell.x + cell.w; ++x) {
                    const float* o = O.data() + (static_cast<std::size_t>(y - R.y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x - R.x)) * static_cast<std::size_t>(C);
                    if (o[cc] <= 0.f) continue;
                    pixel::Px p{};
                    for (int k = 0; k < cc; ++k) p.c[static_cast<std::size_t>(k)] = std::clamp(o[k] / o[cc], 0.f, 1.f);
                    p.c[static_cast<std::size_t>(cc)] = std::min(1.f, o[cc]);
                    w.set(x - tc * kTile, y - tr * kTile, p);
                    any = true;
                }
            grid[static_cast<std::size_t>(tr) * static_cast<std::size_t>(content.cols()) + static_cast<std::size_t>(tc)] = any ? w.freeze() : nullptr;
        }
    return content.with_grid(std::move(grid));
}

}  // namespace klimt::core::styles
