#include "Shell.hpp"
#include "Log.hpp"
#include "core/Composite.hpp"

// Shell_move.cpp -- MOVE (s009 m1). The Move tool's drag and the arrow keys.
//
// The press makes a core::move::Plan (the targets' pixels and the selection
// as they are) and opens a journal GESTURE -- the whole drag is one step.
// Pointer moves only change the offset WANTED; the pixel work runs on a
// worker, ONE job at a time, always from the plan's originals, so the latest
// offset is all that is ever computed and nothing is lost off the edge until
// the release. The release commits once the work for the final offset has
// landed. (The Brush's pipeline, Shell_paint.cpp.)
//
// The arrows (1 px; Shift 10) are steps of their own, folded by the journal's
// merge window; a run of nudges keeps its plan, so it too is computed from
// the pixels before the first nudge.

namespace klimt {

namespace Mv = core::move;

namespace {
std::string nudge_key(const Mv::Plan& p) {
    std::string k = "nudge";
    for (const auto& id : p.ids) k += ":" + id;
    return k;
}
}  // namespace

pixel::Px Shell::background_px() const {
    const auto bg = m_rail.background();
    return core::from_srgb(m_doc.image().format, bg.r, bg.g, bg.b);
}

void Shell::on_move(CanvasView::MoveAt at, int dx, int dy) {
    using A = CanvasView::MoveAt;
    if (at == A::Begin) {
        if (m_move.active || m_stroke.active) return;
        const auto sel = m_layers.selection();
        if (const auto why = Mv::refusal(m_doc, sel); !why.empty()) { toast("Move: " + why); return; }
        auto p = Mv::plan(m_doc, sel, background_px());
        if (!p) return;
        m_nudge = {};   // a drag ends any run of nudges
        m_move = {};
        m_move.active = true;
        m_move.gesture = true;
        m_move.plan = std::make_shared<const Mv::Plan>(std::move(*p));
        m_journal.begin("Move", Mv::step_ids(*m_move.plan));
        return;
    }
    if (!m_move.active || !m_move.gesture) return;
    m_move.want_x = dx;
    m_move.want_y = dy;
    if (at == A::End) m_move.ending = true;
    move_pump();
}

void Shell::on_nudge(int dx, int dy) {
    if (m_stroke.active || (m_move.active && m_move.gesture)) return;
    if (m_move.active && m_nudge.plan && m_move.plan == m_nudge.plan) {
        // Keys faster than the work: they add up; the next job takes the sum.
        m_nudge.x += dx;
        m_nudge.y += dy;
        m_move.want_x = m_nudge.x;
        m_move.want_y = m_nudge.y;
        move_pump();
        return;
    }
    const auto sel = m_layers.selection();
    // Still the same run? The layers (and the selection) hold what the last
    // nudge landed, and nothing else has been done since.
    bool same = m_nudge.plan && m_nudge.sel == sel && m_doc.selection_ptr() == m_nudge.mask;
    if (same)
        for (const auto& [id, px] : m_nudge.landed) {
            const auto l = m_doc.get(id);
            if (!l || l->pixels != px) { same = false; break; }
        }
    if (!same) {
        if (const auto why = Mv::refusal(m_doc, sel); !why.empty()) { toast("Move: " + why); return; }
        auto p = Mv::plan(m_doc, sel, background_px());
        if (!p) return;
        m_nudge = {};
        m_nudge.plan = std::make_shared<const Mv::Plan>(std::move(*p));
        m_nudge.sel = sel;
        m_nudge.mask = m_doc.selection_ptr();
        for (const auto& id : m_nudge.plan->ids) m_nudge.landed[id] = m_doc.get(id)->pixels;
    }
    m_nudge.x += dx;
    m_nudge.y += dy;
    m_move = {};
    m_move.active = true;
    m_move.gesture = false;
    m_move.ending = true;
    m_move.plan = m_nudge.plan;
    m_move.want_x = m_nudge.x;
    m_move.want_y = m_nudge.y;
    m_move.done_x = m_move.done_y = 0x7fffffff;   // not landed yet: always compute
    move_pump();
}

void Shell::move_pump() {
    if (!m_move.active || m_move.job) return;
    if (m_move.want_x == m_move.done_x && m_move.want_y == m_move.done_y) {
        if (!m_move.ending) return;
        if (m_move.gesture && m_journal.in_gesture()) m_journal.commit();
        m_move = {};
        return;
    }
    const int dx = m_move.want_x, dy = m_move.want_y;
    m_move.job = m_jobs.submit("Move", [this, plan = m_move.plan, dx, dy](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto r = Mv::compute(*plan, dx, dy, [&t] { return t.cancelled(); });
        if (!r) return [this] { m_move.job = 0; m_move = {}; };
        auto res = std::make_shared<const Mv::Result>(std::move(*r));
        return [this, plan, res, dx, dy] {
            m_move.job = 0;
            if (!m_move.active || m_move.plan != plan) return;
            if (m_move.gesture) {
                // An undo mid-drag closed the gesture: drop the work, nothing
                // is written around the door.
                if (!m_journal.in_gesture()) { m_move = {}; return; }
                Mv::put(m_doc, *plan, *res);
            } else {
                // A nudge: a step of its own, folded with the run's others.
                if (!m_journal.in_gesture())
                    m_journal.run("Move", Mv::step_ids(*plan), [&] { Mv::put(m_doc, *plan, *res); }, nudge_key(*plan));
                if (m_nudge.plan == plan) {
                    for (const auto& [id, px] : res->pixels) m_nudge.landed[id] = px;
                    m_nudge.mask = m_doc.selection_ptr();
                }
            }
            m_move.done_x = dx;
            m_move.done_y = dy;
            move_pump();
        };
    });
}

}  // namespace klimt
