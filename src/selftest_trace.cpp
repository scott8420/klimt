// klimt_selftest -- s081: THE TRACER (core/Trace.hpp). Shapes with known
// outlines, drawn as HARD pixels (the staircase a Streamline-style tracer
// must see through) and anti-aliased: the polygon's corners where the shape
// turns and nowhere else; the fewest anchors -- a disc 4 smooth, a square 4
// corners with straight sides, a triangle 3, an octagon 8, a rounded rectangle
// its 4 runs and 4 arcs; every curve within its tolerance of the true
// outline. Then the sources Make Path reads: a layer mask, a layer's
// transparency, a saved selection. Rules only -- no fonts, no timing.
#include "core/Document.hpp"
#include "core/Path.hpp"
#include "core/Paths.hpp"
#include "core/Selection.hpp"
#include "core/Trace.hpp"
#include "pixel/Pixel.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace S = klimt::core::select;
namespace L = klimt::core::layers;
namespace path = klimt::core::path;
namespace paths = klimt::core::paths;
namespace trace = klimt::core::trace;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {

P::Format mf() { return {P::Depth::U8, P::Mode::Gray, false}; }
std::string num(double v) { return std::to_string(v); }

P::TileMap hard(int w, int h, const std::function<bool(double, double)>& in) {
    return P::TileMap(mf(), w, h).paint({0, 0, w, h}, [&](int x, int y, const P::Px&) {
        P::Px p;
        p.c[0] = in(x + 0.5, y + 0.5) ? 1.f : 0.f;
        return p;
    });
}

// The farthest the traced outline strays from the true one (signed distance `sd`).
double worst(const path::Path& p, const std::function<double(path::P)>& sd) {
    double m = 0;
    for (const auto& poly : path::flatten(p, 0.05))
        for (const auto& q : poly) m = std::max(m, std::abs(sd(q)));
    return m;
}
std::size_t corners(const path::Path& p) {
    std::size_t c = 0;
    for (const auto& s : p.subs) for (const auto& n : s.nodes) c += n.smooth ? 0 : 1;
    return c;
}
std::string count(const std::optional<path::Path>& p) {
    return p ? std::to_string(p->anchors()) + " anchors, " + std::to_string(corners(*p)) + " corners" : "nullopt";
}

constexpr double cx = 200, cy = 200;
// A square of half-side 75 turned by `a`.
std::function<bool(double, double)> square(double a) {
    return [a](double x, double y) {
        const double c = std::cos(a), s = std::sin(a), u = (x - cx) * c + (y - cy) * s, v = -(x - cx) * s + (y - cy) * c;
        return std::abs(u) < 75 && std::abs(v) < 75;
    };
}
std::function<double(path::P)> square_d(double a) {
    return [a](path::P q) {
        const double c = std::cos(a), s = std::sin(a), u = (q.x - cx) * c + (q.y - cy) * s, v = -(q.x - cx) * s + (q.y - cy) * c;
        return std::max(std::abs(u), std::abs(v)) - 75;
    };
}
path::P square_corner(double a, int k) {
    const double u = (k == 0 || k == 3) ? 75 : -75, v = (k < 2) ? 75 : -75;
    const double c = std::cos(a), s = std::sin(a);
    return {cx + u * c - v * s, cy + u * s + v * c};
}
double rr_d(path::P q) {   // a 260 x 160 rectangle with corners rounded r 30 (centre 200, 200)
    const double ax = std::abs(q.x - cx) - 100, ay = std::abs(q.y - cy) - 50;
    if (ax > 0 || ay > 0) {
        if (ax > 0 && ay > 0) return std::hypot(ax, ay) - 30;
        return std::max(ax, ay) - 30;
    }
    return std::max(ax, ay) - 30;
}

}  // namespace

void selftest_trace(const Check& check) {
    // ── m1: the polygon and its corners ─────────────────────────────────────
    {
        for (const double deg : {10.0, 30.0, 45.0}) {
            const double a = deg * M_PI / 180;
            const auto loops = paths::outlines(hard(400, 400, square(a)));
            const auto pg = loops && loops->size() == 1 ? trace::polygon(loops->front()) : trace::Polygon{};
            bool near = pg.v.size() == 4;
            double far = 0;
            for (std::size_t k = 0; near && k < 4; ++k) {
                double best = 1e9;
                for (int c = 0; c < 4; ++c) best = std::min(best, std::hypot(pg.v[k].x - square_corner(a, c).x, pg.v[k].y - square_corner(a, c).y));
                far = std::max(far, best);
            }
            const std::string d = std::to_string(static_cast<int>(deg));
            check("trace polygon: a hard square turned " + d + " degrees -- 4 vertices, the steps read as straight edges",
                  pg.v.size() == 4, num(static_cast<double>(pg.v.size())));
            check("trace polygon: ... all corners, each within half a pixel of the true one (the raster's own precision)",
                  near && std::all_of(pg.corner.begin(), pg.corner.end(), [](bool c) { return c; }) && far <= 0.5, num(far));
        }
        const auto oct = hard(400, 400, [](double x, double y) {
            const double a = std::atan2(y - cy, x - cx), r = std::hypot(x - cx, y - cy), k = M_PI / 8;
            const double ap = std::fmod(a + 2 * M_PI + k, 2 * k) - k;
            return r * std::cos(ap) < 100 * std::cos(k);
        });
        const auto ol = paths::outlines(oct);
        const auto opg = trace::polygon(ol->front());
        check("trace polygon: a hard octagon -- 8 vertices, 8 corners (a 45-degree turn is a corner on long sides)",
              opg.v.size() == 8 && std::count(opg.corner.begin(), opg.corner.end(), true) == 8, num(static_cast<double>(opg.v.size())));
        const auto disc = hard(400, 400, [](double x, double y) { return std::hypot(x - cx, y - cy) < 100; });
        const auto dpg = trace::polygon(paths::outlines(disc)->front());
        check("trace polygon: a hard disc -- many vertices, no corner", dpg.v.size() > 8 && std::none_of(dpg.corner.begin(), dpg.corner.end(), [](bool c) { return c; }),
              num(static_cast<double>(dpg.v.size())));
        const auto small = hard(400, 400, [](double x, double y) { return std::hypot(x - cx, y - cy) < 8; });
        const auto spg = trace::polygon(paths::outlines(small)->front());
        check("trace polygon: a small hard disc (r 8) -- still no corner", std::none_of(spg.corner.begin(), spg.corner.end(), [](bool c) { return c; }), "");
        check("trace polygon: under three points -- nothing", trace::polygon({{0, 0}, {1, 1}}).v.empty(), "");
    }

    // ── m2: the fewest Beziers ──────────────────────────────────────────────
    {
        const auto disc = hard(400, 400, [](double x, double y) { return std::hypot(x - cx, y - cy) < 100; });
        const auto disc_d = [](path::P q) { return std::hypot(q.x - cx, q.y - cy) - 100; };
        for (const double tol : {1.0, 2.0}) {
            const auto p = paths::traced(disc, tol);
            check("trace: a hard disc at tolerance " + num(tol) + " -- 4 smooth anchors", p && p->anchors() == 4 && corners(*p) == 0, count(p));
            check("trace: ... within tolerance of the true circle", p && worst(*p, disc_d) <= tol, p ? num(worst(*p, disc_d)) : "");
        }
        const auto aa = S::ellipse_mask(mf(), 400, 400, 100, 100, 300, 300);
        const auto ap = paths::traced(aa, 1.0);
        check("trace: an anti-aliased disc -- 4 smooth anchors, within 0.5 px", ap && ap->anchors() == 4 && worst(*ap, disc_d) <= 0.5,
              count(ap) + (ap ? " " + num(worst(*ap, disc_d)) : ""));
        // The s035 fitter on the same hard disc, for the record: the tracer needs far fewer.
        const auto loops = paths::outlines(disc);
        const auto old = path::fit(loops->front(), 1.0);
        const auto now = paths::traced(disc, 1.0);
        check("trace: fewer anchors than the s035 fitter on hard pixels (a quarter or less)",
              now && now->anchors() * 4 <= old.nodes.size(), count(now) + " against " + std::to_string(old.nodes.size()));
        for (const double deg : {10.0, 30.0}) {
            const double a = deg * M_PI / 180;
            const auto p = paths::traced(hard(400, 400, square(a)), 2.0);
            bool straight = p && p->anchors() == 4 && corners(*p) == 4;
            if (straight) for (const auto& n : p->subs[0].nodes) straight = straight && n.in == n.at && n.out == n.at;
            check("trace: a hard square turned " + std::to_string(static_cast<int>(deg)) + " -- 4 corners, straight sides (no handles)", straight, count(p));
            check("trace: ... on the true square (0.35 px)", p && worst(*p, square_d(a)) <= 0.35, p ? num(worst(*p, square_d(a))) : "");
        }
        const auto tri = paths::traced(hard(400, 400, [](double x, double y) { return y > 60 && y < 340 && std::abs(x - cx) < (y - 60) * 0.6; }), 2.0);
        check("trace: a hard triangle -- 3 corners", tri && tri->anchors() == 3 && corners(*tri) == 3, count(tri));
        const auto rr = hard(400, 400, [](double x, double y) { return rr_d({x, y}) < 0; });
        const auto rp = paths::traced(rr, 2.0);
        check("trace: a hard rounded rectangle at 2 px -- 8 anchors (4 runs, 4 arcs), none a corner",
              rp && rp->anchors() == 8 && corners(*rp) == 0, count(rp));
        check("trace: ... within tolerance of the true outline", rp && worst(*rp, rr_d) <= 2.0, rp ? num(worst(*rp, rr_d)) : "");
        bool lines = false;
        if (rp) for (const auto& n : rp->subs[0].nodes) lines = lines || n.out == n.at;
        check("trace: ... its straight runs drawn without handles", lines, "");
        bool in_line = rp.has_value();
        if (rp) for (const auto& n : rp->subs[0].nodes) {
            const double ax = n.in.x - n.at.x, ay = n.in.y - n.at.y, bx = n.out.x - n.at.x, by = n.out.y - n.at.y;
            const double li = std::hypot(ax, ay), lo = std::hypot(bx, by);
            if (li > 1e-6 && lo > 1e-6) in_line = in_line && (ax * bx + ay * by) / (li * lo) < -0.999;
        }
        check("trace: ... every smooth anchor's handles in line", in_line, "");
        // A "D": two corners, a straight back, one curve.
        const auto dee = hard(400, 400, [](double x, double y) { return (x > 100 && x < 200 && std::abs(y - cy) < 100) || (x >= 200 && std::hypot(x - 200, y - cy) < 100); });
        const auto dp = paths::traced(dee, 2.0);
        check("trace: a hard D -- its 2 corners, 5 anchors or fewer", dp && corners(*dp) == 2 && dp->anchors() <= 5, count(dp));
        // A ring keeps its hole; tolerance still loosens.
        const auto ring = S::combine(&disc, hard(400, 400, [](double x, double y) { return std::hypot(x - cx, y - cy) < 50; }), S::Op::Subtract);
        const auto rg = paths::traced(ring, 1.0);
        check("trace: a hard ring -- two subpaths of 4 anchors", rg && rg->subs.size() == 2 && rg->subs[0].nodes.size() == 4 && rg->subs[1].nodes.size() == 4, count(rg));
        // A noisy blob: within tolerance everywhere, and nowhere near one anchor per pixel.
        const auto blob_r = [](double a) { return 120 + 18 * std::sin(3 * a + 0.4) + 9 * std::sin(5 * a + 1.3) + 4 * std::sin(9 * a); };
        const auto blob = hard(400, 400, [&](double x, double y) { return std::hypot(x - cx, y - cy) < blob_r(std::atan2(y - cy, x - cx)); });
        const auto bp = paths::traced(blob, 2.0);
        const auto bl = paths::outlines(blob);
        const auto cov = bp ? paths::coverage(*bp, mf(), 400, 400) : P::TileMap(mf(), 1, 1);
        double i = 0, u = 0;
        for (int y = 0; bp && y < 400; ++y)
            for (int x = 0; x < 400; ++x) {
                const double p = S::coverage(cov, x, y), q = S::coverage(blob, x, y);
                i += std::min(p, q); u += std::max(p, q);
            }
        check("trace: a wavy blob -- the selection back much the same (IoU 0.98)", bp && u > 0 && i / u > 0.98, num(u > 0 ? i / u : 0));
        const auto blob_d = [&](path::P q) { return std::hypot(q.x - cx, q.y - cy) - blob_r(std::atan2(q.y - cy, q.x - cx)); };
        check("trace: ... within tolerance (+ the raster's half pixel) of the true outline", bp && worst(*bp, blob_d) <= 2.5, bp ? num(worst(*bp, blob_d)) : "");
        check("trace: ... in few anchors (under one per 25 px of edge)", bp && bl && bp->anchors() * 25 < bl->front().size(),
              count(bp) + " for " + std::to_string(bl ? bl->front().size() : 0) + " edge points");
    }

    // ── m3: what Make Path reads ────────────────────────────────────────────
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&] { return j.open(); });
        L::load_fixture(d);
        check("make path: nothing selected -- no selection source", !paths::source_mask(d, paths::Source::Selection), "");
        check("make path: a layer with no mask -- no mask source", !paths::source_mask(d, paths::Source::LayerMask, "L4"), "");
        check("make path: a group has no pixels -- no transparency source", !paths::source_mask(d, paths::Source::Transparency, "L2"), "");
        check("make path: no such saved selection", !paths::source_mask(d, paths::Source::Channel, "@channel/9"), "");
        // Transparency: "A" is a red square at (100, 100) 200 x 150.
        const auto tm = paths::source_mask(d, paths::Source::Transparency, "L4");
        const auto made = tm ? paths::from_mask(j, d, *tm, 2.0, "L4", paths::source_label(paths::Source::Transparency)) : std::string{};
        const auto l = made.empty() ? std::nullopt : d.get(made);
        bool at_corners = l && l->path && l->path->anchors() == 4;
        if (at_corners) {
            int hits = 0;
            for (const path::P want : {path::P{100, 100}, path::P{300, 100}, path::P{300, 250}, path::P{100, 250}})
                for (const auto& n : l->path->subs[0].nodes) hits += std::hypot(n.at.x - want.x, n.at.y - want.y) < 0.05 ? 1 : 0;
            at_corners = hits == 4;
        }
        check("make path: a layer's transparency -- its square, 4 corners where they are", at_corners, l && l->path ? count(*l->path) : "");
        check("make path: ... ONE step, \"Make Path from Transparency\", a path layer above the layer",
              j.undo_label().rfind("Make Path from Transparency", 0) == 0 && l && l->kind == core::Kind::Path && l->pos > d.get("L4")->pos, j.undo_label());
        // A layer mask: what it SHOWS.
        L::load_mask(d, "L4");   // shows only (120, 110) 60 x 60
        const auto lm = paths::source_mask(d, paths::Source::LayerMask, "L4");
        const auto mp = lm ? paths::traced(*lm, 2.0) : std::nullopt;
        const auto mb = mp ? path::box(*mp) : path::Box{};
        check("make path: a layer mask -- the part it shows (white reveals), not the part it hides",
              mp && mp->anchors() == 4 && std::abs(mb.x0 - 120) < 0.05 && std::abs(mb.x1 - 180) < 0.05 && std::abs(mb.y0 - 110) < 0.05, count(mp));
        // A saved selection.
        {
            core::Document::Loading load(d);
            core::Layer c;
            c.name = "Alpha 1";
            c.pos = 1;
            c.mask = std::make_shared<const P::TileMap>(S::ellipse_mask(d.mask_format(), d.image().width, d.image().height, 100, 80, 300, 240));
            d.replace_channels({{"@channel/1", c}});
        }
        const auto cm = paths::source_mask(d, paths::Source::Channel, "@channel/1");
        const auto cp = cm ? paths::traced(*cm, 1.0) : std::nullopt;
        check("make path: a saved selection -- its ellipse, 4 smooth anchors", cp && cp->anchors() == 4 && corners(*cp) == 0, count(cp));
        check("make path: an empty mask -- refused, no step",
              paths::from_mask(j, d, P::TileMap(d.mask_format(), d.image().width, d.image().height), 2.0, "", "Make Path").empty() &&
              j.undo_label().rfind("Make Path from Transparency", 0) == 0, j.undo_label());
    }
}
