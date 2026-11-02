// core::trace -- the tracer: an outline's points -> the fewest Beziers (s081).
// See core/Trace.hpp.
#include "core/Trace.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace klimt::core::trace {

using path::P;

namespace {

P operator+(P a, P b) { return {a.x + b.x, a.y + b.y}; }
P operator-(P a, P b) { return {a.x - b.x, a.y - b.y}; }
P operator*(P a, double k) { return {a.x * k, a.y * k}; }
double dot(P a, P b) { return a.x * b.x + a.y * b.y; }
double cross(P a, P b) { return a.x * b.y - a.y * b.x; }
double len(P a) { return std::hypot(a.x, a.y); }
P unit(P a) { const double l = len(a); return l > 1e-12 ? a * (1 / l) : P{0, 0}; }

struct Cubic { P p0, p1, p2, p3; };
P at_t(const Cubic& c, double t) {
    const double u = 1 - t;
    return c.p0 * (u * u * u) + c.p1 * (3 * u * u * t) + c.p2 * (3 * u * t * t) + c.p3 * (t * t * t);
}

// A line: a point on it and its direction (unit).
struct Line { P c, d; };

// The least-squares line through pts[a..b], pointing from the first to the last.
Line line_fit(const std::vector<P>& pts) {
    P c{0, 0};
    for (const P& p : pts) c = c + p;
    c = c * (1.0 / static_cast<double>(pts.size()));
    double sxx = 0, sxy = 0, syy = 0;
    for (const P& p : pts) {
        const P q = p - c;
        sxx += q.x * q.x; sxy += q.x * q.y; syy += q.y * q.y;
    }
    const double th = 0.5 * std::atan2(2 * sxy, sxx - syy);
    P d{std::cos(th), std::sin(th)};
    if (dot(d, pts.back() - pts.front()) < 0) d = d * -1;
    if (len(pts.back() - pts.front()) < 1e-9) d = unit(pts.back() - pts.front());
    return {c, d};
}

bool meet(const Line& a, const Line& b, P& x) {
    const double den = cross(a.d, b.d);
    if (std::abs(den) < 0.05) return false;   // near parallel: no good corner
    const double t = cross(b.c - a.c, b.d) / den;
    x = a.c + a.d * t;
    return true;
}

// ── Schneider's fit, tangents fixed ─────────────────────────────────────────
double bern(int i, double u) {
    const double v = 1 - u;
    switch (i) {
        case 0: return v * v * v;
        case 1: return 3 * u * v * v;
        case 2: return 3 * u * u * v;
        default: return u * u * u;
    }
}
std::vector<double> chord(const std::vector<P>& d) {
    std::vector<double> u(d.size(), 0.0);
    for (std::size_t i = 1; i < d.size(); ++i) u[i] = u[i - 1] + len(d[i] - d[i - 1]);
    if (u.back() > 0) for (auto& v : u) v /= u.back();
    return u;
}
Cubic generate(const std::vector<P>& d, const std::vector<double>& u, P t1, P t2) {
    double C00 = 0, C01 = 0, C11 = 0, X0 = 0, X1 = 0;
    const P p0 = d.front(), p3 = d.back();
    for (std::size_t i = 0; i < d.size(); ++i) {
        const double ui = u[i];
        const P a1 = t1 * bern(1, ui), a2 = t2 * bern(2, ui);
        C00 += dot(a1, a1); C01 += dot(a1, a2); C11 += dot(a2, a2);
        const P tmp = d[i] - (p0 * (bern(0, ui) + bern(1, ui)) + p3 * (bern(2, ui) + bern(3, ui)));
        X0 += dot(a1, tmp); X1 += dot(a2, tmp);
    }
    const double det = C00 * C11 - C01 * C01;
    double al = 0, ar = 0;
    if (std::abs(det) > 1e-12) {
        al = (X0 * C11 - X1 * C01) / det;
        ar = (C00 * X1 - C01 * X0) / det;
    }
    const double seg = len(p3 - p0);
    if (al < 1e-6 * seg || ar < 1e-6 * seg || al > 2 * seg || ar > 2 * seg) al = ar = seg / 3;
    return {p0, p0 + t1 * al, p3 + t2 * ar, p3};
}
void reparam(const std::vector<P>& d, const Cubic& c, std::vector<double>& u) {
    const P q1[3] = {(c.p1 - c.p0) * 3, (c.p2 - c.p1) * 3, (c.p3 - c.p2) * 3};
    const P q2[2] = {(q1[1] - q1[0]) * 2, (q1[2] - q1[1]) * 2};
    for (std::size_t i = 1; i + 1 < u.size(); ++i) {
        const double t = u[i], v = 1 - t;
        const P diff = at_t(c, t) - d[i];
        const P d1 = q1[0] * (v * v) + q1[1] * (2 * v * t) + q1[2] * (t * t);
        const P d2 = q2[0] * v + q2[1] * t;
        const double den = dot(d1, d1) + dot(diff, d2);
        if (std::abs(den) > 1e-12) u[i] = std::clamp(t - dot(diff, d1) / den, 0.0, 1.0);
    }
}
double worst(const std::vector<P>& d, const Cubic& c, const std::vector<double>& u, std::size_t& where) {
    double m = 0;
    where = d.size() / 2;
    for (std::size_t i = 1; i + 1 < d.size(); ++i) {
        const double e = len(at_t(c, u[i]) - d[i]);
        if (e > m) { m = e; where = i; }
    }
    return m;
}
// One cubic through d with these end tangents (t2 pointing BACK from the
// end); its error. Also says whether the run is straight.
Cubic one(const std::vector<P>& d, P t1, P t2, double& err, std::size_t& where) {
    auto u = chord(d);
    Cubic c = generate(d, u, t1, t2);
    err = worst(d, c, u, where);
    for (int it = 0; it < 5 && err > 0; ++it) {
        reparam(d, c, u);
        const Cubic c2 = generate(d, u, t1, t2);
        std::size_t w2 = 0;
        const double e2 = worst(d, c2, u, w2);
        if (e2 >= err) break;
        c = c2; err = e2; where = w2;
    }
    return c;
}
double off_chord(const std::vector<P>& d) {
    const P a = d.front(), b = d.back();
    const P dir = unit(b - a);
    double m = 0;
    for (const P& p : d) m = std::max(m, len(dir) > 0 ? std::abs(cross(dir, p - a)) : len(p - a));
    return m;
}

// An anchor on the loop: where (an absolute point index, may pass n), its
// place, a corner or not, its tangent (smooth) or its two edge directions
// (corner: `out` leaving, `in` arriving).
struct Anchor {
    long i;
    P    at;
    bool corner;
    P    in, out;
};

struct Piece { Cubic c; bool straight; bool split_smooth; };

constexpr double kMaxTurn = 2.0;   // radians, ~115 degrees

}  // namespace

Polygon polygon(const std::vector<P>& loop, const Options& o) {
    Polygon out;
    const long n = static_cast<long>(loop.size());
    if (n < 3) return out;
    const double e = std::max(0.05, o.straight);
    const auto at = [&](long i) { return loop[static_cast<std::size_t>(((i % n) + n) % n)]; };

    // How far a straight edge from each point can reach (points ahead).
    std::vector<long> reach(static_cast<std::size_t>(n), 1);
    for (long i = 0; i < n; ++i) {
        const P base = at(i);
        P b0{0, 0};
        for (long k = 1; k < n && len(b0) == 0; ++k) b0 = unit(at(i + k) - base);
        if (len(b0) == 0) continue;
        double lo = -1e9, hi = 1e9, far = 0;
        long best = 1;
        for (long k = 1; k < n; ++k) {
            const P q = at(i + k) - base;
            const double d = len(q);
            if (d < far - e) break;                       // turning back
            double ang = 0;
            if (d > 1e-9) {
                ang = std::atan2(cross(b0, q), dot(b0, q));
                if (ang < lo || ang > hi) break;          // outside the cone the points before allow
            }
            best = k;
            far = std::max(far, d);
            if (d > e) {
                const double w = std::asin(std::min(1.0, e / d));
                lo = std::max(lo, ang - w);
                hi = std::min(hi, ang + w);
                if (lo > hi) break;
            }
        }
        reach[static_cast<std::size_t>(i)] = best;
    }
    // The fewest edges round: greedy from a start (each edge as long as it
    // reaches), the start tried across the first edge's reach.
    std::vector<long> verts;
    const long r0 = reach[0];
    const long step = std::max(1L, r0 / 32);
    for (long s = 0; s <= std::min(n - 1, r0); s += step) {
        std::vector<long> vs{s};
        long pos = s;
        while (true) {
            const long nxt = pos + reach[static_cast<std::size_t>(pos % n)];
            if (nxt >= s + n) break;
            vs.push_back(nxt);
            pos = nxt;
            if (static_cast<long>(vs.size()) > n) break;
        }
        if (verts.empty() || vs.size() < verts.size()) verts = std::move(vs);
    }
    if (verts.size() < 3) {   // a sliver: four points round it
        verts.clear();
        for (int q = 0; q < 4; ++q) verts.push_back(q * n / 4);
    }

    // Lines through each edge's points; short edges between two sharp turns
    // merged (a chamfered pixel corner); vertices where neighbouring lines meet.
    std::vector<Line> lines;
    std::vector<P> vpos;
    const auto refit = [&] {
        const std::size_t V = verts.size();
        lines.assign(V, {});
        for (std::size_t k = 0; k < V; ++k) {
            const long a = verts[k], b = k + 1 < V ? verts[k + 1] : verts[0] + n;
            // The points between, a couple off each end: a corner's chamfer
            // (pixels cut across the corner) mustn't tilt the edge.
            const long trim = b - a >= 10 ? 2 : b - a >= 6 ? 1 : 0;
            std::vector<P> pts;
            for (long i = a + trim; i <= b - trim; ++i) pts.push_back(at(i));
            lines[k] = line_fit(pts);
        }
        vpos.assign(V, {});
        for (std::size_t k = 0; k < V; ++k) {
            const Line& prev = lines[(k + V - 1) % V];
            const P orig = at(verts[k]);
            P x;
            vpos[k] = meet(prev, lines[k], x) && len(x - orig) <= 2.5 * e + 1.0 ? x : orig;
        }
    };
    refit();
    for (bool merged = true; merged && verts.size() > 3;) {
        merged = false;
        const std::size_t V = verts.size();
        for (std::size_t k = 0; k < V && verts.size() > 3; ++k) {
            const std::size_t k1 = (k + 1) % V;
            if (len(vpos[k1] - vpos[k]) >= std::max(2.5, 4 * e)) continue;
            const Line& before = lines[(k + V - 1) % V];
            const Line& after = lines[k1];
            P x;
            if (!meet(before, after, x) || len(x - vpos[k]) > 3 || len(x - vpos[k1]) > 3) continue;
            // Keep the vertex nearer the meeting point; the other goes.
            const std::size_t gone = len(x - vpos[k]) <= len(x - vpos[k1]) ? k1 : k;
            if (gone == 0) {   // keep the order ascending: the start moves to the kept one
                verts.erase(verts.begin());
                // verts[0] stays the smallest; nothing else to do
            } else {
                verts.erase(verts.begin() + static_cast<long>(gone));
            }
            refit();
            merged = true;
            break;
        }
    }

    const std::size_t V = verts.size();
    out.at.assign(verts.begin(), verts.end());
    for (auto& a : out.at) a = static_cast<int>(((a % n) + n) % n);
    out.v = vpos;
    out.corner.assign(V, false);
    for (std::size_t k = 0; k < V; ++k) {
        const Line& prev = lines[(k + V - 1) % V];
        const Line& next = lines[k];
        const double th = std::acos(std::clamp(dot(prev.d, next.d), -1.0, 1.0));
        const double l1 = len(vpos[k] - vpos[(k + V - 1) % V]), l2 = len(vpos[(k + 1) % V] - vpos[k]);
        const double lmin = std::max(0.5, std::min(l1, l2));
        out.corner[k] = th > std::max(0.1745, o.corner * 16 * e / lmin);
    }
    return out;
}

path::Sub fitted(const std::vector<P>& loop, const Options& o) {
    path::Sub out;
    out.closed = true;
    const long n = static_cast<long>(loop.size());
    if (n < 3) {
        for (const P& p : loop) out.nodes.push_back(path::Node::corner(p));
        return out;
    }
    const auto at = [&](long i) { return loop[static_cast<std::size_t>(((i % n) + n) % n)]; };
    const Polygon pg = polygon(loop, o);
    const std::size_t V = pg.at.size();
    if (V < 3) {
        for (const P& p : loop) out.nodes.push_back(path::Node::corner(p));
        return out;
    }
    const double tol = std::max(0.25, o.tolerance);

    // The anchors, in loop order, absolute indices ascending.
    std::vector<Anchor> an;
    {
        long base = pg.at[0];
        long prev = base;
        for (std::size_t k = 0; k < V; ++k) {
            long i = pg.at[k];
            while (i < prev) i += n;
            prev = i;
            const P vp = pg.v[(k + V - 1) % V], vn = pg.v[(k + 1) % V];
            Anchor a;
            a.i = i;
            a.corner = pg.corner[k];
            a.out = unit(vn - pg.v[k]);
            a.in = unit(vp - pg.v[k]);
            if (a.corner) {
                a.at = pg.v[k];
            } else {
                P m{0, 0};
                for (int j = -2; j <= 2; ++j) m = m + at(i + j);
                a.at = m * 0.2;
                // The tangent: between the two edges -- or along the long one where a
                // straight run meets a curve (the curve leaves the line tangent to it).
                const double ln = len(vn - pg.v[k]), lp = len(pg.v[k] - vp);
                P t = unit(unit(vn - pg.v[k]) + unit(pg.v[k] - vp));
                // ... and the anchor ON that line (the polygon's vertex may sit a
                // little into the curve; the straight run must reach it straight).
                const auto onto = [&](P q, P l0, P l1) { const P d = unit(l1 - l0); return l0 + d * dot(q - l0, d); };
                if (ln > 2.5 * lp) { t = unit(vn - pg.v[k]); a.at = onto(a.at, pg.v[k], vn); }
                else if (lp > 2.5 * ln) { t = unit(pg.v[k] - vp); a.at = onto(a.at, vp, pg.v[k]); }
                a.out = t;
                a.in = t * -1;
            }
            an.push_back(a);
        }
    }
    const std::size_t A = an.size();
    const auto anchor = [&](std::size_t k) {   // k may pass A: the loop again, n further on
        Anchor a = an[k % A];
        a.i += static_cast<long>(k / A) * n;
        return a;
    };
    const auto points = [&](const Anchor& a, const Anchor& b) {
        std::vector<P> d;
        for (long i = a.i; i <= b.i; ++i) d.push_back(at(i));
        d.front() = a.at;
        d.back() = b.at;
        return d;
    };
    // a -> b as one cubic, or as a straight segment, if it fits.
    const auto try_one = [&](const Anchor& a, const Anchor& b, Piece& pc) {
        const auto d = points(a, b);
        if (d.size() < 2) return false;
        const double straight_tol = std::min(tol, std::max(0.5, o.straight));
        if (off_chord(d) <= straight_tol) {
            const P ch = unit(b.at - a.at);
            const bool lined = (a.corner || dot(a.out, ch) > 0.998) && (b.corner || dot(b.in, ch * -1) > 0.998);
            if (lined) { pc = {{a.at, a.at, b.at, b.at}, true, false}; return true; }
        }
        double err = 0;
        std::size_t w = 0;
        const Cubic c = one(d, a.out, b.in, err, w);
        if (err > tol) return false;
        pc = {c, false, false};
        return true;
    };
    // a -> b when even one step doesn't fit: split at the worst point (smooth).
    const std::function<void(const std::vector<P>&, P, P, std::vector<Piece>&, int)> split_fit =
        [&](const std::vector<P>& d, P t1, P t2, std::vector<Piece>& outp, int depth) {
            double err = 0;
            std::size_t w = 0;
            const Cubic c = one(d, t1, t2, err, w);
            if (err <= tol || d.size() < 4 || depth > 12) { outp.push_back({c, false, false}); return; }
            w = std::clamp<std::size_t>(w, 1, d.size() - 2);
            const P tc = unit(d[w + 1] - d[w - 1]);
            std::vector<P> l(d.begin(), d.begin() + static_cast<long>(w) + 1), r(d.begin() + static_cast<long>(w), d.end());
            split_fit(l, t1, tc * -1, outp, depth + 1);
            outp.back().split_smooth = true;
            split_fit(r, tc, t2, outp, depth + 1);
        };
    // Greedy along anchors [from, to] (absolute anchor numbers): each piece as
    // far as one cubic reaches.
    // The fewest pieces from anchor `from` to `to` (absolute anchor numbers):
    // a shortest path over "one cubic reaches from i to j" (at most ~115
    // degrees of turn each); a step no cubic
    // spans is split at its worst point.
    const auto greedy = [&](std::size_t from, std::size_t to, std::vector<Piece>& pcs, std::vector<bool>& smooth_end) {
        const std::size_t m = to - from;
        struct Best { std::size_t cost = std::numeric_limits<std::size_t>::max(); std::size_t from = 0; std::vector<Piece> pieces; };
        std::vector<Best> best(m + 1);
        best[0].cost = 0;
        for (std::size_t i = 0; i < m; ++i) {
            if (best[i].cost == std::numeric_limits<std::size_t>::max()) continue;
            double turn = 0;
            P t_prev = anchor(from + i).out;
            int misses = 0;
            for (std::size_t j = i + 1; j <= m; ++j) {
                const P t_here = anchor(from + j).in * -1;
                turn += std::acos(std::clamp(dot(t_prev, t_here), -1.0, 1.0));
                t_prev = anchor(from + j).out;
                if (turn > kMaxTurn && j > i + 1) break;
                Piece pc;
                std::vector<Piece> way;
                if (try_one(anchor(from + i), anchor(from + j), pc)) {
                    way.push_back(pc);
                    misses = 0;
                } else if (j == i + 1) {
                    const Anchor a = anchor(from + i), b = anchor(from + j);
                    split_fit(points(a, b), a.out, b.in, way, 0);
                } else {
                    if (++misses >= 2) break;
                    continue;
                }
                const std::size_t c = best[i].cost + way.size();
                if (c < best[j].cost) {
                    best[j].cost = c;
                    best[j].from = i;
                    best[j].pieces = std::move(way);
                }
            }
        }
        // Back from the end.
        std::vector<std::size_t> path_ends;
        for (std::size_t k = m; k > 0; k = best[k].from) path_ends.push_back(k);
        std::reverse(path_ends.begin(), path_ends.end());
        for (const std::size_t k : path_ends) {
            const auto& way = best[k].pieces;
            for (std::size_t c = 0; c < way.size(); ++c) {
                pcs.push_back(way[c]);
                smooth_end.push_back(c + 1 < way.size() ? true : !anchor(from + k).corner);
            }
        }
    };

    std::vector<Piece> pieces;
    std::vector<bool> smooth_end;
    std::size_t first = 0;
    const auto first_corner = std::find_if(an.begin(), an.end(), [](const Anchor& a) { return a.corner; });
    if (first_corner != an.end()) {
        first = static_cast<std::size_t>(first_corner - an.begin());
        std::size_t k = first;
        while (k < first + A) {
            std::size_t next = k + 1;
            while (next < first + A && !anchor(next).corner) ++next;
            greedy(k, next, pieces, smooth_end);
            k = next;
        }
    } else {
        // No corner: the start that needs the fewest pieces (four tried, spread round).
        const std::size_t tries = std::min<std::size_t>(A, 4);
        std::vector<Piece> best_p;
        std::vector<bool> best_s;
        std::size_t best_first = 0;
        for (std::size_t q = 0; q < tries; ++q) {
            const std::size_t s = q * A / tries;
            std::vector<Piece> p;
            std::vector<bool> se;
            greedy(s, s + A, p, se);
            if (best_p.empty() || p.size() < best_p.size()) { best_p = std::move(p); best_s = std::move(se); best_first = s; }
        }
        pieces = std::move(best_p);
        smooth_end = std::move(best_s);
        first = best_first;
    }

    // Pieces to anchors: each piece's start is the anchor before it.
    const bool start_smooth = !anchor(first).corner;
    for (std::size_t c = 0; c < pieces.size(); ++c) {
        const Cubic& cu = pieces[c].c;
        if (out.nodes.empty()) out.nodes.push_back({cu.p0, cu.p0, cu.p1, start_smooth});
        else out.nodes.back().out = cu.p1;
        out.nodes.push_back({cu.p3, cu.p2, cu.p3, smooth_end[c]});
    }
    if (out.nodes.size() > 1) {   // the last anchor is the first again
        out.nodes.front().in = out.nodes.back().in;
        out.nodes.pop_back();
    }
    return out;
}

}  // namespace klimt::core::trace
