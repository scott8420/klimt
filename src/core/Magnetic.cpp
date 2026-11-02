#include "core/Magnetic.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace klimt::core::select {

namespace {
constexpr int kBlock = 64;
double dist(Pt a, Pt b) { return std::hypot(a.x - b.x, a.y - b.y); }
Pt centre(int x, int y) { return {x + 0.5, y + 0.5}; }
}  // namespace

void MagneticTrace::start(Pt p, Lum lum, int width, int height, const Params& prm, double near) {
    *this = {};
    m_lum = std::move(lum);
    m_w = width;
    m_h = height;
    m_p = prm;
    m_p.width = std::clamp(m_p.width, 1.0, 256.0);
    m_p.contrast = std::clamp(m_p.contrast, 0.01, 1.0);
    m_p.frequency = std::clamp(m_p.frequency, 0, 100);
    m_near = near;
    const Pt s = snap(p);
    m_fixed = {s};
    m_anchor_at = {0};
    m_wire = {s};
    m_tip = s;
    m_state = State::Open;
}

float MagneticTrace::lum(int x, int y) const {
    x = std::clamp(x, 0, std::max(0, m_w - 1));
    y = std::clamp(y, 0, std::max(0, m_h - 1));
    const std::pair<int, int> key{x / kBlock, y / kBlock};
    auto it = m_lum_cache.find(key);
    if (it == m_lum_cache.end()) {
        if (m_lum_cache.size() > 4096) m_lum_cache.clear();   // ~64 MB: start over
        std::vector<float> b(static_cast<std::size_t>(kBlock) * kBlock, 0.f);
        const int x0 = key.first * kBlock, y0 = key.second * kBlock;
        for (int j = 0; j < kBlock; ++j)
            for (int i = 0; i < kBlock; ++i) {
                const int xx = std::min(x0 + i, m_w - 1), yy = std::min(y0 + j, m_h - 1);
                b[static_cast<std::size_t>(j) * kBlock + static_cast<std::size_t>(i)] = m_lum ? m_lum(xx, yy) : 0.f;
            }
        it = m_lum_cache.emplace(key, std::move(b)).first;
    }
    return it->second[static_cast<std::size_t>(y % kBlock) * kBlock + static_cast<std::size_t>(x % kBlock)];
}

float MagneticTrace::grad(int x, int y) const {
    if (!m_lum) return 0.f;
    const float a = lum(x - 1, y - 1), b = lum(x, y - 1), c = lum(x + 1, y - 1);
    const float d = lum(x - 1, y), f = lum(x + 1, y);
    const float g = lum(x - 1, y + 1), h = lum(x, y + 1), i = lum(x + 1, y + 1);
    const float gx = (c + 2 * f + i) - (a + 2 * d + g), gy = (g + 2 * h + i) - (a + 2 * b + c);
    return std::sqrt(gx * gx + gy * gy);
}

float MagneticTrace::edge(int x, int y) const {
    if (x < 0 || y < 0 || x >= m_w || y >= m_h) return 0.f;
    return std::clamp(grad(x, y) / 4.f, 0.f, 1.f);   // a black / white step is 4
}

Pt MagneticTrace::snap(Pt p) const {
    if (!m_lum || m_w <= 0) return p;
    // The nearest of the strong edges within Width (strong: within 70 % of
    // the strongest there) -- along an even rim, the bit under the pointer.
    const int cx = static_cast<int>(std::floor(p.x)), cy = static_cast<int>(std::floor(p.y));
    const int r = static_cast<int>(std::ceil(m_p.width));
    const int x0 = std::max(0, cx - r), x1 = std::min(m_w - 1, cx + r), y0 = std::max(0, cy - r), y1 = std::min(m_h - 1, cy + r);
    float top = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (std::hypot(x - cx, y - cy) <= m_p.width) top = std::max(top, grad(x, y));
    if (top < 0.08f) return p;   // a hair of an edge isn't one
    Pt at = p;
    double best_d = 1e9;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double d = std::hypot(x - cx, y - cy);
            if (d <= m_p.width && d < best_d && grad(x, y) >= 0.7f * top) { best_d = d; at = centre(x, y); }
        }
    return at;
}

std::vector<Pt> MagneticTrace::path(Pt a, Pt b) const {
    const int ax = std::clamp(static_cast<int>(std::floor(a.x)), 0, std::max(0, m_w - 1));
    const int ay = std::clamp(static_cast<int>(std::floor(a.y)), 0, std::max(0, m_h - 1));
    const int bx = std::clamp(static_cast<int>(std::floor(b.x)), 0, std::max(0, m_w - 1));
    const int by = std::clamp(static_cast<int>(std::floor(b.y)), 0, std::max(0, m_h - 1));
    if (ax == bx && ay == by) return {centre(ax, ay)};
    if (!m_lum) {   // straight
        const int n = std::max(std::abs(bx - ax), std::abs(by - ay));
        std::vector<Pt> out;
        for (int i = 0; i <= n; ++i) out.push_back(centre(static_cast<int>(std::lround(ax + (bx - ax) * double(i) / n)),
                                                          static_cast<int>(std::lround(ay + (by - ay) * double(i) / n))));
        return out;
    }
    // The window: the two, and a margin for the edge to wander.
    const int m = static_cast<int>(std::clamp(m_p.width * 2, 16.0, 64.0));
    const int x0 = std::max(0, std::min(ax, bx) - m), y0 = std::max(0, std::min(ay, by) - m);
    const int x1 = std::min(m_w - 1, std::max(ax, bx) + m), y1 = std::min(m_h - 1, std::max(ay, by) + m);
    const int W = x1 - x0 + 1, H = y1 - y0 + 1;
    const auto N = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    std::vector<float> cost(N);
    float gmax = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float g = grad(x0 + x, y0 + y);
            cost[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)] = g;
            gmax = std::max(gmax, g);
        }
    const float thr = static_cast<float>(m_p.contrast) * gmax;
    for (auto& c : cost) c = (gmax > 0.05f && c >= thr) ? 0.03f + 0.97f * (1 - c / gmax) : 1.f;
    // Dijkstra, 8-connected.
    std::vector<float> d(N, std::numeric_limits<float>::max());
    std::vector<std::int32_t> from(N, -1);
    using Q = std::pair<float, std::int32_t>;
    std::priority_queue<Q, std::vector<Q>, std::greater<>> q;
    const auto idx = [&](int x, int y) { return static_cast<std::int32_t>((y - y0) * W + (x - x0)); };
    const std::int32_t s = idx(ax, ay), t = idx(bx, by);
    d[static_cast<std::size_t>(s)] = 0;
    q.push({0.f, s});
    static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!q.empty()) {
        const auto [dv, u] = q.top();
        q.pop();
        if (dv > d[static_cast<std::size_t>(u)]) continue;
        if (u == t) break;
        const int ux = u % W, uy = u / W;
        for (int k = 0; k < 8; ++k) {
            const int vx = ux + dx[k], vy = uy + dy[k];
            if (vx < 0 || vy < 0 || vx >= W || vy >= H) continue;
            const auto v = static_cast<std::int32_t>(vy * W + vx);
            const float step = cost[static_cast<std::size_t>(v)] * (k < 4 ? 1.f : 1.41421356f);
            const float nd = dv + step;
            if (nd < d[static_cast<std::size_t>(v)]) {
                d[static_cast<std::size_t>(v)] = nd;
                from[static_cast<std::size_t>(v)] = u;
                q.push({nd, v});
            }
        }
    }
    std::vector<Pt> out;
    for (std::int32_t u = t; u >= 0; u = from[static_cast<std::size_t>(u)]) {
        out.push_back(centre(x0 + u % W, y0 + u / W));
        if (u == s) break;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

double MagneticTrace::spacing() const { return 12 + (100 - m_p.frequency) * 0.9; }   // 57 -> ~51 px

void MagneticTrace::lay_auto() {
    // While the wire is long, an anchor part way along it.
    const auto k = static_cast<std::size_t>(spacing());
    for (int guard = 0; guard < 64 && m_wire.size() > k + k / 2; ++guard) {
        m_fixed.insert(m_fixed.end(), m_wire.begin() + 1, m_wire.begin() + static_cast<std::ptrdiff_t>(k) + 1);
        m_anchor_at.push_back(m_fixed.size() - 1);
        m_wire = path(m_fixed.back(), m_tip);
    }
}

void MagneticTrace::move(Pt p) {
    if (m_state != State::Open) return;
    m_tip = snap(p);
    m_wire = path(m_fixed.back(), m_tip);
    lay_auto();
}

void MagneticTrace::click(Pt p) {
    if (m_state != State::Open) return;
    if (m_fixed.size() + m_wire.size() > 3 && dist(p, m_fixed.front()) <= m_near) { finish(); return; }
    m_tip = snap(p);
    m_wire = path(m_fixed.back(), m_tip);
    if (m_wire.size() < 2) return;   // on the last anchor: nothing to lay
    m_fixed.insert(m_fixed.end(), m_wire.begin() + 1, m_wire.end());
    m_anchor_at.push_back(m_fixed.size() - 1);
    m_wire = {m_fixed.back()};
}

bool MagneticTrace::back() {
    if (m_state != State::Open || m_anchor_at.size() <= 1) return false;
    m_anchor_at.pop_back();
    m_fixed.resize(m_anchor_at.back() + 1);
    m_wire = path(m_fixed.back(), m_tip);
    return true;
}

void MagneticTrace::finish() {
    if (m_state != State::Open) return;
    // What the wire held, then the wire to the start.
    if (m_wire.size() > 1) m_fixed.insert(m_fixed.end(), m_wire.begin() + 1, m_wire.end());
    const auto home = path(m_fixed.back(), m_fixed.front());
    if (home.size() > 2) m_fixed.insert(m_fixed.end(), home.begin() + 1, home.end() - 1);
    m_wire.clear();
    m_state = m_fixed.size() >= 3 ? State::Done : State::Cancelled;
}

void MagneticTrace::cancel() {
    m_state = State::Cancelled;
    m_fixed.clear();
    m_wire.clear();
    m_anchor_at.clear();
}

std::vector<Pt> MagneticTrace::points() const {
    std::vector<Pt> out = m_fixed;
    if (m_wire.size() > 1) out.insert(out.end(), m_wire.begin() + 1, m_wire.end());
    return out;
}

std::vector<Pt> MagneticTrace::anchors() const {
    std::vector<Pt> out;
    for (const auto i : m_anchor_at) if (i < m_fixed.size()) out.push_back(m_fixed[i]);
    return out;
}

}  // namespace klimt::core::select
