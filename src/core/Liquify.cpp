#include "core/Liquify.hpp"

#include <algorithm>
#include <cmath>

namespace klimt::core::liquify {

const char* tool_name(Tool t) {
    switch (t) {
        case Tool::Forward:     return "Forward Warp";
        case Tool::Reconstruct: return "Reconstruct";
        case Tool::Twirl:       return "Twirl";
        case Tool::Pucker:      return "Pucker";
        case Tool::Bloat:       return "Bloat";
    }
    return "";
}

Field::Field(int width, int height, int step)
    : m_w(std::max(0, width)), m_h(std::max(0, height)), m_step(std::max(1, step)) {
    if (!m_w || !m_h) return;
    m_gw = (m_w + m_step - 1) / m_step + 1;
    m_gh = (m_h + m_step - 1) / m_step + 1;
    m_dx.assign(static_cast<std::size_t>(m_gw) * static_cast<std::size_t>(m_gh), 0.f);
    m_dy = m_dx;
}

void Field::clear() {
    std::fill(m_dx.begin(), m_dx.end(), 0.f);
    std::fill(m_dy.begin(), m_dy.end(), 0.f);
    m_touched = {};
}

Pt Field::at(double x, double y) const {
    if (!m_gw) return {0, 0};
    const double fx = x / m_step, fy = y / m_step;
    if (fx < 0 || fy < 0 || fx > m_gw - 1 || fy > m_gh - 1) return {0, 0};
    const int i0 = std::min(static_cast<int>(fx), m_gw - 2), j0 = std::min(static_cast<int>(fy), m_gh - 2);
    const double tx = fx - i0, ty = fy - j0;
    const auto k = [&](int i, int j) { return static_cast<std::size_t>(j) * static_cast<std::size_t>(m_gw) + static_cast<std::size_t>(i); };
    const auto lerp = [&](const std::vector<float>& v) {
        const double a = v[k(i0, j0)] * (1 - tx) + v[k(i0 + 1, j0)] * tx;
        const double b = v[k(i0, j0 + 1)] * (1 - tx) + v[k(i0 + 1, j0 + 1)] * tx;
        return a * (1 - ty) + b * ty;
    };
    return {lerp(m_dx), lerp(m_dy)};
}

bool Field::identity() const {
    for (std::size_t i = 0; i < m_dx.size(); ++i)
        if (std::abs(m_dx[i]) > 1e-4f || std::abs(m_dy[i]) > 1e-4f) return false;
    return true;
}

void Field::touch(const pixel::Rect& r) {
    const pixel::Rect c = r.intersect({0, 0, m_w, m_h});
    if (c.empty()) return;
    if (m_touched.empty()) { m_touched = c; return; }
    const int x0 = std::min(m_touched.x, c.x), y0 = std::min(m_touched.y, c.y);
    const int x1 = std::max(m_touched.x + m_touched.w, c.x + c.w), y1 = std::max(m_touched.y + m_touched.h, c.y + c.h);
    m_touched = {x0, y0, x1 - x0, y1 - y0};
}

void Field::dab(const Brush& b, Pt c, Pt motion, bool reverse) {
    if (!m_gw || b.radius <= 0) return;
    const double r = b.radius, p = std::clamp(b.pressure, 0.0, 1.0), rate = std::clamp(b.rate, 0.0, 1.0);
    const int i0 = std::max(0, static_cast<int>(std::floor((c.x - r) / m_step))), i1 = std::min(m_gw - 1, static_cast<int>(std::ceil((c.x + r) / m_step)));
    const int j0 = std::max(0, static_cast<int>(std::floor((c.y - r) / m_step))), j1 = std::min(m_gh - 1, static_cast<int>(std::ceil((c.y + r) / m_step)));
    if (i0 > i1 || j0 > j1) return;
    struct Up { std::size_t k; float dx, dy; };
    std::vector<Up> ups;
    Tool tool = b.tool;
    if (reverse && tool == Tool::Pucker) tool = Tool::Bloat;
    else if (reverse && tool == Tool::Bloat) tool = Tool::Pucker;
    const double turn = (reverse ? -1 : 1) * rate * 0.25;   // radians a full-strength dab turns at the middle
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const Pt n{double(i) * m_step, double(j) * m_step};
            const double d = std::hypot(n.x - c.x, n.y - c.y);
            if (d >= r) continue;
            const double q = 1 - (d / r) * (d / r);
            const double w = q * q * p;   // smooth to nothing at the rim
            if (w <= 0) continue;
            const auto k = static_cast<std::size_t>(j) * static_cast<std::size_t>(m_gw) + static_cast<std::size_t>(i);
            if (tool == Tool::Reconstruct) {
                const auto f = static_cast<float>(1 - std::min(1.0, w * rate * 2));
                ups.push_back({k, m_dx[k] * f, m_dy[k] * f});
                continue;
            }
            Pt to = n;   // where n looks now, before the old field
            switch (tool) {
                case Tool::Forward: to = {n.x - w * motion.x, n.y - w * motion.y}; break;
                case Tool::Twirl: {
                    const double a = -turn * w, cs = std::cos(a), sn = std::sin(a);
                    to = {c.x + cs * (n.x - c.x) - sn * (n.y - c.y), c.y + sn * (n.x - c.x) + cs * (n.y - c.y)};
                    break;
                }
                case Tool::Pucker: { const double s = 1 + 0.1 * rate * w; to = {c.x + (n.x - c.x) * s, c.y + (n.y - c.y) * s}; break; }
                case Tool::Bloat:  { const double s = 1 - 0.1 * rate * w; to = {c.x + (n.x - c.x) * s, c.y + (n.y - c.y) * s}; break; }
                case Tool::Reconstruct: break;
            }
            const Pt old = at(to.x, to.y);
            ups.push_back({k, static_cast<float>(old.x + to.x - n.x), static_cast<float>(old.y + to.y - n.y)});
        }
    for (const auto& u : ups) { m_dx[u.k] = u.dx; m_dy[u.k] = u.dy; }
    const int pad = m_step + 2;
    touch({static_cast<int>(std::floor(c.x - r)) - pad, static_cast<int>(std::floor(c.y - r)) - pad,
           static_cast<int>(std::ceil(2 * r)) + 2 * pad, static_cast<int>(std::ceil(2 * r)) + 2 * pad});
}

std::optional<pixel::TileMap> apply(const pixel::TileMap& src, const Field& f, xform::Quality q,
                                    const std::function<bool()>& cancelled) {
    const pixel::Rect image{0, 0, src.width(), src.height()};
    const pixel::Rect land = f.touched().intersect(image);
    if (land.empty()) return src;
    // What the resample rewrites (the land and its 2 px of slack), cleared
    // first: every pixel there is drawn afresh, nothing laid over itself.
    const pixel::Rect redo = pixel::Rect{land.x - 2, land.y - 2, land.w + 4, land.h + 4}.intersect(image);
    const pixel::TileMap base = src.fill(redo, pixel::transparent());
    const double w = src.width(), h = src.height();
    const std::function<bool(int, int, Pt&)> source = [&](int x, int y, Pt& sp) {
        if (x < redo.x || y < redo.y || x >= redo.x + redo.w || y >= redo.y + redo.h) return false;
        const Pt c{x + 0.5, y + 0.5};
        const Pt d = f.at(c.x, c.y);
        sp = {std::clamp(c.x + d.x, 0.5, w - 0.5), std::clamp(c.y + d.y, 0.5, h - 0.5)};   // the edge stretches, never empties
        return true;
    };
    return xform::over_mapped(base, src, image, land, source, q, cancelled);
}

std::string refusal(const Document& d, const Document::Id& id) {
    const auto l = d.get(id);
    if (!l) return "pick a layer";
    if (l->kind == Kind::Text) return "type stays type -- Layer > Rasterize Type to liquify it";
    if (l->kind != Kind::Pixel) return "pick a pixel layer";
    if (!l->pixels) return "the layer is empty";
    if ((l->locks & (lock::Pixels | lock::All)) || d.locked_all(id)) return "the layer's pixels are locked";
    return {};
}

bool liquify(DocJournal& j, Document& d, const Document::Id& id, const Field& f) {
    if (!refusal(d, id).empty() || f.identity()) return false;
    const auto l = d.get(id);
    auto out = apply(*l->pixels, f, xform::Quality::Best);
    if (!out) return false;
    auto px = std::make_shared<const pixel::TileMap>(std::move(*out));
    return j.run("Liquify", {id}, [&] {
        auto nl = *d.get(id);
        nl.pixels = px;
        d.put(id, nl);
    });
}

}  // namespace klimt::core::liquify
