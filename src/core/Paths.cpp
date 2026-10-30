// core::paths -- the path verbs, a path's coverage, a selection traced into
// a path (s035). docs/paths.md.
#include "core/Paths.hpp"
#include "core/File.hpp"      // m7: the picture isolate reads
#include "core/Isolate.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace klimt::core::paths {

namespace {
using path::P;

}  // namespace

pixel::TileMap coverage(const path::Path& p, const pixel::Format& mf, int w, int h) {
    std::vector<std::vector<select::Pt>> polys;
    for (const auto& poly : path::flatten(p, 0.1)) {
        std::vector<select::Pt> v;
        v.reserve(poly.size());
        for (const P& q : poly) v.push_back({q.x, q.y});
        polys.push_back(std::move(v));
    }
    return select::polygons_mask(mf, w, h, polys, true);
}

std::optional<path::Path> traced(const pixel::TileMap& mask, double tolerance, const std::function<bool()>& cancelled) {
    path::Path out;
    const pixel::Rect b = select::bounds(mask);
    if (b.w <= 0 || b.h <= 0) return out;
    // The grid: pixel centres from one before the bounds to one after (zero
    // there), so every edge closes.
    const int gx0 = b.x - 1, gy0 = b.y - 1, GW = b.w + 2, GH = b.h + 2;
    std::vector<float> g(static_cast<std::size_t>(GW) * static_cast<std::size_t>(GH), 0.f);
    for (int y = 0; y < b.h; ++y) {
        if ((y & 63) == 0 && cancelled && cancelled()) return std::nullopt;
        for (int x = 0; x < b.w; ++x)
            g[static_cast<std::size_t>(y + 1) * static_cast<std::size_t>(GW) + static_cast<std::size_t>(x + 1)] = select::coverage(mask, b.x + x, b.y + y);
    }
    const auto G = [&](int i, int j) { return g[static_cast<std::size_t>(j) * static_cast<std::size_t>(GW) + static_cast<std::size_t>(i)]; };
    // Edge ids: horizontal (i, j)-(i+1, j) = 2 k, vertical (i, j)-(i, j+1) = 2 k + 1.
    using Key = long long;
    const auto H = [&](int i, int j) { return (static_cast<Key>(j) * GW + i) * 2; };
    const auto V = [&](int i, int j) { return (static_cast<Key>(j) * GW + i) * 2 + 1; };
    const auto where = [&](Key e) {
        const Key k = e / 2;
        const int i = static_cast<int>(k % GW), j = static_cast<int>(k / GW);
        const bool horiz = (e & 1) == 0;
        const float a = G(i, j), c = horiz ? G(i + 1, j) : G(i, j + 1);
        const double t = std::abs(c - a) > 1e-9f ? std::clamp((0.5 - a) / (c - a), 0.0, 1.0) : 0.5;
        return horiz ? P{gx0 + i + 0.5 + t, gy0 + j + 0.5} : P{gx0 + i + 0.5, gy0 + j + 0.5 + t};
    };
    struct Ends { Key a = -1, b = -1; };
    std::unordered_map<Key, Ends> adj;
    const auto link = [&](Key x, Key y) {
        auto& ex = adj[x];
        (ex.a < 0 ? ex.a : ex.b) = y;
        auto& ey = adj[y];
        (ey.a < 0 ? ey.a : ey.b) = x;
    };
    for (int j = 0; j + 1 < GH; ++j) {
        if ((j & 63) == 0 && cancelled && cancelled()) return std::nullopt;
        for (int i = 0; i + 1 < GW; ++i) {
            const bool a = G(i, j) >= 0.5f, bb = G(i + 1, j) >= 0.5f, c = G(i + 1, j + 1) >= 0.5f, dd = G(i, j + 1) >= 0.5f;
            const int k = (a ? 1 : 0) | (bb ? 2 : 0) | (c ? 4 : 0) | (dd ? 8 : 0);
            if (k == 0 || k == 15) continue;
            const Key T = H(i, j), B = H(i, j + 1), L = V(i, j), R = V(i + 1, j);
            const bool centre = (G(i, j) + G(i + 1, j) + G(i + 1, j + 1) + G(i, j + 1)) / 4 >= 0.5f;
            switch (k) {
                case 1: case 14: link(T, L); break;
                case 2: case 13: link(T, R); break;
                case 3: case 12: link(L, R); break;
                case 4: case 11: link(R, B); break;
                case 6: case 9:  link(T, B); break;
                case 7: case 8:  link(L, B); break;
                case 5:   // a and c inside
                    if (centre) { link(T, R); link(B, L); } else { link(T, L); link(R, B); }
                    break;
                case 10:  // b and d inside
                    if (centre) { link(T, L); link(R, B); } else { link(T, R); link(L, B); }
                    break;
                default: break;
            }
        }
    }
    // Walk the loops.
    std::unordered_map<Key, bool> seen;
    for (const auto& [start, ends] : adj) {
        if (seen.count(start)) continue;
        std::vector<P> loop;
        Key prev = -1, cur = start;
        while (cur >= 0 && !seen.count(cur)) {
            seen[cur] = true;
            loop.push_back(where(cur));
            const Ends& e = adj[cur];
            const Key next = (e.a != prev) ? e.a : e.b;
            prev = cur;
            cur = next;
        }
        if (loop.size() < 4) continue;
        double area = 0;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const P& p0 = loop[i];
            const P& p1 = loop[(i + 1) % loop.size()];
            area += p0.x * p1.y - p1.x * p0.y;
        }
        if (std::abs(area) / 2 < 1.0) continue;   // a speck
        out.subs.push_back(path::fit(loop, std::max(0.1, tolerance)));
        if (cancelled && cancelled()) return std::nullopt;
    }
    // A stable order: top-left first (unordered_map's order isn't).
    std::sort(out.subs.begin(), out.subs.end(), [](const path::Sub& x, const path::Sub& y) {
        const auto bx = path::box({{x}}), by = path::box({{y}});
        return bx.y0 != by.y0 ? bx.y0 < by.y0 : bx.x0 < by.x0;
    });
    return out;
}

std::shared_ptr<const path::Path> of(const Document& d, const Document::Id& id) {
    const auto l = d.get(id);
    return l && l->kind == Kind::Path ? l->path : nullptr;
}

bool to_selection(DocJournal& j, Document& d, const Document::Id& id, select::Op op, double feather) {
    const auto p = of(d, id);
    if (!p || p->empty()) return false;
    const Image& im = d.image();
    auto shape = coverage(*p, d.mask_format(), im.width, im.height);
    if (feather > 0) {
        auto f = select::feathered(shape, feather);
        if (!f) return false;
        shape = std::move(*f);
    }
    auto m = select::combine(d.selection(), shape, op);
    const std::string label = op == select::Op::Replace ? "Load Path as Selection" : std::string(select::op_name(op)) + " (Path)";
    return select::set(j, d, select::empty(m) ? std::nullopt : std::optional<pixel::TileMap>(std::move(m)), label);
}

Document::Id from_selection(DocJournal& j, Document& d, double tolerance, const Document::Id& above) {
    const auto* sel = d.selection();
    if (!sel) return {};
    auto p = traced(*sel, tolerance);
    if (!p || p->empty()) return {};
    return layers::add_path_layer(j, d, *p, above, "Make Path from Selection");
}

std::optional<pixel::TileMap> picture_for_isolate(const Image& im, const std::map<Document::Id, Layer>& layers) {
    auto ls = layers;
    for (auto& [id, l] : ls) if (l.kind == Kind::Path) l.path_mask = false;   // the hint reads the picture whole
    return file::flatten(im, ls);
}

bool isolate(DocJournal& j, Document& d, const Document::Id& id, double band, select::Op op) {
    const auto p = of(d, id);
    if (!p || p->empty()) return false;
    const auto pic = picture_for_isolate(d.image(), d.snapshot());
    if (!pic) return false;
    isolate::Options o;
    o.band = band;
    auto m = isolate::isolated(*pic, *p, d.mask_format(), o);
    if (!m) return false;
    auto c = select::combine(d.selection(), *m, op);
    return select::set(j, d, select::empty(c) ? std::nullopt : std::optional<pixel::TileMap>(std::move(c)), "Isolate (Path)");
}

std::vector<Document::Id> masked_by(const std::map<Document::Id, Layer>& ls, const Document::Id& path) {
    const auto it = ls.find(path);
    if (it == ls.end() || it->second.kind != Kind::Path) return {};
    const Layer& l = it->second;
    std::vector<Document::Id> below;
    for (const auto& [oid, o] : ls)
        if (oid != path && o.parent == l.parent && o.pos < l.pos && o.kind != Kind::Path) below.push_back(oid);
    std::sort(below.begin(), below.end(), [&](const Document::Id& a, const Document::Id& b) { return ls.at(a).pos > ls.at(b).pos; });
    return below;
}

path::Path rect_path(double x0, double y0, double x1, double y1) {
    path::Sub s;
    s.closed = true;
    for (const P q : {P{x0, y0}, P{x1, y0}, P{x1, y1}, P{x0, y1}}) s.nodes.push_back(path::Node::corner(q));
    return {{s}};
}

path::Path ellipse_path(double cx, double cy, double rx, double ry) {
    constexpr double k = 0.5522847498;   // a quarter circle's handle, as a fraction of the radius
    path::Sub s;
    s.closed = true;
    s.nodes = {
        {{cx + rx, cy}, {cx + rx, cy - k * ry}, {cx + rx, cy + k * ry}, true},
        {{cx, cy + ry}, {cx + k * rx, cy + ry}, {cx - k * rx, cy + ry}, true},
        {{cx - rx, cy}, {cx - rx, cy + k * ry}, {cx - rx, cy - k * ry}, true},
        {{cx, cy - ry}, {cx - k * rx, cy - ry}, {cx + k * rx, cy - ry}, true},
    };
    return {{s}};
}

}  // namespace klimt::core::paths
