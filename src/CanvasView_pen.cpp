// CanvasView -- the PEN (s035): drawing and editing the active path on the
// canvas, and drawing the path over the picture. docs/paths.md.
//
// The canvas keeps its own copy of the path while a gesture runs (a drag
// redraws without touching the document); the gesture's end tells the
// Shell the whole new path and a step name -- ONE step a gesture.
#include "CanvasView.hpp"

#include <algorithm>
#include <cmath>

namespace klimt {

namespace path = core::path;

namespace {
path::P add(path::P a, path::P b) { return {a.x + b.x, a.y + b.y}; }
path::P sub(path::P a, path::P b) { return {a.x - b.x, a.y - b.y}; }
path::P mirror(path::P at, path::P h) { return {2 * at.x - h.x, 2 * at.y - h.y}; }
}  // namespace

void CanvasView::set_path(const path::Path& p, bool show) {
    if (m_pen_drag) return;   // a gesture is under way: its own copy rules until it ends
    m_path = p;
    m_show_path = show;
    // What the Pen was drawing may be gone (an undo, another path picked).
    if (m_pen_sub >= static_cast<int>(m_path.subs.size()) || (m_pen_sub >= 0 && m_path.subs[static_cast<std::size_t>(m_pen_sub)].closed))
        m_pen_sub = -1;
    if (m_sel_sub >= static_cast<int>(m_path.subs.size()) ||
        (m_sel_sub >= 0 && m_sel_node >= static_cast<int>(m_path.subs[static_cast<std::size_t>(m_sel_sub)].nodes.size())))
        m_sel_sub = m_sel_node = -1;
    queue_draw();
}

bool CanvasView::pen_drawing() const { return m_pen_sub >= 0; }

void CanvasView::pen_finish() {
    m_pen_sub = -1;
    queue_draw();
}

void CanvasView::pen_emit(const std::string& label) {
    queue_draw();
    if (!(m_path == m_pen_before)) m_path_edit.emit(m_path, label);
}

bool CanvasView::pen_backspace() {
    m_pen_before = m_path;
    if (m_pen_sub >= 0) {
        auto& s = m_path.subs[static_cast<std::size_t>(m_pen_sub)];
        const int last = static_cast<int>(s.nodes.size()) - 1;
        const int sub = m_pen_sub;
        m_path = path::removed(m_path, sub, last);
        if (sub >= static_cast<int>(m_path.subs.size())) m_pen_sub = -1;   // its last anchor went
        m_sel_sub = m_pen_sub;
        m_sel_node = m_pen_sub >= 0 ? static_cast<int>(m_path.subs[static_cast<std::size_t>(m_pen_sub)].nodes.size()) - 1 : -1;
        pen_emit("Delete Anchor Point");
        return true;
    }
    if (m_sel_sub >= 0 && m_sel_node >= 0) {
        m_path = path::removed(m_path, m_sel_sub, m_sel_node);
        m_sel_sub = m_sel_node = -1;
        pen_emit("Delete Anchor Point");
        return true;
    }
    return false;
}

bool CanvasView::pen_set_smooth(bool smooth) {
    if (m_sel_sub < 0 || m_sel_node < 0 || m_sel_sub >= static_cast<int>(m_path.subs.size())) return false;
    const auto& s = m_path.subs[static_cast<std::size_t>(m_sel_sub)];
    if (m_sel_node >= static_cast<int>(s.nodes.size())) return false;
    m_pen_before = m_path;
    const auto& n = s.nodes[static_cast<std::size_t>(m_sel_node)];
    const bool corner = !n.smooth && n.in == n.at && n.out == n.at;
    if (smooth && !n.smooth) {
        if (corner) m_path = path::toggled(m_path, m_sel_sub, m_sel_node);      // new handles along the neighbours
        else m_path.subs[static_cast<std::size_t>(m_sel_sub)].nodes[static_cast<std::size_t>(m_sel_node)].smooth = true;
    } else if (!smooth && !corner) {
        m_path.subs[static_cast<std::size_t>(m_sel_sub)].nodes[static_cast<std::size_t>(m_sel_node)] = path::Node::corner(n.at);
    }
    pen_emit("Convert Point");
    return true;
}

void CanvasView::pen_press(double sx, double sy, bool ctrl, bool alt, bool shift) {
    path::P q{to_image_x(sx), to_image_y(sy)};
    const double tol = 6 / m_zoom;
    m_pen_before = m_path;
    m_pen_drag = true;
    m_pen_moved = false;
    m_pen_from = q;
    m_pen_mode = PenMode::None;
    m_pen_label.clear();
    m_pen_broke = false;
    const path::Hit h = path::hit(m_path, q, tol);
    m_pen_hit = h;
    using W = path::Hit;
    // s035 m5: the modes -- Select works as Ctrl does, Convert as Alt does.
    if (m_variant == 1) { ctrl = true; alt = false; m_pen_sub = -1; }
    else if (m_variant == 2) { alt = true; ctrl = false; m_pen_sub = -1; }
    if (ctrl) {   // move anchors and handles (Select: a click on nothing lets go of the picked point)
        if (h.what == W::None || h.what == W::Segment) m_sel_sub = m_sel_node = -1;
        if (h.what == W::Anchor || h.what == W::In || h.what == W::Out) {
            m_pen_mode = h.what == W::Anchor ? PenMode::Point : PenMode::Handle;
            m_pen_label = h.what == W::Anchor ? "Move Anchor Point" : "Move Handle";
            m_sel_sub = h.sub;
            m_sel_node = h.node;
        }
        queue_draw();
        return;
    }
    if (alt) {   // convert
        if (h.what == W::Anchor) {
            m_pen_mode = PenMode::Pull;
            m_pen_label = "Convert Point";
            m_sel_sub = h.sub;
            m_sel_node = h.node;
        } else if (h.what == W::In || h.what == W::Out) {
            auto& n = m_path.subs[static_cast<std::size_t>(h.sub)].nodes[static_cast<std::size_t>(h.node)];
            n.smooth = false;   // the handles part
            m_pen_before = m_path;
            m_pen_before.subs[static_cast<std::size_t>(h.sub)].nodes[static_cast<std::size_t>(h.node)].smooth = true;   // so a click alone still counts as a change
            m_pen_mode = PenMode::Handle;
            m_pen_label = "Convert Point";
            m_sel_sub = h.sub;
            m_sel_node = h.node;
        }
        queue_draw();
        return;
    }
    const bool drawing = m_pen_sub >= 0;
    if (drawing) {
        auto& s = m_path.subs[static_cast<std::size_t>(m_pen_sub)];
        if (h.what == W::Anchor && h.sub == m_pen_sub && h.node == 0 && s.nodes.size() >= 2) {   // close it
            s.closed = true;
            m_pen_mode = PenMode::Close;
            m_pen_label = "Close Path";
            m_sel_sub = m_pen_sub;
            m_sel_node = 0;
            queue_draw();
            return;
        }
        if (shift && !s.nodes.empty()) {   // 45-degree steps from the last anchor
            const path::P a = s.nodes.back().at;
            const double dx = q.x - a.x, dy = q.y - a.y, len = std::hypot(dx, dy);
            const double ang = std::round(std::atan2(dy, dx) / (M_PI / 4)) * (M_PI / 4);
            q = {a.x + len * std::cos(ang), a.y + len * std::sin(ang)};
        }
        s.nodes.push_back(path::Node::corner(q));
        m_sel_sub = m_pen_sub;
        m_sel_node = static_cast<int>(s.nodes.size()) - 1;
        m_pen_mode = PenMode::NewAnchor;
        m_pen_label = "Add Anchor Point";
        m_pen_from = q;
        queue_draw();
        return;
    }
    if (h.what == W::Anchor) {
        auto& s = m_path.subs[static_cast<std::size_t>(h.sub)];
        const int n = static_cast<int>(s.nodes.size());
        if (!s.closed && (h.node == n - 1 || h.node == 0)) {   // carry on from an open end
            if (h.node == 0 && n > 1) {
                std::reverse(s.nodes.begin(), s.nodes.end());
                for (auto& nd : s.nodes) std::swap(nd.in, nd.out);
                m_pen_before = m_path;   // the same path, drawn the other way: not a change
            }
            m_pen_sub = h.sub;
            m_sel_sub = h.sub;
            m_sel_node = static_cast<int>(s.nodes.size()) - 1;
            queue_draw();
            return;
        }
        // s035 m5 (Scott): a click on an anchor PICKS it -- its handles show,
        // a drag moves it, Corner / Smooth and Delete act on it.
        m_pen_mode = PenMode::Point;
        m_pen_label = "Move Anchor Point";
        m_sel_sub = h.sub;
        m_sel_node = h.node;
        queue_draw();
        return;
    }
    if (h.what == W::Segment) {   // a click on a segment adds an anchor there
        m_path = path::inserted(m_path, h.sub, h.node, h.t);
        m_pen_label = "Add Anchor Point";
        m_sel_sub = h.sub;
        m_sel_node = h.node + 1;
        queue_draw();
        return;
    }
    // A new subpath.
    m_path.subs.push_back({{path::Node::corner(q)}, false});
    m_pen_sub = static_cast<int>(m_path.subs.size()) - 1;
    m_sel_sub = m_pen_sub;
    m_sel_node = 0;
    m_pen_mode = PenMode::NewAnchor;
    m_pen_label = "Add Anchor Point";
    queue_draw();
}

void CanvasView::pen_drag(double sx, double sy, bool alt) {
    const path::P q{to_image_x(sx), to_image_y(sy)};
    const path::P d = sub(q, m_pen_from);
    if (std::hypot(d.x, d.y) * m_zoom >= 2) m_pen_moved = true;
    if (!m_pen_moved) return;
    using W = path::Hit;
    switch (m_pen_mode) {
        case PenMode::NewAnchor:
        case PenMode::Close: {
            auto& n = m_path.subs[static_cast<std::size_t>(m_sel_sub)].nodes[static_cast<std::size_t>(m_sel_node)];
            // The handle the pointer pulls, and its twin. Closing, the drag
            // shapes the curve ARRIVING at the first anchor (the twin is out).
            path::P& pulled = m_pen_mode == PenMode::Close ? n.in : n.out;
            path::P& twin = m_pen_mode == PenMode::Close ? n.out : n.in;
            const path::P target = m_pen_mode == PenMode::Close ? mirror(n.at, q) : q;
            pulled = target;
            if (alt) {   // Alt: a corner -- the twin stays where it is (Scott)
                m_pen_broke = true;
                n.smooth = false;
            } else if (!m_pen_broke) {   // a plain drag: mirrored, equal lengths
                twin = mirror(n.at, target);
                n.smooth = true;
            } else {   // Alt let go: smooth again -- the twin turns into line but keeps its own length
                const double tl = std::hypot(twin.x - n.at.x, twin.y - n.at.y);
                const double dx = target.x - n.at.x, dy = target.y - n.at.y, dl = std::hypot(dx, dy);
                if (dl > 1e-9 && tl > 1e-9) twin = {n.at.x - dx / dl * tl, n.at.y - dy / dl * tl};
                else if (tl <= 1e-9) twin = mirror(n.at, target);   // no twin yet (Alt from the start): mirror it
                n.smooth = true;
            }
            break;
        }
        case PenMode::Point:
            m_path = path::moved(m_pen_before, m_pen_hit.sub, m_pen_hit.node, W::Anchor, d);
            break;
        case PenMode::Handle: {
            path::Path base = m_pen_before;
            auto& bn = base.subs[static_cast<std::size_t>(m_pen_hit.sub)].nodes[static_cast<std::size_t>(m_pen_hit.node)];
            if (m_pen_label == "Convert Point") bn.smooth = false;
            m_path = path::moved(base, m_pen_hit.sub, m_pen_hit.node, m_pen_hit.what, d);
            break;
        }
        case PenMode::Pull: {
            m_path = m_pen_before;
            auto& n = m_path.subs[static_cast<std::size_t>(m_pen_hit.sub)].nodes[static_cast<std::size_t>(m_pen_hit.node)];
            n.out = q;
            n.in = mirror(n.at, q);
            n.smooth = true;
            break;
        }
        case PenMode::None: break;
    }
    queue_draw();
}

void CanvasView::pen_release() {
    if (!m_pen_drag) return;
    m_pen_drag = false;
    if (m_pen_mode == PenMode::Pull && !m_pen_moved)   // Alt-click: corner <-> smooth
        m_path = path::toggled(m_pen_before, m_pen_hit.sub, m_pen_hit.node);
    if (m_pen_mode == PenMode::Close) m_pen_sub = -1;   // closed: drawing ends
    m_pen_mode = PenMode::None;
    if (!m_pen_label.empty()) pen_emit(m_pen_label);
    else queue_draw();
}

void CanvasView::draw_path(const Cairo::RefPtr<Cairo::Context>& cr) {
    if (!m_show_path || m_path.empty()) return;
    const bool pen = m_tool == "pen";
    cr->save();
    // The outline: a light halo under a blue line, so it reads on any picture.
    const auto lines = path::outline(m_path, 0.3 / m_zoom);
    for (const auto& pts : lines) {
        if (pts.size() < 2) continue;
        cr->move_to(to_screen_x(pts[0].x), to_screen_y(pts[0].y));
        for (std::size_t i = 1; i < pts.size(); ++i) cr->line_to(to_screen_x(pts[i].x), to_screen_y(pts[i].y));
    }
    cr->set_line_join(Cairo::Context::LineJoin::ROUND);
    cr->set_line_width(3);
    cr->set_source_rgba(1, 1, 1, 0.55);
    cr->stroke_preserve();
    cr->set_line_width(1.25);
    cr->set_source_rgb(0.10, 0.42, 0.95);
    cr->stroke();
    // The Pen's rubber band: the next segment as it would be, to the pointer.
    if (pen && m_pen_sub >= 0 && !m_pen_drag && m_px >= 0) {
        const auto& s = m_path.subs[static_cast<std::size_t>(m_pen_sub)];
        if (!s.nodes.empty()) {
            const auto& a = s.nodes.back();
            cr->move_to(to_screen_x(a.at.x), to_screen_y(a.at.y));
            cr->curve_to(to_screen_x(a.out.x), to_screen_y(a.out.y), m_px, m_py, m_px, m_py);
            cr->set_line_width(1);
            cr->set_dash(std::vector<double>{4, 3}, 0);
            cr->set_source_rgba(0.10, 0.42, 0.95, 0.8);
            cr->stroke();
            cr->unset_dash();
        }
    }
    if (pen) {
        // Handles: the touched anchor's, and its neighbours'.
        const auto handles = [&](int si, int ni) {
            if (si < 0 || si >= static_cast<int>(m_path.subs.size())) return;
            const auto& s = m_path.subs[static_cast<std::size_t>(si)];
            const int n = static_cast<int>(s.nodes.size());
            if (n == 0) return;
            if (ni < 0 || ni >= n) { if (!s.closed) return; ni = (ni + n) % n; }
            const auto& nd = s.nodes[static_cast<std::size_t>(ni)];
            for (const path::P h : {nd.in, nd.out}) {
                if (h == nd.at) continue;
                const double hx = to_screen_x(h.x), hy = to_screen_y(h.y);
                cr->move_to(to_screen_x(nd.at.x), to_screen_y(nd.at.y));
                cr->line_to(hx, hy);
                cr->set_line_width(1);
                cr->set_source_rgba(0.10, 0.42, 0.95, 0.9);
                cr->stroke();
                cr->arc(hx, hy, 3.5, 0, 2 * M_PI);
                cr->set_source_rgb(1, 1, 1);
                cr->fill_preserve();
                cr->set_source_rgb(0.10, 0.42, 0.95);
                cr->stroke();
            }
        };
        if (m_sel_sub >= 0) {
            handles(m_sel_sub, m_sel_node - 1);
            handles(m_sel_sub, m_sel_node);
            handles(m_sel_sub, m_sel_node + 1);
        }
        // Anchors: small squares; the touched one filled.
        for (int si = 0; si < static_cast<int>(m_path.subs.size()); ++si) {
            const auto& s = m_path.subs[static_cast<std::size_t>(si)];
            for (int ni = 0; ni < static_cast<int>(s.nodes.size()); ++ni) {
                const auto& nd = s.nodes[static_cast<std::size_t>(ni)];
                const double x = std::round(to_screen_x(nd.at.x)) + 0.5, y = std::round(to_screen_y(nd.at.y)) + 0.5;
                const bool picked = si == m_sel_sub && ni == m_sel_node;
                cr->rectangle(x - 3.5, y - 3.5, 7, 7);
                if (picked) cr->set_source_rgb(0.10, 0.42, 0.95);
                else cr->set_source_rgb(1, 1, 1);
                cr->fill_preserve();
                cr->set_line_width(1);
                cr->set_source_rgb(0.10, 0.42, 0.95);
                cr->stroke();
            }
        }
    }
    cr->restore();
}

// ── s037: Isolate's strokes ──────────────────────────────────────────────────
void CanvasView::set_hints(const path::Hints& h, bool show) {
    if (h == m_hints && show == m_show_hints) return;
    m_hints = h;
    m_show_hints = show;
    queue_draw();
}

void CanvasView::set_hint_mode(int mode, double radius) {
    m_hint_mode = std::clamp(mode, 0, 2);
    m_hint_r = std::max(0.5, radius);
    if (!m_hint_mode) m_hint_drag = false;
    update_cursor();
    queue_draw();
}

void CanvasView::draw_hints(const Cairo::RefPtr<Cairo::Context>& cr) {
    const bool cur = m_hint_drag && !m_hint_cur.pts.empty();
    if ((!m_show_hints || m_hints.empty()) && !cur && !(m_hint_mode && m_px >= 0)) return;
    cr->save();
    // halo: a light rim 2 px wider, so a red stroke still reads on red (seen under Xvfb).
    const auto lay = [&](const path::Stroke& st, bool halo) {
        if (st.pts.empty()) return;
        cr->move_to(to_screen_x(st.pts[0].x), to_screen_y(st.pts[0].y));
        if (st.pts.size() == 1) cr->line_to(to_screen_x(st.pts[0].x), to_screen_y(st.pts[0].y));
        for (std::size_t i = 1; i < st.pts.size(); ++i) cr->line_to(to_screen_x(st.pts[i].x), to_screen_y(st.pts[i].y));
        cr->set_line_width(std::max(1.0, 2 * st.radius * m_zoom) + (halo ? 4 : 0));
        if (halo) cr->set_source_rgb(1, 1, 1);
        else if (st.figure) cr->set_source_rgb(0.16, 0.78, 0.32);
        else cr->set_source_rgb(0.92, 0.22, 0.20);
        cr->stroke();
    };
    // Opaque, in order, into a group (a later stroke covers an earlier one, as
    // Isolate reads them), the rims first under them all, then shown see-through.
    cr->push_group();
    cr->set_line_cap(Cairo::Context::LineCap::ROUND);
    cr->set_line_join(Cairo::Context::LineJoin::ROUND);
    for (const bool halo : {true, false}) {
        if (m_show_hints) for (const auto& st : m_hints.strokes) lay(st, halo);
        if (cur) lay(m_hint_cur, halo);
    }
    cr->pop_group_to_source();
    cr->paint_with_alpha(0.55);
    // The stroke's size at the pointer.
    if (m_hint_mode && m_px >= 0 && !m_panning) {
        const double r = std::max(2.0, m_hint_r * m_zoom);
        cr->arc(m_px, m_py, r, 0, 2 * M_PI);
        cr->set_line_width(2.5);
        cr->set_source_rgba(1, 1, 1, 0.7);
        cr->stroke_preserve();
        cr->set_line_width(1.25);
        const int mode = m_tool == "refine" && m_pen_alt ? 3 - m_hint_mode : m_hint_mode;   // s038: Alt held -- the Refine brush takes away
        if (mode == 1) cr->set_source_rgb(0.10, 0.60, 0.22);
        else cr->set_source_rgb(0.80, 0.14, 0.12);
        cr->stroke();
    }
    cr->restore();
}

}  // namespace klimt
