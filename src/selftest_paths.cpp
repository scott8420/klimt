// klimt_selftest -- PATHS (s035). docs/paths.md. The geometry (flatten, hit,
// split / move / toggle), a path's coverage against the selection's own
// shapes, the verbs and their steps, the canvas verbs carrying paths, the
// file, and a selection traced back into a path (corners kept, curves
// within tolerance, holes, specks).
#include "core/Canvas.hpp"
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Path.hpp"
#include "core/Paths.hpp"
#include "core/Selection.hpp"
#include "core/Transform.hpp"

#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

#include <unistd.h>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace S = klimt::core::select;
namespace path = klimt::core::path;
namespace paths = klimt::core::paths;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
std::string num(double v) { return std::to_string(v); }
P::Format mf8() { return {P::Depth::U8, P::Mode::Gray, false}; }
P::Format mff() { return {P::Depth::F32, P::Mode::Gray, false}; }

double area(const P::TileMap& m) {
    double a = 0;
    for (int y = 0; y < m.height(); ++y)
        for (int x = 0; x < m.width(); ++x) a += S::coverage(m, x, y);
    return a;
}
// Intersection over union of two masks (coverage as fuzzy membership).
double iou(const P::TileMap& a, const P::TileMap& b) {
    double i = 0, u = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const double p = S::coverage(a, x, y), q = S::coverage(b, x, y);
            i += std::min(p, q);
            u += std::max(p, q);
        }
    return u > 0 ? i / u : 1;
}
bool near(path::P a, path::P b, double eps) { return std::hypot(a.x - b.x, a.y - b.y) <= eps; }

struct Doc {
    core::Document d;
    core::DocJournal j{d};
    Doc() { d.set_door([this] { return j.open(); }); L::load_fixture(d); }
};
}  // namespace

void selftest_paths(const Check& check) {
    std::cout << "\n-- paths: geometry (s035) --\n";
    {
        const auto r = paths::rect_path(10, 20, 110, 70);
        check("path: a rectangle is 4 anchors, closed", r.anchors() == 4 && r.subs[0].closed && !r.empty(), "");
        const auto fl = path::flatten(r);
        check("flatten: straight segments stay 4 points", fl.size() == 1 && fl[0].size() == 4, num(fl.empty() ? 0 : fl[0].size()));
        const auto e = paths::ellipse_path(100, 100, 50, 50);
        bool on = true;
        const auto ef = path::flatten(e, 0.05);
        for (const auto& q : ef[0]) on = on && std::abs(std::hypot(q.x - 100, q.y - 100) - 50) < 0.05;
        check("flatten: an ellipse path's points lie on the circle (the Bezier circle's error is tiny)", on, "");
        const auto b = path::box(r);
        check("box", b.x0 == 10 && b.y0 == 20 && b.x1 == 110 && b.y1 == 70, "");
        // Hit testing.
        check("hit: an anchor", path::hit(r, {11, 21}, 3).what == path::Hit::Anchor && path::hit(r, {11, 21}, 3).node == 0, "");
        const auto hs = path::hit(r, {60, 21}, 3);
        check("hit: a segment, near its middle", hs.what == path::Hit::Segment && hs.node == 0 && std::abs(hs.t - 0.5) < 0.05, num(hs.t));
        check("hit: the closing segment", path::hit(r, {10.5, 45}, 3).what == path::Hit::Segment && path::hit(r, {10.5, 45}, 3).node == 3, "");
        check("hit: nothing far away", path::hit(r, {300, 300}, 3).what == path::Hit::None, "");
        const auto eh = path::hit(e, e.subs[0].nodes[0].out, 3);
        check("hit: a handle", eh.what == path::Hit::Out && eh.node == 0, "");
        // Insert keeps the curve's shape.
        const auto ins = path::inserted(e, 0, 0, 0.5);
        double worst = 0;
        const auto inf = path::flatten(ins, 0.05);
        for (const auto& q : inf[0]) worst = std::max(worst, std::abs(std::hypot(q.x - 100, q.y - 100) - 50));
        check("insert: an anchor split into a curve keeps its shape", ins.anchors() == 5 && worst < 0.06, num(worst));
        const auto ins2 = path::inserted(r, 0, 1, 0.25);
        check("insert: on a straight segment, a corner on the line", ins2.anchors() == 5 && near(ins2.subs[0].nodes[2].at, {110, 32.5}, 1e-9) &&
                                                                    ins2.subs[0].nodes[2].in == ins2.subs[0].nodes[2].at, "");
        check("remove: an anchor out", path::removed(r, 0, 2).anchors() == 3, "");
        check("remove: the last one takes the subpath", path::removed(path::removed(path::removed(path::removed(r, 0, 0), 0, 0), 0, 0), 0, 0).subs.empty(), "");
        // Move: an anchor carries its handles; a smooth handle turns its twin.
        const auto mv = path::moved(e, 0, 0, path::Hit::Anchor, {5, 0});
        check("move: an anchor carries its handles", near(mv.subs[0].nodes[0].in, {e.subs[0].nodes[0].in.x + 5, e.subs[0].nodes[0].in.y}, 1e-9), "");
        const auto mh = path::moved(e, 0, 0, path::Hit::Out, {10, 0});
        const auto& n = mh.subs[0].nodes[0];
        const double cross = (n.out.x - n.at.x) * (n.in.y - n.at.y) - (n.out.y - n.at.y) * (n.in.x - n.at.x);
        const double lin = std::hypot(n.in.x - n.at.x, n.in.y - n.at.y), lin0 = std::hypot(e.subs[0].nodes[0].in.x - n.at.x, e.subs[0].nodes[0].in.y - n.at.y);
        check("move: a smooth handle turns its twin, in line, same length", std::abs(cross) < 1e-6 && std::abs(lin - lin0) < 1e-9, num(cross));
        // Toggle.
        const auto tc = path::toggled(e, 0, 1);
        check("toggle: smooth -> corner (no handles)", !tc.subs[0].nodes[1].smooth && tc.subs[0].nodes[1].in == tc.subs[0].nodes[1].at, "");
        const auto ts = path::toggled(r, 0, 1);
        check("toggle: corner -> smooth (handles along the neighbours' chord)", ts.subs[0].nodes[1].smooth &&
              !(ts.subs[0].nodes[1].in == ts.subs[0].nodes[1].at), "");
        const auto mp = path::mapped(r, [](path::P q) { return path::P{q.x + 1, q.y * 2}; });
        check("mapped: every point", near(mp.subs[0].nodes[2].at, {111, 140}, 1e-12), "");
    }

    std::cout << "\n-- paths: coverage (s035) --\n";
    {
        const auto rc = paths::coverage(paths::rect_path(30, 40, 230, 140), mf8(), 400, 300);
        check("coverage: a pixel-aligned rectangle == the marquee's", iou(rc, S::rect_mask(mf8(), 400, 300, {30, 40, 200, 100})) == 1.0, "");
        const auto ec = paths::coverage(paths::ellipse_path(200, 150, 100, 60), mff(), 400, 300);
        const auto em = S::ellipse_mask(mff(), 400, 300, 100, 90, 300, 210);
        check("coverage: an ellipse path ~ the elliptical marquee", iou(ec, em) > 0.995, num(iou(ec, em)));
        check("coverage: its area ~ pi a b", std::abs(area(ec) - M_PI * 100 * 60) < 30, num(area(ec)));
        auto donut = paths::rect_path(50, 50, 250, 250);
        donut.subs.push_back(paths::rect_path(100, 100, 200, 200).subs[0]);
        const auto dc = paths::coverage(donut, mf8(), 300, 300);
        check("coverage: a subpath inside another is a hole (even-odd)", S::coverage(dc, 150, 150) == 0.f && S::coverage(dc, 75, 75) == 1.f, "");
        path::Path open;
        open.subs.push_back({{path::Node::corner({10, 10}), path::Node::corner({110, 10}), path::Node::corner({10, 110})}, false});
        const auto oc = paths::coverage(open, mf8(), 200, 200);
        check("coverage: an open subpath fills as if closed", S::coverage(oc, 30, 30) == 1.f && S::coverage(oc, 100, 100) == 0.f, "");
        check("coverage: an empty path covers nothing", S::empty(paths::coverage({}, mf8(), 100, 100)), "");
    }

    std::cout << "\n-- paths: path layers (s035 m6) --\n";
    {
        Doc t;
        auto& d = t.d;
        auto& j = t.j;
        const auto seen = [&](int x, int y) { return core::sample_composite(d.image(), d.snapshot(), x, y, 0); };
        const auto red_before = seen(250, 175);
        check("fixture: the red square shows at (250, 175)", red_before.r > 0.9 && red_before.g < 0.1, num(red_before.g));
        const auto id = L::add_path_layer(j, d, paths::rect_path(100, 100, 150, 250), "L4");
        const auto pl = d.get(id);
        check("new path layer: a Path layer, \"Path 1\", ONE step", pl && pl->kind == core::Kind::Path && pl->name == "Path 1" && pl->path &&
                                                                     j.size() == 1 && j.undo_label().rfind("New Path", 0) == 0, j.undo_label());
        check("new path layer: just above the picked layer", pl && pl->parent == d.get("L4")->parent && pl->pos > d.get("L4")->pos, "");
        check("new path layer: the tree's invariants hold", d.check().empty(), d.check());
        check("new path layer: draws nothing (the picture is the same)", seen(250, 175).g < 0.1 && seen(120, 175).g < 0.1, "");
        const auto id2 = L::add_path_layer(j, d, paths::ellipse_path(50, 50, 20, 20));
        check("new path layer: Path 2", d.get(id2)->name == "Path 2", d.get(id2)->name);
        check("set: its outline, ONE step", L::set_path_layer(j, d, id, paths::rect_path(100, 100, 160, 250), "Move Anchor Point") &&
                                            j.undo_label() == "Move Anchor Point", j.undo_label());
        check("set: the same again keeps no step", !L::set_path_layer(j, d, id, paths::rect_path(100, 100, 160, 250), "x"), "");
        check("set: refused on a pixel layer", !L::set_path_layer(j, d, "L4", paths::rect_path(0, 0, 9, 9), "x"), "");
        // Mask: the layer just below shows only inside the path.
        check("Mask on: one step", L::set_path_switches(j, d, id, true, false) && j.undo_label().rfind("Path as Mask", 0) == 0, j.undo_label());
        check("Mask: inside the path the red square still shows", seen(120, 175).r > 0.9 && seen(120, 175).g < 0.1, num(seen(120, 175).g));
        check("Mask: outside it, the square is hidden", seen(250, 175).g > 0.5, num(seen(250, 175).g));
        check("Mask: the layer's own pixels never change", d.get("L4")->pixels == t.d.get("L4")->pixels && !d.get("L4")->mask, "");
        L::set_path_layer(j, d, id, paths::rect_path(100, 100, 300, 250), "Move Anchor Point");
        check("Mask: live -- editing the path shows more", seen(250, 175).g < 0.1, num(seen(250, 175).g));
        core::Layer hidden = *d.get(id);
        L::set_visible(j, d, id, false);
        check("Mask: a hidden path layer masks nothing", seen(250, 175).g < 0.1, "");
        L::set_visible(j, d, id, true);
        L::set_path_layer(j, d, id, paths::rect_path(100, 100, 160, 250), "Move Anchor Point");
        // Selection: the selection is the path's area, and follows it.
        check("Selection on: the selection becomes the path's area, ONE step", L::set_path_switches(j, d, id, true, true) && d.selection() &&
              S::coverage(*d.selection(), 120, 175) == 1.f && S::coverage(*d.selection(), 250, 175) == 0.f, "");
        const auto before = j.size();
        check("Selection: an edit moves the selection too, in the same step", L::set_path_layer(j, d, id, paths::rect_path(100, 100, 300, 250), "Move Anchor Point") &&
              j.size() == before + 1 && S::coverage(*d.selection(), 250, 175) == 1.f, "");
        j.undo();
        check("Selection: undo puts both back", S::coverage(*d.selection(), 250, 175) == 0.f && path::box(*d.get(id)->path).x1 == 160, "");
        check("Selection off: the selection stays", L::set_path_switches(j, d, id, true, false) && d.selection(), "");
        // Load as Selection (by layer id) and Make Path from Selection.
        core::select::none(j, d);
        check("to selection: replace", paths::to_selection(j, d, id) && S::coverage(*d.selection(), 120, 175) == 1.f, "");
        check("to selection: add", paths::to_selection(j, d, id2, S::Op::Add) && S::coverage(*d.selection(), 50, 50) == 1.f &&
                                   j.undo_label() == "Add to Selection (Path)", j.undo_label());
        check("to selection: refused on a pixel layer", !paths::to_selection(j, d, "L4"), "");
        L::load_selection(d, {200, 40, 120, 50});
        const auto made = paths::from_selection(j, d, 2.0, "L5");
        check("Make Path from Selection: a new path layer, 4 corners, ONE step", !made.empty() && d.get(made)->path->anchors() == 4 &&
              j.undo_label().rfind("Make Path from Selection", 0) == 0, j.undo_label());
        const auto steps = j.size();
        for (std::size_t i = 0; i < steps; ++i) j.undo();
        check("undo: all the way back -- no path layers", !d.get(id) && !d.get(id2) && !d.get(made), "");
        for (std::size_t i = 0; i < steps; ++i) j.redo();
        check("redo: all the way forward", d.get(id) && d.get(made) && d.check().empty(), d.check());
        check("one door: no stray writes", d.strays() == 0, "");
        (void)hidden;
    }
    {
        // s035 (Scott: "the path goes into the group at the top ... it would run
        // the whole group"): an adjustment between the path and the pixels.
        Doc t;
        const auto seen = [&](int x, int y) { return core::sample_composite(t.d.image(), t.d.snapshot(), x, y, 0); };
        const auto adj = L::add_adjustment(t.j, t.d, core::AdjustKind::HueSat, "L4");
        auto a = t.d.get(adj)->adjust;
        a.hue = 120;
        L::set_adjust(t.j, t.d, adj, a);
        const auto pid = L::add_path_layer(t.j, t.d, paths::rect_path(100, 100, 150, 250), adj);
        L::set_path_switches(t.j, t.d, pid, true, false);
        const auto in = seen(120, 175), out = seen(250, 175);
        check("mask, group's top: inside -- the adjusted pixels", in.g > 0.9 && in.r < 0.1, num(in.r) + "," + num(in.g));
        check("mask, group's top: outside -- the whole group hidden (the pixels under the adjustment too)", out.r > 0.9 && out.g > 0.9 && out.b > 0.9,
              num(out.r) + "," + num(out.g) + "," + num(out.b));
        check("mask, group's top: masks every layer below it in the group", paths::masked_by(t.d.snapshot(), pid).size() == 3,
              num(static_cast<double>(paths::masked_by(t.d.snapshot(), pid).size())));
    }
    {
        // s035 (Scott: "sluggishness when drawing with the pen"): what a path
        // edit redraws.
        Doc t;
        L::load_path_layer(t.d, paths::rect_path(20, 20, 60, 60));
        const auto im = t.d.image();
        auto before = t.d.snapshot();
        L::set_path_layer(t.j, t.d, "L6", paths::rect_path(20, 20, 80, 70), "Move Anchor Point");
        auto ts = core::changed_tiles(im, before, im, t.d.snapshot());
        check("redraw: a path that masks nothing -- no tiles", !ts.all && ts.count() == 0, num(static_cast<double>(ts.count())));
        L::set_path_switches(t.j, t.d, "L6", true, false);
        before = t.d.snapshot();
        L::set_path_layer(t.j, t.d, "L6", paths::rect_path(20, 20, 90, 70), "Move Anchor Point");
        ts = core::changed_tiles(im, before, im, t.d.snapshot());
        check("redraw: a masking path's edit -- only the tiles under its outlines", !ts.all && ts.count() >= 1 && ts.has(0, 0) &&
              ts.count() < static_cast<std::size_t>(ts.cols * ts.rows), num(static_cast<double>(ts.count())));
        before = t.d.snapshot();
        L::set_visible(t.j, t.d, "L5", false);   // another layer: its own tiles, as ever
        ts = core::changed_tiles(im, before, im, t.d.snapshot());
        check("redraw: another layer's change -- not everything", !ts.all, "");
    }
    {
        // The canvas verbs carry path layers with the picture.
        Doc t;
        L::load_path_layer(t.d, paths::rect_path(100, 100, 200, 150), true);
        core::canvas::crop(t.j, t.d, {50, 40, 400, 300}, P::rgba(1, 1, 1, 1));
        check("crop: the path moves with the picture", near(t.d.get("L6")->path->subs[0].nodes[0].at, {50, 60}, 1e-9), "");
        t.j.undo();
        check("crop undone: back", near(t.d.get("L6")->path->subs[0].nodes[0].at, {100, 100}, 1e-9), "");
        const int w = t.d.image().width, h = t.d.image().height;
        core::canvas::image_size(t.j, t.d, w * 2, h * 2, t.d.image().dpi, core::canvas::Filter::Smooth);
        check("image size: the path scales", near(t.d.get("L6")->path->subs[0].nodes[2].at, {400, 300}, 1e-9), "");
        t.j.undo();
        core::xform::turn(t.j, t.d, core::xform::Turn::Clockwise);
        const auto b = path::box(*t.d.get("L6")->path);
        check("turn 90: the path turns (its box 50 x 100 now)", std::abs((b.x1 - b.x0) - 50) < 1e-6 && std::abs((b.y1 - b.y0) - 100) < 1e-6,
              num(b.x1 - b.x0) + " x " + num(b.y1 - b.y0));
    }
    {
        // The file keeps them.
        Doc t;
        auto curvy = paths::ellipse_path(120, 90, 70, 40);
        curvy.subs[0].nodes[1].smooth = false;
        curvy.subs.push_back({{path::Node::corner({1.5, 2.25}), {{30, 40}, {25, 35}, {35, 45}, true}}, false});
        L::load_path_layer(t.d, curvy, true, true);
        namespace fs = std::filesystem;
        const auto file = (fs::temp_directory_path() / ("klimt_paths_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = t.d.image();
        c.layers = t.d.snapshot();
        c.next_id = t.d.next_id();
        std::string err;
        check("file: saved", core::file::save_klimt(file, c, err), err);
        const auto back = core::file::open(file, err);
        const core::Layer* bl = back && back->layers.count("L6") ? &back->layers.at("L6") : nullptr;
        check("file: the path layer back exactly, with its switches", bl && bl->kind == core::Kind::Path && bl->path && *bl->path == curvy &&
                                                                      bl->path_mask && bl->path_select && bl->name == "Path 1", err);
        fs::remove(file);
    }

    std::cout << "\n-- paths: a selection traced into a path (s035 m2) --\n";
    {
        const auto rm = S::rect_mask(mf8(), 400, 300, {50, 60, 120, 80});
        const auto rp = paths::traced(rm, 2.0);
        bool corners = rp && rp->subs.size() == 1 && rp->anchors() == 4;
        if (corners) {
            int hits = 0;
            for (const path::P want : {path::P{50, 60}, path::P{170, 60}, path::P{170, 140}, path::P{50, 140}})
                for (const auto& n : rp->subs[0].nodes) if (near(n.at, want, 0.01)) ++hits;
            corners = hits == 4;
            for (const auto& n : rp->subs[0].nodes) corners = corners && !n.smooth && n.in == n.at && n.out == n.at;
        }
        check("trace: a marquee rectangle -> 4 corner anchors at its corners, straight sides", corners,
              rp ? num(static_cast<double>(rp->anchors())) : "nullopt");
        check("trace: ... and back to the same selection", rp && iou(paths::coverage(*rp, mf8(), 400, 300), rm) > 0.999,
              rp ? num(iou(paths::coverage(*rp, mf8(), 400, 300), rm)) : "");
        const auto em = S::ellipse_mask(mff(), 400, 300, 100, 80, 300, 220);
        const auto ep = paths::traced(em, 1.0);
        check("trace: an ellipse -> a few smooth anchors", ep && ep->subs.size() == 1 && ep->anchors() <= 12 && ep->anchors() >= 4,
              ep ? num(static_cast<double>(ep->anchors())) : "");
        check("trace: ... within tolerance (the selection back ~ the same)", ep && iou(paths::coverage(*ep, mff(), 400, 300), em) > 0.99,
              ep ? num(iou(paths::coverage(*ep, mff(), 400, 300), em)) : "");
        const auto loose = paths::traced(em, 8.0);
        check("trace: a looser tolerance, no more anchors", loose && ep && loose->anchors() <= ep->anchors(), "");
        // A hole and two islands.
        const auto ring = S::combine(&em, S::ellipse_mask(mff(), 400, 300, 160, 120, 240, 180), S::Op::Subtract);
        const auto rg = paths::traced(ring, 1.0);
        check("trace: a ring -> two subpaths, the hole kept", rg && rg->subs.size() == 2 && S::coverage(paths::coverage(*rg, mff(), 400, 300), 200, 150) == 0.f &&
                                                             S::coverage(paths::coverage(*rg, mff(), 400, 300), 120, 150) == 1.f, "");
        const auto two = S::combine(&rm, S::rect_mask(mf8(), 400, 300, {250, 200, 40, 40}), S::Op::Add);
        const auto tp = paths::traced(two, 2.0);
        check("trace: two islands -> two subpaths, top-left first", tp && tp->subs.size() == 2 && path::box({{tp->subs[0]}}).x0 == 50, "");
        const auto speck = S::combine(&rm, S::rect_mask(mf8(), 400, 300, {300, 20, 1, 1}), S::Op::Add);
        const auto sp = paths::traced(speck, 2.0);
        check("trace: a one-pixel speck is dropped", sp && sp->subs.size() == 1, sp ? num(static_cast<double>(sp->subs.size())) : "");
        // An L: six corners.
        const auto ell = S::combine(&rm, S::rect_mask(mf8(), 400, 300, {50, 140, 40, 100}), S::Op::Add);
        const auto lp = paths::traced(ell, 2.0);
        check("trace: an L shape -> 6 corners", lp && lp->subs.size() == 1 && lp->anchors() == 6, lp ? num(static_cast<double>(lp->anchors())) : "");
        check("trace: nothing selected -> an empty path", paths::traced(P::TileMap(mf8(), 100, 100), 2.0)->empty(), "");
        check("trace: cancelled -> nullopt", !paths::traced(em, 1.0, [] { return true; }), "");
        Doc t;
        check("Make Path from Selection: refused with nothing selected", paths::from_selection(t.j, t.d, 2.0).empty(), "");
    }
}

// ── s035 m7: the max-flow and Isolate ───────────────────────────────────────
#include "core/Isolate.hpp"
#include "core/Matte.hpp"
#include "core/MaxFlow.hpp"

#include <queue>

namespace {
// Edmonds-Karp on a dense matrix: the reference the max-flow is checked against.
double reference_flow(int n, std::vector<std::vector<double>> cap, int s, int t) {
    double flow = 0;
    for (;;) {
        std::vector<int> prev(static_cast<std::size_t>(n), -1);
        prev[static_cast<std::size_t>(s)] = s;
        std::queue<int> q;
        q.push(s);
        while (!q.empty() && prev[static_cast<std::size_t>(t)] < 0) {
            const int u = q.front();
            q.pop();
            for (int v = 0; v < n; ++v)
                if (prev[static_cast<std::size_t>(v)] < 0 && cap[static_cast<std::size_t>(u)][static_cast<std::size_t>(v)] > 1e-9) {
                    prev[static_cast<std::size_t>(v)] = u;
                    q.push(v);
                }
        }
        if (prev[static_cast<std::size_t>(t)] < 0) return flow;
        double b = 1e300;
        for (int v = t; v != s; v = prev[static_cast<std::size_t>(v)])
            b = std::min(b, cap[static_cast<std::size_t>(prev[static_cast<std::size_t>(v)])][static_cast<std::size_t>(v)]);
        for (int v = t; v != s; v = prev[static_cast<std::size_t>(v)]) {
            cap[static_cast<std::size_t>(prev[static_cast<std::size_t>(v)])][static_cast<std::size_t>(v)] -= b;
            cap[static_cast<std::size_t>(v)][static_cast<std::size_t>(prev[static_cast<std::size_t>(v)])] += b;
        }
        flow += b;
    }
}

// A textured figure: a disc of reddish noise on a light, mottled ground.
P::TileMap figure(int W, int H, double cx, double cy, double r) {
    auto m = P::TileMap(P::Format{}, W, H);
    std::uint32_t seed = 77;
    std::vector<P::TileMap::TileRef> grid(static_cast<std::size_t>(m.cols()) * static_cast<std::size_t>(m.rows()));
    for (int row = 0; row < m.rows(); ++row)
        for (int col = 0; col < m.cols(); ++col) {
            P::TileWriter w(P::Format{}, P::rgba(0, 0, 0, 1));
            for (int y = 0; y < P::kTile; ++y)
                for (int x = 0; x < P::kTile; ++x) {
                    const int X = col * P::kTile + x, Y = row * P::kTile + y;
                    if (X >= W || Y >= H) continue;
                    seed = seed * 1664525u + 1013904223u;
                    const float n = static_cast<float>((seed >> 24) % 21) / 255.f - 0.04f;
                    const bool in = std::hypot(X + 0.5 - cx, Y + 0.5 - cy) <= r;
                    const float mott = 0.04f * std::sin(X * 0.09f) * std::cos(Y * 0.07f);
                    w.set(x, y, in ? P::rgba(0.72f + n, 0.18f + n, 0.15f + n, 1) : P::rgba(0.82f + n + mott, 0.80f + n + mott, 0.70f + n, 1));
                }
            grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(m.cols()) + static_cast<std::size_t>(col)] = w.freeze();
        }
    return P::TileMap::from_tiles(P::Format{}, W, H, std::move(grid));
}
P::TileMap disc_mask(int W, int H, double cx, double cy, double r) {
    return S::ellipse_mask(mff(), W, H, cx - r, cy - r, cx + r, cy + r);
}
}  // namespace

void selftest_isolate(const Check& check) {
    std::cout << "\n-- paths: the max-flow (s035 m7) --\n";
    {
        core::MaxFlow g;
        g.add_node(); g.add_node();
        g.add_tweights(0, 5, 1);
        g.add_tweights(1, 2, 6);
        g.add_edge(0, 1, 3, 4);
        const double f = g.maxflow();
        check("max-flow: the two-node case (flow 6, node 0 with the source, 1 with the sink)", std::abs(f - 6) < 1e-6 && g.source_side(0) && !g.source_side(1), num(f));
        std::uint32_t seed = 3;
        const auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<double>((seed >> 8) % 1000) / 100.0; };
        bool all = true;
        std::string worst;
        for (int trial = 0; trial < 40 && all; ++trial) {
            const int n = 4 + trial % 9;   // plus source and sink
            std::vector<std::vector<double>> cap(static_cast<std::size_t>(n + 2), std::vector<double>(static_cast<std::size_t>(n + 2), 0));
            core::MaxFlow mf;
            for (int i = 0; i < n; ++i) mf.add_node();
            for (int i = 0; i < n; ++i) {
                const double a = (seed & 3) ? rnd() : 0, b = (seed & 5) ? rnd() : 0;
                mf.add_tweights(i, static_cast<float>(a), static_cast<float>(b));
                cap[static_cast<std::size_t>(n)][static_cast<std::size_t>(i)] += a;
                cap[static_cast<std::size_t>(i)][static_cast<std::size_t>(n + 1)] += b;
            }
            for (int e = 0; e < n * 2; ++e) {
                const int i = static_cast<int>(rnd() * 100) % n, k = static_cast<int>(rnd() * 100) % n;
                if (i == k) continue;
                const double c1 = rnd(), c2 = rnd();
                mf.add_edge(i, k, static_cast<float>(c1), static_cast<float>(c2));
                cap[static_cast<std::size_t>(i)][static_cast<std::size_t>(k)] += c1;
                cap[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)] += c2;
            }
            const double want = reference_flow(n + 2, cap, n, n + 1), got = mf.maxflow();
            if (std::abs(want - got) > 1e-3 * std::max(1.0, want)) { all = false; worst = num(want) + " vs " + num(got); }
        }
        check("max-flow: 40 random graphs agree with Edmonds-Karp", all, worst);
    }

    std::cout << "\n-- paths: Isolate -- the path as a hint (s035 m7) --\n";
    {
        const int W = 300, H = 300;
        const auto pic = figure(W, H, 150, 150, 70);
        const auto truth = disc_mask(W, H, 150, 150, 70);
        // A loose path: about 10 px outside the figure, not quite round.
        const auto loose = paths::ellipse_path(152, 149, 81, 78);
        const double path_iou = iou(paths::coverage(loose, mff(), W, H), truth);
        core::isolate::Options o;
        o.band = 16;
        const auto a = core::isolate::isolated(pic, loose, mff(), o);
        check("isolate: a loose path -> the figure (closer than the path itself)", a && iou(*a, truth) > 0.97 && iou(*a, truth) > path_iou + 0.1,
              a ? num(iou(*a, truth)) + " (the path " + num(path_iou) + ")" : "nullopt");
        // A tight one: inside the figure.
        const auto tight = paths::ellipse_path(149, 151, 62, 64);
        const auto b = core::isolate::isolated(pic, tight, mff(), o);
        check("isolate: a path inside the figure -> the figure still", b && iou(*b, truth) > 0.97, b ? num(iou(*b, truth)) : "nullopt");
        // A rough polygon around it.
        path::Path rough;
        rough.subs.push_back({{path::Node::corner({70, 85}), path::Node::corner({160, 68}), path::Node::corner({232, 110}),
                               path::Node::corner({228, 205}), path::Node::corner({150, 233}), path::Node::corner({72, 200})}, true});
        o.band = 20;
        const auto c = core::isolate::isolated(pic, rough, mff(), o);
        check("isolate: a rough hexagon round it -> the round figure", c && iou(*c, truth) > 0.95, c ? num(iou(*c, truth)) : "nullopt");
        // A plain box round a round figure: its corners far beyond any band (seen
        // under Xvfb: the first Isolate took the box's corners as sure figure).
        o.band = 12;
        const auto box = core::isolate::isolated(pic, paths::rect_path(65, 65, 235, 235), mff(), o);
        check("isolate: a box round a disc -> the disc (the corners are background)", box && iou(*box, truth) > 0.96 &&
              S::coverage(*box, 75, 75) == 0.f, box ? num(iou(*box, truth)) : "nullopt");
        check("isolate: an edge pixel is soft (anti-aliased)", a && [&] {
            for (int x = 70; x < 90; ++x) { const float v = S::coverage(*a, x, 150); if (v > 0.05f && v < 0.95f) return true; }
            return false;
        }(), "");
        check("isolate: cancelled -> nullopt", !core::isolate::isolated(pic, loose, mff(), o, [] { return true; }), "");
        check("isolate: an empty path -> nullopt", !core::isolate::isolated(pic, {}, mff(), o), "");
        // The verb, on a document holding the picture.
        Doc t;
        {
            core::Document::Loading load(t.d);
            core::Image im = t.d.image();
            im.width = W; im.height = H; im.format = P::Format{};
            core::Layer bg;
            bg.name = "Background";
            bg.pixels = std::make_shared<const P::TileMap>(pic);
            t.d.replace(im, {{"L1", bg}}, 2);
        }
        const auto id = L::add_path_layer(t.j, t.d, loose, "L1");
        L::set_path_switches(t.j, t.d, id, true, false);   // masking: isolate still reads the picture whole
        check("Isolate (verb): ONE step, the selection ~ the figure", core::paths::isolate(t.j, t.d, id, 16) && t.j.undo_label() == "Isolate (Path)" &&
              t.d.selection() && iou(*t.d.selection(), S::ellipse_mask(t.d.mask_format(), W, H, 80, 80, 220, 220)) > 0.95, t.j.undo_label());
    }

    // s036 m3 -- the SOFT EDGE: a blurred figure edge (hair, soft focus) --
    // coverage read from colour where the hard cut can only say yes or no.
    std::cout << "\n-- paths: Isolate's soft edge (s036) --\n";
    {
        const int W = 300, H = 200;
        // Red (x < 140) ramping to green over 20 px (x 140..160): a soft edge.
        P::TileMap pic(P::Format{}, W, H);
        pic = pic.paint({0, 0, W, H}, [](int x, int, const P::Px&) {
            const float k = std::clamp((x + 0.5f - 140) / 20.f, 0.f, 1.f);
            return P::rgba(1 - k, k, 0, 1);
        });
        const auto hard = S::rect_mask(mf8(), W, H, {0, 0, 150, H});   // the cut, mid-ramp
        const auto soft = core::matte::soft_edge(pic, hard, 16);
        check("soft edge: made", soft.has_value(), "");
        if (soft) {
            const float at145 = S::coverage(*soft, 145, 100), at155 = S::coverage(*soft, 155, 100);
            check("soft edge: a quarter in -- about 3/4 figure", std::abs(at145 - 0.75f) < 0.08f, num(at145));
            check("soft edge: a quarter out -- about 1/4 figure", std::abs(at155 - 0.25f) < 0.08f, num(at155));
            check("soft edge: well inside / outside as the cut", S::coverage(*soft, 60, 100) > 0.99f && S::coverage(*soft, 250, 100) < 0.01f, "");
        }
        // A cut where figure and background are one colour: nothing to read -- the hard edge stands.
        P::TileMap flat(P::Format{}, W, H);
        flat = flat.fill({0, 0, W, H}, P::rgba(0.5f, 0.5f, 0.5f, 1));
        const auto same = core::matte::soft_edge(flat, hard, 16);
        check("soft edge: alike colours keep the hard edge", same && S::coverage(*same, 145, 100) > 0.99f && S::coverage(*same, 155, 100) < 0.01f, "");
        // Through Isolate: a loose path round the textured disc, soft edge on.
        const auto pf = figure(W, H, 150, 100, 60);
        const auto path = core::paths::rect_path(70, 20, 230, 180);
        core::isolate::Options o;
        o.soft = 6;
        const auto m = core::isolate::isolated(pf, path, mf8(), o);
        check("soft edge: Isolate takes it -- the figure still found", m && iou(*m, disc_mask(W, H, 150, 100, 60)) > 0.9, m ? num(iou(*m, disc_mask(W, H, 150, 100, 60))) : "");
    }
}
