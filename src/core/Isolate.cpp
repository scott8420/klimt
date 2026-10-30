// core::isolate -- the path as a hint; GrabCut decides the band (s035 m7).
// docs/paths.md.
#include "core/Isolate.hpp"
#include "core/Matte.hpp"   // s036: the soft edge
#include "core/MaxFlow.hpp"
#include "core/Paths.hpp"
#include "core/Selection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>

namespace klimt::core::isolate {

namespace {
using Vec = std::array<double, 3>;

// A mixture of K full-covariance Gaussians over RGB.
struct Gmm {
    static constexpr int K = 5;
    struct Comp { double w = 0; Vec mu{}; double inv[3][3]{}; double logdet = 0; };
    std::array<Comp, K> c;
    int used = 0;

    static double dot3(const double m[3][3], const Vec& d) {
        double s = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) s += d[static_cast<std::size_t>(i)] * m[i][j] * d[static_cast<std::size_t>(j)];
        return s;
    }
    // -log of the likelihood (up to a constant shared by both models).
    double cost(const Vec& z) const {
        double best = 0;
        for (int k = 0; k < used; ++k) {
            const auto& q = c[static_cast<std::size_t>(k)];
            const Vec d{z[0] - q.mu[0], z[1] - q.mu[1], z[2] - q.mu[2]};
            best += q.w * std::exp(-0.5 * dot3(q.inv, d) - 0.5 * q.logdet);
        }
        return -std::log(best + 1e-300);
    }
    int nearest(const Vec& z) const {
        int bk = 0;
        double bv = -1e300;
        for (int k = 0; k < used; ++k) {
            const auto& q = c[static_cast<std::size_t>(k)];
            const Vec d{z[0] - q.mu[0], z[1] - q.mu[1], z[2] - q.mu[2]};
            const double v = std::log(q.w + 1e-300) - 0.5 * dot3(q.inv, d) - 0.5 * q.logdet;
            if (v > bv) { bv = v; bk = k; }
        }
        return bk;
    }
    // Learn from samples given each a component.
    void fit(const std::vector<Vec>& s, const std::vector<int>& comp) {
        std::array<double, K> n{};
        std::array<Vec, K> sum{};
        std::array<std::array<double, 9>, K> prod{};
        for (std::size_t i = 0; i < s.size(); ++i) {
            const auto k = static_cast<std::size_t>(comp[i]);
            n[k] += 1;
            for (int a = 0; a < 3; ++a) {
                sum[k][static_cast<std::size_t>(a)] += s[i][static_cast<std::size_t>(a)];
                for (int b = 0; b < 3; ++b) prod[k][static_cast<std::size_t>(a * 3 + b)] += s[i][static_cast<std::size_t>(a)] * s[i][static_cast<std::size_t>(b)];
            }
        }
        used = 0;
        const double total = static_cast<double>(s.size());
        for (int k = 0; k < K; ++k) {
            const auto kk = static_cast<std::size_t>(k);
            if (n[kk] < 2) continue;
            Comp q;
            q.w = n[kk] / total;
            for (int a = 0; a < 3; ++a) q.mu[static_cast<std::size_t>(a)] = sum[kk][static_cast<std::size_t>(a)] / n[kk];
            double cov[3][3];
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    cov[a][b] = prod[kk][static_cast<std::size_t>(a * 3 + b)] / n[kk] - q.mu[static_cast<std::size_t>(a)] * q.mu[static_cast<std::size_t>(b)] +
                                (a == b ? 1e-4 : 0);
            const double det = cov[0][0] * (cov[1][1] * cov[2][2] - cov[1][2] * cov[2][1]) -
                               cov[0][1] * (cov[1][0] * cov[2][2] - cov[1][2] * cov[2][0]) +
                               cov[0][2] * (cov[1][0] * cov[2][1] - cov[1][1] * cov[2][0]);
            if (!(det > 1e-18)) continue;
            q.logdet = std::log(det);
            q.inv[0][0] = (cov[1][1] * cov[2][2] - cov[1][2] * cov[2][1]) / det;
            q.inv[0][1] = (cov[0][2] * cov[2][1] - cov[0][1] * cov[2][2]) / det;
            q.inv[0][2] = (cov[0][1] * cov[1][2] - cov[0][2] * cov[1][1]) / det;
            q.inv[1][0] = (cov[1][2] * cov[2][0] - cov[1][0] * cov[2][2]) / det;
            q.inv[1][1] = (cov[0][0] * cov[2][2] - cov[0][2] * cov[2][0]) / det;
            q.inv[1][2] = (cov[0][2] * cov[1][0] - cov[0][0] * cov[1][2]) / det;
            q.inv[2][0] = (cov[1][0] * cov[2][1] - cov[1][1] * cov[2][0]) / det;
            q.inv[2][1] = (cov[0][1] * cov[2][0] - cov[0][0] * cov[2][1]) / det;
            q.inv[2][2] = (cov[0][0] * cov[1][1] - cov[0][1] * cov[1][0]) / det;
            c[static_cast<std::size_t>(used++)] = q;
        }
        double ws = 0;
        for (int k = 0; k < used; ++k) ws += c[static_cast<std::size_t>(k)].w;
        for (int k = 0; k < used; ++k) c[static_cast<std::size_t>(k)].w /= ws;
    }
    // A first model: k-means (a few rounds) seeds the components.
    void init(const std::vector<Vec>& s) {
        if (s.empty()) { used = 0; return; }
        std::array<Vec, K> centre{};
        for (int k = 0; k < K; ++k) centre[static_cast<std::size_t>(k)] = s[s.size() * static_cast<std::size_t>(k) / K + s.size() / (2 * K)];
        std::vector<int> comp(s.size(), 0);
        for (int it = 0; it < 6; ++it) {
            for (std::size_t i = 0; i < s.size(); ++i) {
                double bd = 1e300;
                for (int k = 0; k < K; ++k) {
                    const auto& m = centre[static_cast<std::size_t>(k)];
                    const double d = (s[i][0] - m[0]) * (s[i][0] - m[0]) + (s[i][1] - m[1]) * (s[i][1] - m[1]) + (s[i][2] - m[2]) * (s[i][2] - m[2]);
                    if (d < bd) { bd = d; comp[i] = k; }
                }
            }
            std::array<Vec, K> sum{};
            std::array<double, K> n{};
            for (std::size_t i = 0; i < s.size(); ++i) {
                const auto k = static_cast<std::size_t>(comp[i]);
                n[k] += 1;
                for (int a = 0; a < 3; ++a) sum[k][static_cast<std::size_t>(a)] += s[i][static_cast<std::size_t>(a)];
            }
            for (int k = 0; k < K; ++k)
                if (n[static_cast<std::size_t>(k)] > 0)
                    for (int a = 0; a < 3; ++a) centre[static_cast<std::size_t>(k)][static_cast<std::size_t>(a)] = sum[static_cast<std::size_t>(k)][static_cast<std::size_t>(a)] / n[static_cast<std::size_t>(k)];
        }
        fit(s, comp);
        if (used == 0) {   // a flat region: one component, a little spread
            Comp q;
            q.w = 1;
            q.mu = s[0];
            for (int a = 0; a < 3; ++a) q.inv[a][a] = 1e4;
            q.logdet = 3 * std::log(1e-4);
            c[0] = q;
            used = 1;
        }
    }
    // Re-learn: each sample to its likeliest component, then fit.
    void relearn(const std::vector<Vec>& s) {
        if (s.empty() || used == 0) return;
        std::vector<int> comp(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) comp[i] = nearest(s[i]);
        Gmm next = *this;
        next.fit(s, comp);
        if (next.used > 0) *this = next;
    }
};

// Every `step`-th of the pixels in `ids`, at most `cap` of them.
std::vector<Vec> sample(const std::vector<Vec>& col, const std::vector<int>& ids, std::size_t cap) {
    std::vector<Vec> out;
    if (ids.empty()) return out;
    const std::size_t step = std::max<std::size_t>(1, ids.size() / cap);
    for (std::size_t i = 0; i < ids.size(); i += step) out.push_back(col[static_cast<std::size_t>(ids[i])]);
    return out;
}

// s037: the strokes laid down, in order, on a W x H grid: 0 none, 1 figure,
// 2 background (a pixel's centre within a stroke's radius of its polyline).
std::vector<std::uint8_t> strokes_laid(const path::Hints& h, int W, int H) {
    std::vector<std::uint8_t> m;
    if (h.empty() || W <= 0 || H <= 0) return m;
    m.assign(static_cast<std::size_t>(W) * static_cast<std::size_t>(H), 0);
    for (const auto& s : h.strokes) {
        const std::uint8_t v = s.figure ? 1 : 2;
        const double r = std::max(0.5, s.radius), r2 = r * r;
        const std::size_t n = s.pts.size();
        for (std::size_t k = 0; k < n; ++k) {
            const path::P a = s.pts[k], b = s.pts[k + 1 < n ? k + 1 : k];
            if (k + 1 == n && n > 1) break;   // the last point closes the previous segment
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min(a.x, b.x) - r))), x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max(a.x, b.x) + r)));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min(a.y, b.y) - r))), y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max(a.y, b.y) + r)));
            const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const double px = x + 0.5 - a.x, py = y + 0.5 - a.y;
                    const double t = len2 > 0 ? std::clamp((px * dx + py * dy) / len2, 0.0, 1.0) : 0.0;
                    const double ex = px - t * dx, ey = py - t * dy;
                    if (ex * ex + ey * ey <= r2) m[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)] = v;
                }
        }
    }
    return m;
}

// The pixels of `laid` equal to `v`, as a mask (F32 Gray, W x H).
pixel::TileMap laid_map(const std::vector<std::uint8_t>& laid, std::uint8_t v, int W, int H) {
    const pixel::Format ff{pixel::Depth::F32, pixel::Mode::Gray, false};
    pixel::TileMap out(ff, W, H);
    if (laid.empty()) return out;
    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(out.cols()) * static_cast<std::size_t>(out.rows()));
    pixel::Px one;
    one.c[0] = 1.f;
    for (int row = 0; row < out.rows(); ++row)
        for (int c = 0; c < out.cols(); ++c) {
            pixel::TileWriter w(ff, pixel::Px{});
            bool any = false;
            const pixel::Rect cell = pixel::Rect{c * pixel::kTile, row * pixel::kTile, pixel::kTile, pixel::kTile}.intersect({0, 0, W, H});
            for (int y = cell.y; y < cell.y + cell.h; ++y)
                for (int x = cell.x; x < cell.x + cell.w; ++x)
                    if (laid[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)] == v) {
                        w.set(x - cell.x, y - cell.y, one);
                        any = true;
                    }
            if (any) grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(out.cols()) + static_cast<std::size_t>(c)] = w.freeze();
        }
    return pixel::TileMap::from_tiles(ff, W, H, std::move(grid));
}

// One grid: its colours, each cell's role, its labels.
struct Grid {
    int w = 0, h = 0;
    std::vector<Vec> col;
    // 0 background (sure), 1 figure (sure), 2 to decide, 3 outside the work.
    std::vector<std::uint8_t> role;
    std::vector<std::uint8_t> label;   // 1 the figure
    std::vector<std::uint8_t> stroke;  // s037: 0 none, 1 under a figure stroke, 2 a background one (empty: no strokes)
};

// s037: how far each cell is from a stroke of kind `v`, through what the
// stroke touches. s038 (seen under Xvfb, a noisy picture): the first way --
// neighbour to neighbour, exp(beta |dz|^2) a step -- LEAKED: noise sets beta
// low, and a soft (anti-aliased, blurred) edge is many small steps, so one
// stroke reached right across into the background. Now a FLOOD: a cell joins
// only when its colour is like the STROKE's own colours (a handful of
// clusters of what lies under the stroke, each with its spread) and it
// touches one already in. Noise inside a blob is in the clusters' spread; a
// gradual edge is not -- its middle is like neither side. The distance:
// steps from the stroke (8 neighbours, a diagonal sqrt 2). Infinity: not
// reached. Cells outside the work (role 3) are not crossed.
std::vector<float> geodesic(const Grid& g, std::uint8_t v, const std::function<bool()>& stop) {
    const int W = g.w, H = g.h;
    const std::size_t N = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    std::vector<float> d(N, std::numeric_limits<float>::infinity());
    // The stroke's colours: up to 6 clusters (k-means, a few rounds), each
    // with its RMS spread.
    std::vector<int> seeds;
    for (std::size_t i = 0; i < N; ++i) if (g.stroke[i] == v) seeds.push_back(static_cast<int>(i));
    if (seeds.empty()) return d;
    const auto samples = sample(g.col, seeds, 3000);
    const auto dist2 = [](const Vec& a, const Vec& b) {
        return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
    };
    const std::size_t K = std::min<std::size_t>(6, samples.size());
    std::vector<Vec> c(K);
    for (std::size_t k = 0; k < K; ++k) c[k] = samples[k * samples.size() / K];
    std::vector<std::size_t> of(samples.size(), 0);
    for (int round = 0; round < 8; ++round) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            double best = 1e300;
            for (std::size_t k = 0; k < K; ++k) { const double e = dist2(samples[i], c[k]); if (e < best) { best = e; of[i] = k; } }
        }
        std::vector<Vec> sum(K, Vec{});
        std::vector<int> n(K, 0);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            for (int ch = 0; ch < 3; ++ch) sum[of[i]][static_cast<std::size_t>(ch)] += samples[i][static_cast<std::size_t>(ch)];
            ++n[of[i]];
        }
        for (std::size_t k = 0; k < K; ++k)
            if (n[k]) for (int ch = 0; ch < 3; ++ch) c[k][static_cast<std::size_t>(ch)] = sum[k][static_cast<std::size_t>(ch)] / static_cast<float>(n[k]);
    }
    // Each cluster's reach: 2.5 x its RMS spread, at least kFloor (a flat
    // blob still takes its own slight variation).
    constexpr double kFloor = 0.06;
    std::vector<double> t2(K, 0);
    {
        std::vector<double> ss(K, 0);
        std::vector<int> n(K, 0);
        for (std::size_t i = 0; i < samples.size(); ++i) { ss[of[i]] += dist2(samples[i], c[of[i]]); ++n[of[i]]; }
        for (std::size_t k = 0; k < K; ++k) {
            const double r = n[k] ? 2.5 * std::sqrt(ss[k] / n[k]) : 0;
            t2[k] = std::max(r, kFloor) * std::max(r, kFloor);
        }
    }
    // Which clusters each cell is like (bits). A step goes only between
    // cells that share one: a stroke over two colours reaches each colour's
    // own region, never on from one into ground that happens to look like
    // the other (s037's patch: a ground-coloured hole ringed by the figure).
    std::vector<std::uint8_t> like(N, 0);
    for (std::size_t i = 0; i < N; ++i) {
        if (g.role[i] == 3) continue;
        std::uint8_t m = 0;
        for (std::size_t k = 0; k < K; ++k) if (dist2(g.col[i], c[k]) <= t2[k]) m = static_cast<std::uint8_t>(m | (1u << k));
        like[i] = m;
    }
    for (const int i : seeds)   // under the stroke: like whatever it touches (its edge pixels, mixed)
        if (!like[static_cast<std::size_t>(i)]) like[static_cast<std::size_t>(i)] = 0x3F;
    using Q = std::pair<float, int>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
    for (const int i : seeds) { d[static_cast<std::size_t>(i)] = 0; q.push({0.f, i}); }
    const int nx8[8] = {1, -1, 0, 0, 1, 1, -1, -1}, ny8[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    std::size_t popped = 0;
    while (!q.empty()) {
        const auto [di, i] = q.top();
        q.pop();
        if (di > d[static_cast<std::size_t>(i)]) continue;
        if ((++popped & 0xFFFF) == 0 && stop()) return d;
        const int x = i % W, y = i / W;
        for (int k = 0; k < 8; ++k) {
            const int X = x + nx8[k], Y = y + ny8[k];
            if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
            const int j = Y * W + X;
            if (g.role[static_cast<std::size_t>(j)] == 3) continue;
            const float nd = di + (k < 4 ? 1.f : 1.41421356f);
            if (nd >= d[static_cast<std::size_t>(j)]) continue;
            if (!(like[static_cast<std::size_t>(i)] & like[static_cast<std::size_t>(j)])) continue;
            d[static_cast<std::size_t>(j)] = nd;
            q.push({nd, j});
        }
    }
    return d;
}

// GrabCut's rounds on a grid: cut the cells to decide, re-learn, again.
// The models come in learnt and go out re-learnt. false: cancelled.
// s037: `bias` -- per cell, a pull toward the figure (> 0) or the background
// (< 0) from the strokes: made here from g.stroke when it comes in empty (the
// first grid cut), else taken as it is (the fine pass: the coarse one's).
bool cut(Grid& g, Gmm& fg, Gmm& bg, int rounds, const std::function<bool()>& stop, double dist_unit, std::vector<float>& bias) {
    const int W = g.w, H = g.h;
    const std::size_t N = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    std::vector<int> fg_ids, bg_ids, und;
    for (std::size_t i = 0; i < N; ++i) {
        const auto r = g.role[i];
        if (r == 1) fg_ids.push_back(static_cast<int>(i));
        else if (r == 0) bg_ids.push_back(static_cast<int>(i));
        else if (r == 2) und.push_back(static_cast<int>(i));
    }
    if (und.empty()) return true;
    const int dx[4] = {1, 0, 1, -1}, dy[4] = {0, 1, 1, 1};
    const double wdist[4] = {1 / dist_unit, 1 / dist_unit, 1 / (dist_unit * std::sqrt(2.0)), 1 / (dist_unit * std::sqrt(2.0))};
    double sum = 0;
    std::size_t cnt = 0;
    for (const int i : und) {
        const int x = i % W, y = i / W;
        for (int k = 0; k < 4; ++k) {
            const int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            const auto& a = g.col[static_cast<std::size_t>(i)];
            const auto& b = g.col[static_cast<std::size_t>(ny * W + nx)];
            sum += (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
            ++cnt;
        }
    }
    const double beta = 1 / (2 * std::max(sum / std::max<std::size_t>(cnt, 1), 1e-8));
    constexpr double kGamma = 50;
    // s037: a stroke's reach -- through what it touches without an edge (a
    // blob of one colour, the stroke laid across it), fading with distance;
    // strong enough to outweigh the colour models, never sure (the cut still pays for edges).
    if (bias.empty() && !g.stroke.empty()) {
        const auto has = [&](std::uint8_t v) { return std::find(g.stroke.begin(), g.stroke.end(), v) != g.stroke.end(); };
        const double reach = std::max(W, H);
        constexpr double kPull = 12;
        bias.assign(N, 0.f);
        for (const std::uint8_t v : {std::uint8_t{1}, std::uint8_t{2}}) {
            if (!has(v)) continue;
            const auto d = geodesic(g, v, stop);
            if (stop()) return false;
            for (std::size_t i = 0; i < N; ++i) {
                const double p = d[i] < reach ? kPull * (1 - d[i] / reach) : 0;
                bias[i] += static_cast<float>(v == 1 ? p : -p);
            }
        }
    }
    const bool biased = bias.size() == N;
    const auto smooth = [&](int i, int j, int k) {
        const auto& a = g.col[static_cast<std::size_t>(i)];
        const auto& b = g.col[static_cast<std::size_t>(j)];
        const double d2 = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
        return kGamma * wdist[k] * std::exp(-beta * d2);
    };
    std::vector<int> node(N, -1);
    for (std::size_t n = 0; n < und.size(); ++n) node[static_cast<std::size_t>(und[n])] = static_cast<int>(n);
    for (int round = 0; round < rounds; ++round) {
        if (stop()) return false;
        MaxFlow mf;
        mf.reserve(und.size(), und.size() * 4);
        for (std::size_t n = 0; n < und.size(); ++n) mf.add_node();
        for (std::size_t n = 0; n < und.size(); ++n) {
            const int i = und[n];
            const auto& z = g.col[static_cast<std::size_t>(i)];
            // Source = the figure: calling the pixel background costs the background's -log p.
            double to_fg = bg.cost(z), to_bg = fg.cost(z);
            if (biased) {   // s037: the strokes' pull
                const double b = bias[static_cast<std::size_t>(i)];
                if (b > 0) to_fg += b; else to_bg -= b;
            }
            const double lo = std::min(to_fg, to_bg);
            to_fg -= lo;
            to_bg -= lo;
            const int x = i % W, y = i / W;
            for (int k = 0; k < 8; ++k) {   // sure neighbours pull it their way
                const int kk = k % 4, sgn = k < 4 ? 1 : -1;
                const int nx = x + sgn * dx[kk], ny = y + sgn * dy[kk];
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                const int j = ny * W + nx;
                const auto r = g.role[static_cast<std::size_t>(j)];
                if (r == 2) continue;
                const double v = smooth(i, j, kk);
                if (r == 1) to_fg += v; else to_bg += v;   // (3, beyond the work: counts as background)
            }
            mf.add_tweights(static_cast<int>(n), static_cast<float>(to_fg), static_cast<float>(to_bg));
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                const int j = ny * W + nx;
                if (g.role[static_cast<std::size_t>(j)] != 2) continue;
                const auto v = static_cast<float>(smooth(i, j, k));
                mf.add_edge(static_cast<int>(n), node[static_cast<std::size_t>(j)], v, v);
            }
        }
        mf.maxflow(stop);
        if (stop()) return false;
        for (std::size_t n = 0; n < und.size(); ++n) g.label[static_cast<std::size_t>(und[n])] = mf.source_side(static_cast<int>(n)) ? 1 : 0;
        if (round + 1 < rounds) {
            std::vector<int> f = fg_ids, b = bg_ids;
            for (const int i : und) (g.label[static_cast<std::size_t>(i)] ? f : b).push_back(i);
            fg.relearn(sample(g.col, f, 20000));
            bg.relearn(sample(g.col, b, 20000));
        }
    }
    return true;
}
}  // namespace

std::optional<pixel::TileMap> isolated(const pixel::TileMap& picture, const path::Path& p, const pixel::Format& mf, const Options& o,
                                       const std::function<bool()>& cancelled, const std::function<void(double)>& progress) {
    const pixel::Format ff{pixel::Depth::F32, pixel::Mode::Gray, false};
    return isolated(picture, paths::coverage(p, ff, picture.width(), picture.height()), mf, o, cancelled, progress);
}

std::optional<pixel::TileMap> isolated(const pixel::TileMap& picture, const pixel::TileMap& area, const pixel::Format& mf, const Options& o,
                                       const std::function<bool()>& cancelled, const std::function<void(double)>& progress) {
    const int W = picture.width(), H = picture.height();
    if (area.width() != W || area.height() != H) return std::nullopt;
    const auto stop = [&] { return cancelled && cancelled(); };
    const auto tell = [&](double f) { if (progress) progress(f); };
    const double band = std::clamp(o.band, 2.0, 64.0);
    const pixel::Format ff{pixel::Depth::F32, pixel::Mode::Gray, false};
    // The path's area; grown by the band (beyond it: surely background);
    // a ring beyond that (the background's colours).
    // s037: the strokes, laid down; a figure stroke widens the path's area.
    const auto laid = o.hints ? strokes_laid(*o.hints, W, H) : std::vector<std::uint8_t>{};
    const auto hint = [&](int x, int y) -> std::uint8_t {
        return laid.empty() ? 0 : laid[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)];
    };
    auto inside = area;
    const auto fig = laid_map(laid, 1, W, H);
    if (!select::empty(fig)) inside = select::combine(&inside, fig, select::Op::Add);
    if (select::empty(inside)) return std::nullopt;
    auto outer = select::modified(inside, select::Modify::Expand, band, cancelled);
    // s038: a figure stroke says "this thing" -- not how far it goes. It looks
    // kStrokeReach round itself (seen under Xvfb: with only the band, the sure
    // ring ran THROUGH the stroked blob, its colour taught as background, and
    // the cut flipped the rough area's background to figure).
    if (outer && !select::empty(fig)) {
        const auto wide = select::modified(fig, select::Modify::Expand, std::max(band, kStrokeReach), cancelled);
        if (!wide) return std::nullopt;
        outer = select::combine(&*outer, *wide, select::Op::Add);
    }
    const double ring_w = std::max(16.0, 2 * band);
    const auto ring = outer ? select::modified(*outer, select::Modify::Expand, ring_w, cancelled) : std::nullopt;
    if (!outer || !ring || stop()) return std::nullopt;
    tell(0.05);
    const pixel::Rect R = select::bounds(*ring).intersect({0, 0, W, H});
    if (R.empty()) return std::nullopt;
    // Full resolution: the colours, the roles. Inside the path + the band:
    // to decide (a path's corners can be far from the figure -- only its
    // outside is sure). The figure's first model: the path's inside.
    const std::size_t N = static_cast<std::size_t>(R.w) * static_cast<std::size_t>(R.h);
    std::vector<Vec> col(N);
    std::vector<std::uint8_t> role(N, 3), in_path(N, 0);
    std::size_t undecided = 0;
    for (int y = R.y; y < R.y + R.h; ++y) {
        if ((y & 63) == 0 && stop()) return std::nullopt;
        for (int x = R.x; x < R.x + R.w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y - R.y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x - R.x);
            std::uint8_t r = 3;
            const std::uint8_t h = hint(x, y);   // s037: a stroke is sure
            if (h == 1) r = 1;
            else if (h == 2 && select::coverage(*ring, x, y) >= 0.5f) r = 0;
            else if (select::coverage(*outer, x, y) >= 0.5f) { r = 2; ++undecided; }
            else if (select::coverage(*ring, x, y) >= 0.5f) r = 0;
            role[i] = r;
            if (r == 3) continue;
            in_path[i] = r == 1 || (r == 2 && select::coverage(inside, x, y) >= 0.5f);
            const auto px = picture.get(x, y);
            col[i] = {px.c[0], px.c[1], px.c[2]};
        }
    }
    tell(0.15);
    // The first models: the figure from the path's inside, the background from the ring.
    Gmm fg, bg;
    {
        std::vector<int> f, b;
        for (std::size_t i = 0; i < N; ++i) {
            if (role[i] == 0) b.push_back(static_cast<int>(i));
            else if (role[i] == 1 || (role[i] == 2 && in_path[i])) f.push_back(static_cast<int>(i));
        }
        if (f.empty()) return std::nullopt;
        fg.init(sample(col, f, 20000));
        bg.init(sample(col, b, 20000));
    }
    std::vector<std::uint8_t> label(N, 0);
    for (std::size_t i = 0; i < N; ++i) label[i] = role[i] == 1 || (role[i] == 2 && in_path[i]);
    // COARSE first when there is much to decide: a reduced grid (cells of s x s,
    // their mean colour), at most ~300k cells to decide.
    const int s = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(undecided) / 300000.0))));
    int fine_strip = 0;   // the full-resolution strip either side of the coarse edge
    std::vector<float> coarse_bias;   // s037: the strokes' pull on the coarse grid
    if (s > 1) {
        Grid g;
        g.w = (R.w + s - 1) / s;
        g.h = (R.h + s - 1) / s;
        const std::size_t M = static_cast<std::size_t>(g.w) * static_cast<std::size_t>(g.h);
        g.col.assign(M, Vec{});
        g.role.assign(M, 3);
        g.label.assign(M, 0);
        std::vector<int> n_und(M, 0), n_bg(M, 0), n_all(M, 0), n_in(M, 0), n_fg(M, 0);
        for (int y = 0; y < R.h; ++y)
            for (int x = 0; x < R.w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x);
                const std::size_t c = static_cast<std::size_t>(y / s) * static_cast<std::size_t>(g.w) + static_cast<std::size_t>(x / s);
                if (role[i] == 3) continue;
                ++n_all[c];
                for (int k = 0; k < 3; ++k) g.col[c][static_cast<std::size_t>(k)] += col[i][static_cast<std::size_t>(k)];
                if (role[i] == 2) ++n_und[c];
                else if (role[i] == 1) ++n_fg[c];   // s037: a figure stroke
                else ++n_bg[c];
                n_in[c] += in_path[i];
            }
        for (std::size_t c = 0; c < M; ++c) {
            if (!n_all[c]) continue;
            for (int k = 0; k < 3; ++k) g.col[c][static_cast<std::size_t>(k)] /= n_all[c];
            // s037: a cell mostly under a stroke is as sure as the stroke.
            g.role[c] = n_fg[c] * 2 >= n_all[c] ? 1 : (n_und[c] || n_fg[c]) ? 2 : 0;
            g.label[c] = g.role[c] == 1 || n_in[c] * 2 > n_all[c];
        }
        if (!laid.empty()) {   // s037: a cell is under a stroke when any of it is (the geodesic's sources)
            std::vector<int> n_s1(M, 0), n_s2(M, 0);
            for (int y = 0; y < R.h; ++y)
                for (int x = 0; x < R.w; ++x) {
                    const std::uint8_t h = hint(x + R.x, y + R.y);
                    if (!h) continue;
                    const std::size_t c = static_cast<std::size_t>(y / s) * static_cast<std::size_t>(g.w) + static_cast<std::size_t>(x / s);
                    ++(h == 1 ? n_s1 : n_s2)[c];
                }
            g.stroke.assign(M, 0);
            for (std::size_t c = 0; c < M; ++c)
                if (n_all[c]) g.stroke[c] = n_s1[c] && n_s1[c] >= n_s2[c] ? 1 : n_s2[c] ? 2 : 0;
        }
        if (!cut(g, fg, bg, std::max(1, o.rounds), stop, s, coarse_bias)) return std::nullopt;
        tell(0.6);
        // Back to full resolution: the coarse labels; the strip along their edge decided again.
        for (std::size_t i = 0; i < N; ++i) {
            if (role[i] != 2) continue;
            const int y = static_cast<int>(i / static_cast<std::size_t>(R.w)), x = static_cast<int>(i % static_cast<std::size_t>(R.w));
            label[i] = g.label[static_cast<std::size_t>(y / s) * static_cast<std::size_t>(g.w) + static_cast<std::size_t>(x / s)];
        }
        fine_strip = s + 2;
    }
    // FINE: full resolution. After a coarse pass only a strip along its edge
    // is decided again (the rest is sure now); else everything to decide.
    Grid g;
    g.w = R.w;
    g.h = R.h;
    g.col = std::move(col);
    g.role = role;
    g.label = label;
    if (fine_strip > 0) {
        // A cell to decide stays so only within fine_strip of a label change.
        std::vector<int> d(N, 1 << 20);   // city-block distance to the nearest edge, two passes
        const auto edge = [&](int x, int y) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x);
            if (role[i] != 2) return false;
            const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& n : nb) {
                const int nx = x + n[0], ny = y + n[1];
                if (nx < 0 || ny < 0 || nx >= R.w || ny >= R.h) continue;
                const std::size_t j = static_cast<std::size_t>(ny) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(nx);
                const bool lj = role[j] == 2 ? label[j] != 0 : role[j] == 1;   // s037: a figure stroke is figure
                if (lj != (label[i] != 0)) return true;
            }
            return false;
        };
        for (int y = 0; y < R.h; ++y)
            for (int x = 0; x < R.w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x);
                if (edge(x, y)) { d[i] = 0; continue; }
                if (x > 0) d[i] = std::min(d[i], d[i - 1] + 1);
                if (y > 0) d[i] = std::min(d[i], d[i - static_cast<std::size_t>(R.w)] + 1);
            }
        for (int y = R.h - 1; y >= 0; --y)
            for (int x = R.w - 1; x >= 0; --x) {
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x);
                if (x + 1 < R.w) d[i] = std::min(d[i], d[i + 1] + 1);
                if (y + 1 < R.h) d[i] = std::min(d[i], d[i + static_cast<std::size_t>(R.w)] + 1);
            }
        for (std::size_t i = 0; i < N; ++i)
            if (g.role[i] == 2 && d[i] > fine_strip) g.role[i] = label[i] ? 1 : 0;
        // The models, re-learnt at full resolution from the coarse answer.
        std::vector<int> f, b;
        for (std::size_t i = 0; i < N; ++i) {
            if (g.role[i] == 3) continue;
            ((g.role[i] == 1 || (g.role[i] == 2 && label[i])) ? f : b).push_back(static_cast<int>(i));
        }
        fg.relearn(sample(g.col, f, 20000));
        bg.relearn(sample(g.col, b, 20000));
    }
    std::vector<float> fine_bias;   // s037: the coarse pull, cell by cell; or made here (no coarse pass)
    if (!laid.empty()) {
        g.stroke.assign(N, 0);
        for (int y = 0; y < R.h; ++y)
            for (int x = 0; x < R.w; ++x) g.stroke[static_cast<std::size_t>(y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x)] = hint(x + R.x, y + R.y);
        if (fine_strip > 0 && !coarse_bias.empty()) {
            const int cw = (R.w + s - 1) / s;
            fine_bias.resize(N);
            for (int y = 0; y < R.h; ++y)
                for (int x = 0; x < R.w; ++x)
                    fine_bias[static_cast<std::size_t>(y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x)] =
                        coarse_bias[static_cast<std::size_t>(y / s) * static_cast<std::size_t>(cw) + static_cast<std::size_t>(x / s)];
        }
    }
    if (!cut(g, fg, bg, fine_strip > 0 ? 1 : std::max(1, o.rounds), stop, 1, fine_bias)) return std::nullopt;
    for (std::size_t i = 0; i < N; ++i) label[i] = g.role[i] == 1 || (g.role[i] == 2 && g.label[i]);
    tell(0.9);
    const auto idx = [&](int x, int y) { return static_cast<std::size_t>(y - R.y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x - R.x); };
    // The mask: the sure figure and the band's figure, tile by tile; then a
    // soft edge (a blur of 0.6 px -- anti-aliasing, not a feather).
    pixel::TileMap hard(ff, W, H);
    {
        std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(hard.cols()) * static_cast<std::size_t>(hard.rows()));
        for (int row = R.y / pixel::kTile; row <= (R.y + R.h - 1) / pixel::kTile; ++row)
            for (int c = R.x / pixel::kTile; c <= (R.x + R.w - 1) / pixel::kTile; ++c) {
                pixel::TileWriter w(ff, pixel::Px{});
                bool any = false;
                const pixel::Rect cell = pixel::Rect{c * pixel::kTile, row * pixel::kTile, pixel::kTile, pixel::kTile}.intersect(R);
                pixel::Px one;
                one.c[0] = 1.f;
                for (int y = cell.y; y < cell.y + cell.h; ++y)
                    for (int x = cell.x; x < cell.x + cell.w; ++x)
                        if (label[idx(x, y)]) { w.set(x - c * pixel::kTile, y - row * pixel::kTile, one); any = true; }
                if (any) grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(hard.cols()) + static_cast<std::size_t>(c)] = w.freeze();
            }
        hard = pixel::TileMap::from_tiles(ff, W, H, std::move(grid));
    }
    auto soft = select::feathered(hard, 0.6, cancelled);
    if (!soft) return std::nullopt;
    // To the selection's format.
    pixel::TileMap out(mf, W, H);
    {
        std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(out.cols()) * static_cast<std::size_t>(out.rows()));
        for (int row = 0; row < out.rows(); ++row)
            for (int c = 0; c < out.cols(); ++c) {
                const auto t = soft->tile(c, row);
                if (!t) continue;
                pixel::TileWriter w(mf, pixel::Px{});
                const pixel::Rect cell = pixel::Rect{c * pixel::kTile, row * pixel::kTile, pixel::kTile, pixel::kTile}.intersect({0, 0, W, H});
                for (int y = 0; y < cell.h; ++y)
                    for (int x = 0; x < cell.w; ++x) w.set(x, y, t->get(x, y));
                grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(out.cols()) + static_cast<std::size_t>(c)] = w.freeze();
            }
        out = pixel::TileMap::from_tiles(mf, W, H, std::move(grid));
    }
    // s036: the soft edge -- hair, fur: coverage from colour in a band.
    if (o.soft > 0) {
        auto sm = matte::soft_edge(picture, out, std::clamp(o.soft, 1.0, 64.0), cancelled);
        if (!sm) return std::nullopt;
        out = std::move(*sm);
    }
    tell(1.0);
    return out;
}

}  // namespace klimt::core::isolate
