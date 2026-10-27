#include "Shell.hpp"
#include "Log.hpp"
#include "core/Transform.hpp"
#include "NumberDialog.hpp"

// Shell_transform.cpp -- TRANSFORM (s011 m2). Free Transform (Ctrl+T) and the
// one-shot flips and quarter turns. Scott, s010 -> s011: "transform selection
// if any else the whole layer" (core::xform does the choosing).
//
// Free Transform never writes the document until Enter. The plan is made on
// a worker; the canvas shows the box; each change of the box computes a
// PREVIEW (bilinear) on a worker, one job at a time, the latest box only, and
// the render shows the previewed layers in place of the real ones
// (m_preview). Enter computes the result (bicubic) on a worker and lands it
// as ONE step -- refused if the layers or the selection changed meanwhile.
// Esc, Ctrl+Z or an empty box drop it: nothing was ever written.

namespace klimt {

namespace Xf = core::xform;

namespace {
Xf::Affine affine_of(const std::array<double, 6>& m) { return {m[0], m[1], m[2], m[3], m[4], m[5]}; }
bool same(const Xf::Affine& a, const Xf::Affine& b) {
    return a.a == b.a && a.b == b.b && a.c == b.c && a.d == b.d && a.e == b.e && a.f == b.f;
}
}  // namespace

// The document still holds what the plan was made from?
bool Shell::xform_current(const core::xform::Plan& p) const {
    for (const auto& id : p.p.ids) {
        const auto l = m_doc.get(id);
        if (!l || l->pixels != p.p.original.at(id)) return false;
    }
    return m_doc.selection_ptr() == p.p.mask;
}

void Shell::on_free_transform() {
    if (m_xform.active || m_xform.planning || m_move.active || m_stroke.active) return;
    if (!has_image()) { toast("Free Transform: no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    const auto sel = m_layers.selection();
    if (const auto why = Xf::refusal(m_doc, sel); !why.empty()) { toast("Free Transform: " + why); return; }
    auto mp = core::move::plan(m_doc, sel, background_px());   // cheap, as the Move tool's press
    if (!mp) return;
    m_xform.planning = true;
    auto shared = std::make_shared<const core::move::Plan>(std::move(*mp));
    // The box needs a scan of what the layers hold: a worker's job.
    start_job("Free Transform", [this, shared](core::Jobs::Token&) -> core::Jobs::Landing {
        const auto box = Xf::box_of(*shared);
        return [this, shared, box] {
            m_xform.planning = false;
            if (box.empty()) { toast("Free Transform: nothing there to transform"); return; }
            auto p = std::make_shared<const Xf::Plan>(Xf::Plan{*shared, box});
            if (!xform_current(*p)) { toast("Free Transform: the layers changed meanwhile -- try again"); return; }
            m_xform = {};
            m_xform.active = true;
            m_xform.plan = p;
            m_canvas.set_outline(nullptr);   // the ants come back with the result
            m_canvas.begin_transform(box.x, box.y, box.w, box.h);
            m_canvas.grab_focus();
            toast("Free Transform: drag the box -- Enter applies, Esc cancels");
        };
    });
}

void Shell::on_xform_change(const std::array<double, 6>& m) {
    if (!m_xform.active || m_xform.committing) return;
    m_xform.want = affine_of(m);
    xform_pump();
}

void Shell::xform_pump() {
    if (!m_xform.active || m_xform.committing || m_xform.job) return;
    if (m_xform.has_done && same(m_xform.want, m_xform.done)) return;
    const auto want = m_xform.want;
    m_xform.job = m_jobs.submit("Transform", [this, plan = m_xform.plan, want](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto r = Xf::compute(*plan, want, Xf::Quality::Fast, [&t] { return t.cancelled(); });
        if (!r) return [this] { m_xform.job = 0; };
        auto res = std::make_shared<const core::move::Result>(std::move(*r));
        return [this, plan, res, want] {
            m_xform.job = 0;
            if (!m_xform.active || m_xform.plan != plan || m_xform.committing) return;
            m_preview = res->pixels;
            m_xform.done = want;
            m_xform.has_done = true;
            queue_render();
            xform_pump();
        };
    });
}

void Shell::xform_commit() {
    if (!m_xform.active || m_xform.committing) return;
    if (m_xform.want.identity()) { xform_cancel(); return; }
    m_xform.committing = true;
    if (m_xform.job) m_jobs.cancel(m_xform.job);
    m_xform.job = 0;
    const auto plan = m_xform.plan;
    const auto m = m_xform.want;
    start_job("Free Transform", [this, plan, m](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto r = Xf::compute(*plan, m, Xf::Quality::Best, [&t] { return t.cancelled(); });
        if (!r) return [this] { xform_end(); };
        auto res = std::make_shared<const core::move::Result>(std::move(*r));
        return [this, plan, res] {
            if (xform_current(*plan))
                m_journal.run("Free Transform", core::move::step_ids(plan->p), [&] { core::move::put(m_doc, plan->p, *res); });
            else
                toast("Free Transform: the layers changed meanwhile -- nothing done");
            xform_end();
        };
    });
}

void Shell::xform_cancel() {
    if (!m_xform.active) return;
    if (m_xform.job) m_jobs.cancel(m_xform.job);
    xform_end();
}

void Shell::xform_end() {
    m_xform = {};
    m_preview.clear();
    m_canvas.end_transform();
    m_ants_known = false;   // the ants come back (moved, if the selection was)
    queue_ants();
    queue_render();
}

// The one-shot transforms: plan, compute and land, as a job (one step).
void Shell::run_xform(const std::string& label, std::function<Xf::Affine(const pixel::Rect&)> map) {
    if (m_xform.active || m_move.active || m_stroke.active) return;
    if (!has_image()) { toast(label + ": no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    const auto sel = m_layers.selection();
    if (const auto why = Xf::refusal(m_doc, sel); !why.empty()) { toast(label + ": " + why); return; }
    auto mp = core::move::plan(m_doc, sel, background_px());
    if (!mp) return;
    auto shared = std::make_shared<const core::move::Plan>(std::move(*mp));
    start_job(label, [this, shared, map, label](core::Jobs::Token& t) -> core::Jobs::Landing {
        const auto box = Xf::box_of(*shared);
        if (box.empty()) return [this, label] { toast(label + ": nothing there to transform"); };
        auto plan = std::make_shared<const Xf::Plan>(Xf::Plan{*shared, box});
        auto r = Xf::compute(*plan, map(box), Xf::Quality::Best, [&t] { return t.cancelled(); });
        if (!r) return [] {};
        auto res = std::make_shared<const core::move::Result>(std::move(*r));
        return [this, plan, res, label] {
            if (!xform_current(*plan)) { toast(label + ": the layers changed meanwhile -- nothing done"); return; }
            m_journal.run(label, core::move::step_ids(plan->p), [&] { core::move::put(m_doc, plan->p, *res); });
        };
    });
}

// ── the whole canvas (s011 m3) ──────────────────────────────────────────────
void Shell::on_turn(core::xform::Turn t) {
    if (m_xform.active) return;
    run_canvas_job(Xf::turn_label(t), [t](const core::canvas::Before& b, core::Jobs::Token& tok) {
        return Xf::turn_plan(b, t, [&tok] { return tok.cancelled(); });
    });
}

void Shell::on_rotate_canvas() {
    if (m_xform.active) return;
    if (!has_image()) { toast("Rotate Canvas: no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (!m_rotate_dialog) {
        m_rotate_dialog = std::make_unique<NumberDialog>(*this, "shell.rotate_canvas", "Rotate Canvas",
                                                         "Angle (clockwise)", "°", -359.9, 359.9, 15, 1);
        m_rotate_dialog->signal_ok().connect([this](double deg) {
            const auto bg = background_px();
            run_canvas_job("Rotate Canvas", [deg, bg](const core::canvas::Before& b, core::Jobs::Token& tok) {
                return Xf::rotate_plan(b, deg, bg, [&tok] { return tok.cancelled(); });
            });
        });
    }
    m_rotate_dialog->ask();
}

void Shell::on_straighten(core::select::Pt a, core::select::Pt b) {
    if (m_xform.active) return;
    if (std::abs(Xf::straighten_angle(a, b)) < 0.01) { toast("Straighten: that line is already level"); return; }
    const auto bg = background_px();
    run_canvas_job("Straighten", [a, b, bg](const core::canvas::Before& before, core::Jobs::Token& tok) {
        return Xf::straighten_plan(before, a, b, bg, [&tok] { return tok.cancelled(); });
    });
}

}  // namespace klimt
