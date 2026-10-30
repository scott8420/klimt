// core::path -- paths' geometry (s035). docs/paths.md.
#include "core/Path.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace klimt::core::path {

namespace {
P operator+(P a, P b) { return {a.x + b.x, a.y + b.y}; }
P operator-(P a, P b) { return {a.x - b.x, a.y - b.y}; }
P operator*(P a, double k) { return {a.x * k, a.y * k}; }
double dot(P a, P b) { return a.x * b.x + a.y * b.y; }
double len(P a) { return std::hypot(a.x, a.y); }
double dist2(P a, P b) { const P d = a - b; return dot(d, d); }
P unit(P a) { const double l = len(a); return l > 1e-12 ? a * (1 / l) : P{0, 0}; }
P lerp(P a, P b, double t) { return a + (b - a) * t; }

P cubic(P p0, P p1, P p2, P p3, double t) {
    const double u = 1 - t;
    return p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
}

// Distance from q to the line through a, b (a == b: to the point).
double off_line(P q, P a, P b) {
    const P d = b - a;
    const double l = len(d);
    if (l < 1e-12) return len(q - a);
    return std::abs(d.x * (q.y - a.y) - d.y * (q.x - a.x)) / l;
}

// Adaptive flattening: split until the handles lie within tol of the chord.
void flat(P p0, P p1, P p2, P p3, double tol, int depth, std::vector<P>& out) {
    if (depth > 16 || (off_line(p1, p0, p3) <= tol && off_line(p2, p0, p3) <= tol)) {
        out.push_back(p3);
        return;
    }
    const P q0 = lerp(p0, p1, 0.5), q1 = lerp(p1, p2, 0.5), q2 = lerp(p2, p3, 0.5);
    const P r0 = lerp(q0, q1, 0.5), r1 = lerp(q1, q2, 0.5), s = lerp(r0, r1, 0.5);
    flat(p0, q0, r0, s, tol, depth + 1, out);
    flat(s, r1, q2, p3, tol, depth + 1, out);
}

std::vector<P> sub_points(const Sub& s, double tol, bool close) {
    std::vector<P> out;
    const auto n = s.nodes.size();
    if (n == 0) return out;
    out.push_back(s.nodes[0].at);
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const Node& a = s.nodes[i];
        const Node& b = s.nodes[i + 1];
        flat(a.at, a.out, b.in, b.at, tol, 0, out);
    }
    if (s.closed && n > 1) {
        const Node& a = s.nodes[n - 1];
        const Node& b = s.nodes[0];
        flat(a.at, a.out, b.in, b.at, tol, 0, out);
        if (!close) return out;   // ends on the first point again
        out.pop_back();           // a polygon: the first point closes it
    }
    return out;
}

bool is_corner(const Node& n) { return n.in == n.at && n.out == n.at; }
}  // namespace

bool Path::empty() const {
    for (const auto& s : subs) if (!s.nodes.empty()) return false;
    return true;
}
std::size_t Path::anchors() const {
    std::size_t n = 0;
    for (const auto& s : subs) n += s.nodes.size();
    return n;
}

P bezier(const Node& a, const Node& b, double t) { return cubic(a.at, a.out, b.in, b.at, t); }

std::vector<std::vector<P>> flatten(const Path& p, double tol) {
    std::vector<std::vector<P>> v;
    for (const auto& s : p.subs) {
        if (s.nodes.size() < 2) continue;
        auto pts = sub_points(s, tol, true);
        if (pts.size() >= 3) v.push_back(std::move(pts));
    }
    return v;
}

std::vector<std::vector<P>> outline(const Path& p, double tol) {
    std::vector<std::vector<P>> v;
    for (const auto& s : p.subs) {
        if (s.nodes.empty()) continue;
        v.push_back(sub_points(s, tol, false));
    }
    return v;
}

Path mapped(const Path& p, const std::function<P(P)>& f) {
    Path q = p;
    for (auto& s : q.subs)
        for (auto& n : s.nodes) { n.at = f(n.at); n.in = f(n.in); n.out = f(n.out); }
    return q;
}

Box box(const Path& p) {
    Box b;
    bool first = true;
    for (const auto& s : p.subs)
        for (const auto& n : s.nodes)
            for (const P q : {n.at, n.in, n.out}) {
                if (first) { b = {q.x, q.y, q.x, q.y}; first = false; continue; }
                b.x0 = std::min(b.x0, q.x); b.y0 = std::min(b.y0, q.y);
                b.x1 = std::max(b.x1, q.x); b.y1 = std::max(b.y1, q.y);
            }
    return b;
}

Hit hit(const Path& p, P q, double tol) {
    const double t2 = tol * tol;
    Hit best;
    double bd = t2;
    for (int si = 0; si < static_cast<int>(p.subs.size()); ++si)
        for (int ni = 0; ni < static_cast<int>(p.subs[static_cast<std::size_t>(si)].nodes.size()); ++ni) {
            const double d = dist2(q, p.subs[static_cast<std::size_t>(si)].nodes[static_cast<std::size_t>(ni)].at);
            if (d <= bd) { bd = d; best = {Hit::Anchor, si, ni, 0}; }
        }
    if (best.what != Hit::None) return best;
    for (int si = 0; si < static_cast<int>(p.subs.size()); ++si)
        for (int ni = 0; ni < static_cast<int>(p.subs[static_cast<std::size_t>(si)].nodes.size()); ++ni) {
            const Node& n = p.subs[static_cast<std::size_t>(si)].nodes[static_cast<std::size_t>(ni)];
            if (!(n.in == n.at)) { const double d = dist2(q, n.in); if (d <= bd) { bd = d; best = {Hit::In, si, ni, 0}; } }
            if (!(n.out == n.at)) { const double d = dist2(q, n.out); if (d <= bd) { bd = d; best = {Hit::Out, si, ni, 0}; } }
        }
    if (best.what != Hit::None) return best;
    for (int si = 0; si < static_cast<int>(p.subs.size()); ++si) {
        const Sub& s = p.subs[static_cast<std::size_t>(si)];
        const int n = static_cast<int>(s.nodes.size());
        const int segs = s.closed ? n : n - 1;
        for (int i = 0; i < segs; ++i) {
            const Node& a = s.nodes[static_cast<std::size_t>(i)];
            const Node& b = s.nodes[static_cast<std::size_t>((i + 1) % n)];
            constexpr int kSteps = 64;
            for (int k = 1; k < kSteps; ++k) {
                const double t = static_cast<double>(k) / kSteps;
                const double d = dist2(q, bezier(a, b, t));
                if (d <= bd) { bd = d; best = {Hit::Segment, si, i, t}; }
            }
        }
    }
    return best;
}

Path inserted(const Path& p, int sub, int node, double t) {
    Path q = p;
    if (sub < 0 || sub >= static_cast<int>(q.subs.size())) return q;
    Sub& s = q.subs[static_cast<std::size_t>(sub)];
    const int n = static_cast<int>(s.nodes.size());
    if (node < 0 || node >= n || (node == n - 1 && !s.closed) || n < 2) return q;
    Node& a = s.nodes[static_cast<std::size_t>(node)];
    Node& b = s.nodes[static_cast<std::size_t>((node + 1) % n)];
    Node mid;
    if (a.out == a.at && b.in == b.at) {   // a straight segment: a corner on it
        mid = Node::corner(lerp(a.at, b.at, t));
    } else {
        const P q0 = lerp(a.at, a.out, t), q1 = lerp(a.out, b.in, t), q2 = lerp(b.in, b.at, t);
        const P r0 = lerp(q0, q1, t), r1 = lerp(q1, q2, t);
        mid = {lerp(r0, r1, t), r0, r1, true};
        a.out = q0;
        b.in = q2;
    }
    s.nodes.insert(s.nodes.begin() + node + 1, mid);
    return q;
}

Path removed(const Path& p, int sub, int node) {
    Path q = p;
    if (sub < 0 || sub >= static_cast<int>(q.subs.size())) return q;
    Sub& s = q.subs[static_cast<std::size_t>(sub)];
    if (node < 0 || node >= static_cast<int>(s.nodes.size())) return q;
    s.nodes.erase(s.nodes.begin() + node);
    if (s.nodes.size() < 2) s.closed = false;
    if (s.nodes.empty()) q.subs.erase(q.subs.begin() + sub);
    return q;
}

Path moved(const Path& p, int sub, int node, Hit::What what, P d) {
    Path q = p;
    if (sub < 0 || sub >= static_cast<int>(q.subs.size())) return q;
    Sub& s = q.subs[static_cast<std::size_t>(sub)];
    if (node < 0 || node >= static_cast<int>(s.nodes.size())) return q;
    Node& n = s.nodes[static_cast<std::size_t>(node)];
    switch (what) {
        case Hit::Anchor: n.at = n.at + d; n.in = n.in + d; n.out = n.out + d; break;
        case Hit::In:
        case Hit::Out: {
            P& h = what == Hit::In ? n.in : n.out;
            P& o = what == Hit::In ? n.out : n.in;
            h = h + d;
            if (n.smooth) {
                const double l = len(o - n.at);
                const P dir = unit(n.at - h);
                if (l > 1e-9 && len(dir) > 0) o = n.at + dir * l;
            }
            break;
        }
        default: break;
    }
    return q;
}

Path toggled(const Path& p, int sub, int node) {
    Path q = p;
    if (sub < 0 || sub >= static_cast<int>(q.subs.size())) return q;
    Sub& s = q.subs[static_cast<std::size_t>(sub)];
    const int n = static_cast<int>(s.nodes.size());
    if (node < 0 || node >= n) return q;
    Node& m = s.nodes[static_cast<std::size_t>(node)];
    if (m.smooth || !is_corner(m)) {
        m = Node::corner(m.at);
        return q;
    }
    const auto at = [&](int i) {
        if (s.closed) return s.nodes[static_cast<std::size_t>((i + n) % n)].at;
        return s.nodes[static_cast<std::size_t>(std::clamp(i, 0, n - 1))].at;
    };
    const P prev = at(node - 1), next = at(node + 1);
    P dir = unit(next - prev);
    if (len(dir) == 0) dir = {1, 0};
    m.in = m.at - dir * (len(m.at - prev) / 3);
    m.out = m.at + dir * (len(next - m.at) / 3);
    m.smooth = true;
    return q;
}

// ── fitting ────────────────────────────────────────────────────────────────

namespace {
struct Cubic { P p0, p1, p2, p3; bool split_after = false; };

double bern(int i, double u) {
    const double v = 1 - u;
    switch (i) {
        case 0: return v * v * v;
        case 1: return 3 * u * v * v;
        case 2: return 3 * u * u * v;
        default: return u * u * u;
    }
}

Cubic generate(const std::vector<P>& d, int first, int last, const std::vector<double>& u, P t1, P t2) {
    const int n = last - first + 1;
    double C[2][2] = {{0, 0}, {0, 0}}, X[2] = {0, 0};
    const P p0 = d[static_cast<std::size_t>(first)], p3 = d[static_cast<std::size_t>(last)];
    for (int i = 0; i < n; ++i) {
        const double ui = u[static_cast<std::size_t>(i)];
        const P a1 = t1 * bern(1, ui), a2 = t2 * bern(2, ui);
        C[0][0] += dot(a1, a1); C[0][1] += dot(a1, a2); C[1][1] += dot(a2, a2);
        const P tmp = d[static_cast<std::size_t>(first + i)] - (p0 * (bern(0, ui) + bern(1, ui)) + p3 * (bern(2, ui) + bern(3, ui)));
        X[0] += dot(a1, tmp); X[1] += dot(a2, tmp);
    }
    C[1][0] = C[0][1];
    const double det = C[0][0] * C[1][1] - C[1][0] * C[0][1];
    double al = 0, ar = 0;
    if (std::abs(det) > 1e-12) {
        al = (X[0] * C[1][1] - X[1] * C[0][1]) / det;
        ar = (C[0][0] * X[1] - C[1][0] * X[0]) / det;
    }
    const double seg = len(p3 - p0), eps = 1e-6 * seg;
    if (al < eps || ar < eps) al = ar = seg / 3;   // Wu / Barsky's fallback
    return {p0, p0 + t1 * al, p3 + t2 * ar, p3};
}

double max_error(const std::vector<P>& d, int first, int last, const Cubic& c, const std::vector<double>& u, int& split) {
    double m = 0;
    split = (first + last) / 2;
    for (int i = first + 1; i < last; ++i) {
        const double e = dist2(cubic(c.p0, c.p1, c.p2, c.p3, u[static_cast<std::size_t>(i - first)]), d[static_cast<std::size_t>(i)]);
        if (e >= m) { m = e; split = i; }
    }
    return m;
}

std::vector<double> chord(const std::vector<P>& d, int first, int last) {
    std::vector<double> u(static_cast<std::size_t>(last - first + 1), 0.0);
    for (int i = first + 1; i <= last; ++i)
        u[static_cast<std::size_t>(i - first)] = u[static_cast<std::size_t>(i - first - 1)] + len(d[static_cast<std::size_t>(i)] - d[static_cast<std::size_t>(i - 1)]);
    const double total = u.back();
    if (total > 0) for (auto& v : u) v /= total;
    return u;
}

// Newton-Raphson: each u moved toward the curve's nearest point.
void reparam(const std::vector<P>& d, int first, const Cubic& c, std::vector<double>& u) {
    const P q1[3] = {(c.p1 - c.p0) * 3, (c.p2 - c.p1) * 3, (c.p3 - c.p2) * 3};
    const P q2[2] = {(q1[1] - q1[0]) * 2, (q1[2] - q1[1]) * 2};
    for (std::size_t i = 0; i < u.size(); ++i) {
        const double t = u[i], v = 1 - t;
        const P qt = cubic(c.p0, c.p1, c.p2, c.p3, t);
        const P q1t = q1[0] * (v * v) + q1[1] * (2 * v * t) + q1[2] * (t * t);
        const P q2t = q2[0] * v + q2[1] * t;
        const P diff = qt - d[static_cast<std::size_t>(first) + i];
        const double num = dot(diff, q1t), den = dot(q1t, q1t) + dot(diff, q2t);
        if (std::abs(den) > 1e-12) u[i] = std::clamp(t - num / den, 0.0, 1.0);
    }
}

void fit_cubic(const std::vector<P>& d, int first, int last, P t1, P t2, double err, std::vector<Cubic>& out, int depth = 0) {
    const int n = last - first + 1;
    if (n == 2) {
        const double l = len(d[static_cast<std::size_t>(last)] - d[static_cast<std::size_t>(first)]) / 3;
        out.push_back({d[static_cast<std::size_t>(first)], d[static_cast<std::size_t>(first)] + t1 * l,
                       d[static_cast<std::size_t>(last)] + t2 * l, d[static_cast<std::size_t>(last)]});
        return;
    }
    auto u = chord(d, first, last);
    Cubic c = generate(d, first, last, u, t1, t2);
    int split = 0;
    double e = max_error(d, first, last, c, u, split);
    if (e < err) { out.push_back(c); return; }
    if (e < err * 4) {
        for (int it = 0; it < 6; ++it) {
            reparam(d, first, c, u);
            c = generate(d, first, last, u, t1, t2);
            e = max_error(d, first, last, c, u, split);
            if (e < err) { out.push_back(c); return; }
        }
    }
    if (depth > 40) { out.push_back(c); return; }
    split = std::clamp(split, first + 1, last - 1);
    P tc = unit(d[static_cast<std::size_t>(split - 1)] - d[static_cast<std::size_t>(split + 1)]);
    if (len(tc) == 0) tc = unit(d[static_cast<std::size_t>(first)] - d[static_cast<std::size_t>(last)]);
    fit_cubic(d, first, split, t1, tc, err, out, depth + 1);
    out.back().split_after = true;   // the anchor between the halves is smooth
    fit_cubic(d, split, last, tc * -1, t2, err, out, depth + 1);
}

// Where two straight runs meet: the line through a0 -> a1 against b0 -> b1.
bool meet(P a0, P a1, P b0, P b1, P& x) {
    const P r = a1 - a0, s = b1 - b0;
    const double den = r.x * s.y - r.y * s.x;
    if (std::abs(den) < 1e-9) return false;
    const double t = ((b0.x - a0.x) * s.y - (b0.y - a0.y) * s.x) / den;
    x = a0 + r * t;
    return true;
}
}  // namespace

Sub fit(const std::vector<P>& loop, double tol) {
    Sub out;
    out.closed = true;
    const int n = static_cast<int>(loop.size());
    if (n < 3) {
        for (const P& p : loop) out.nodes.push_back(Node::corner(p));
        return out;
    }
    const auto at = [&](int i) { return loop[static_cast<std::size_t>(((i % n) + n) % n)]; };
    // Corners: a turn sharper than ~55 degrees seen k points either side,
    // the sharpest of each run of sharp points.
    const int k = std::max(1, std::min(3, n / 8));
    std::vector<double> cosv(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const P a = unit(at(i) - at(i - k)), b = unit(at(i + k) - at(i));
        cosv[static_cast<std::size_t>(i)] = dot(a, b);
    }
    constexpr double kSharp = 0.57;
    std::vector<int> corners;
    for (int i = 0; i < n; ++i) {
        const double c = cosv[static_cast<std::size_t>(i)];
        if (c >= kSharp) continue;
        bool best = true;
        for (int j = -k; j <= k && best; ++j) {
            if (j == 0) continue;
            const double o = cosv[static_cast<std::size_t>(((i + j) % n + n) % n)];
            if (o < c || (o == c && j < 0)) best = false;
        }
        if (best) corners.push_back(i);
    }
    // The anchors: corners where they are (moved to where the runs either
    // side meet, when that's close), else four smooth joins.
    struct Mark { int i; P at; bool corner; };
    std::vector<Mark> marks;
    if (corners.empty()) {
        for (int q = 0; q < 4; ++q) {
            const int i = q * n / 4;
            marks.push_back({i, at(i), false});
        }
    } else {
        const int m = std::max(2, std::min(8, n / 8));
        for (const int i : corners) {
            P x = at(i);
            P y;
            if (meet(at(i - m), at(i - 2), at(i + 2), at(i + m), y) && len(y - x) < 2.0) x = y;
            marks.push_back({i, x, true});
        }
    }
    const double err = tol * tol;
    std::vector<Cubic> curves;
    std::vector<bool> smooth_after;   // per curve: is the anchor at its END smooth
    const int M = static_cast<int>(marks.size());
    for (int r = 0; r < M; ++r) {
        const Mark& a = marks[static_cast<std::size_t>(r)];
        const Mark& b = marks[static_cast<std::size_t>((r + 1) % M)];
        int span = b.i - a.i;
        if (span <= 0) span += n;
        std::vector<P> d;
        d.reserve(static_cast<std::size_t>(span + 1));
        for (int s = 0; s <= span; ++s) d.push_back(at(a.i + s));
        d.front() = a.at;
        d.back() = b.at;
        const int last = static_cast<int>(d.size()) - 1;
        const int h = std::max(1, std::min(3, last));
        P t1 = a.corner ? unit(d[static_cast<std::size_t>(h)] - d[0]) : unit(at(a.i + 2) - at(a.i - 2));
        P t2 = b.corner ? unit(d[static_cast<std::size_t>(last - h)] - d[static_cast<std::size_t>(last)]) : unit(at(b.i - 2) - at(b.i + 2));
        if (len(t1) == 0) t1 = unit(d[static_cast<std::size_t>(last)] - d[0]);
        if (len(t2) == 0) t2 = unit(d[0] - d[static_cast<std::size_t>(last)]);
        const std::size_t before = curves.size();
        fit_cubic(d, 0, last, t1, t2, err, curves);
        for (std::size_t c = before; c < curves.size(); ++c) smooth_after.push_back(curves[c].split_after);
        smooth_after.back() = !b.corner;
    }
    // Curves to anchors: each curve's start is the anchor before it.
    for (std::size_t c = 0; c < curves.size(); ++c) {
        const Cubic& cu = curves[c];
        if (out.nodes.empty()) out.nodes.push_back({cu.p0, cu.p0, cu.p1, !marks[0].corner});
        else out.nodes.back().out = cu.p1;
        out.nodes.push_back({cu.p3, cu.p2, cu.p3, smooth_after[c]});
    }
    // The last anchor is the first again: fold it in.
    if (out.nodes.size() > 1) {
        out.nodes.front().in = out.nodes.back().in;
        out.nodes.pop_back();
    }
    // A segment fitted straight: no handles (cleaner to edit).
    const int N = static_cast<int>(out.nodes.size());
    for (int i = 0; i < N; ++i) {
        Node& a = out.nodes[static_cast<std::size_t>(i)];
        Node& b = out.nodes[static_cast<std::size_t>((i + 1) % N)];
        if (!a.smooth && !b.smooth && off_line(a.out, a.at, b.at) < 0.05 && off_line(b.in, a.at, b.at) < 0.05) {
            a.out = a.at;
            b.in = b.at;
        }
    }
    return out;
}

}  // namespace klimt::core::path
