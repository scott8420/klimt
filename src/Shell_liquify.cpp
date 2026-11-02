#include "Shell.hpp"
#include "Log.hpp"

// Shell_liquify.cpp -- FILTER > LIQUIFY (s076). docs/liquify.md.
//
// Liquify is a mode over the canvas, as Free Transform is: the picked pixel
// layer, a displacement field (core::liquify::Field) the brushes reshape on
// the UI thread (a dab touches only the nodes under the brush), and a
// PREVIEW computed on a worker -- the layer through the field, bilinear,
// over what was touched; one job at a time, the newest field only. Nothing is
// written until Enter / Apply: then the layer through the field, bicubic, on
// a worker, lands as ONE step "Liquify" -- refused if the layer changed
// meanwhile. Esc / Cancel drop it: nothing was ever written.

namespace klimt {

namespace Lq = core::liquify;

void Shell::on_liquify() {
    if (m_liq.active || m_xform.active || m_move.active || m_stroke.active) return;
    if (!has_image()) { toast("Liquify: no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    const auto& sel = m_layers.selection();
    if (sel.size() != 1) { toast("Liquify: pick one pixel layer"); return; }
    const auto id = sel.front();
    if (const auto why = Lq::refusal(m_doc, id); !why.empty()) { toast("Liquify: " + why); return; }
    m_liq = {};
    m_liq.active = true;
    m_liq.id = id;
    m_liq.original = m_doc.get(id)->pixels;
    m_liq.field = Lq::Field(m_doc.image().width, m_doc.image().height);
    m_canvas.set_outline(nullptr);   // no ants while brushing
    m_canvas.begin_liquify(m_strip.liquify().size / 2.0);
    m_strip.show_liquify();
    m_canvas.grab_focus();
    if (auto lg = log::get(log::Area::Shell)) lg->info("liquify: {} ({} x {} field)", id, m_doc.image().width, m_doc.image().height);
    toast("Liquify: brush the picture -- Enter applies, Esc cancels");
}

void Shell::on_liquify_dab(CanvasView::LiqAt at, core::select::Pt c, core::select::Pt motion, double pressure, bool alt) {
    if (!m_liq.active || m_liq.committing) return;
    if (at == CanvasView::LiqAt::End) return;
    const auto& o = m_strip.liquify();
    const auto tool = static_cast<Lq::Tool>(o.tool);
    if (tool == Lq::Tool::Forward && motion.x == 0 && motion.y == 0) return;   // a push needs a direction
    Lq::Brush b;
    b.tool = tool;
    b.radius = o.size / 2.0;
    b.pressure = std::clamp(o.pressure / 100.0 * pressure, 0.0, 1.0);
    b.rate = o.rate / 100.0;
    m_liq.field.dab(b, c, motion, alt);
    ++m_liq.gen;
    liquify_pump();
}

void Shell::liquify_pump() {
    if (!m_liq.active || m_liq.committing || m_liq.job) return;
    if (m_liq.done_gen == m_liq.gen) return;
    auto field = std::make_shared<const Lq::Field>(m_liq.field);   // the newest, as it is now
    const unsigned gen = m_liq.gen;
    const auto src = m_liq.original;
    const auto id = m_liq.id;
    m_liq.job = m_jobs.submit("Liquify", [this, field, gen, src, id](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto r = Lq::apply(*src, *field, core::xform::Quality::Fast, [&t] { return t.cancelled(); });
        if (!r) return [this] { m_liq.job = 0; };
        auto px = std::make_shared<const pixel::TileMap>(std::move(*r));
        return [this, px, gen, id] {
            m_liq.job = 0;
            if (!m_liq.active || m_liq.committing || m_liq.id != id) return;
            m_preview.clear();
            if (!m_liq.field.identity()) m_preview[id] = px;
            m_liq.done_gen = gen;
            queue_render();
            liquify_pump();
        };
    });
}

void Shell::liquify_commit() {
    if (!m_liq.active || m_liq.committing) return;
    if (m_liq.field.identity()) { liquify_cancel(); return; }
    m_liq.committing = true;
    if (m_liq.job) m_jobs.cancel(m_liq.job);
    m_liq.job = 0;
    auto field = std::make_shared<const Lq::Field>(m_liq.field);
    const auto src = m_liq.original;
    const auto id = m_liq.id;
    start_job("Liquify", [this, field, src, id](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto r = Lq::apply(*src, *field, core::xform::Quality::Best, [&t] { return t.cancelled(); });
        if (!r) return [this] { liquify_end(); };
        auto px = std::make_shared<const pixel::TileMap>(std::move(*r));
        return [this, px, src, id] {
            const auto l = m_doc.get(id);
            if (l && l->pixels == src)
                m_journal.run("Liquify", {id}, [&] {
                    auto nl = *m_doc.get(id);
                    nl.pixels = px;
                    m_doc.put(id, nl);
                });
            else
                toast("Liquify: the layer changed meanwhile -- nothing done");
            liquify_end();
        };
    });
}

void Shell::liquify_cancel() {
    if (!m_liq.active) return;
    if (m_liq.job) m_jobs.cancel(m_liq.job);
    liquify_end();
}

void Shell::liquify_end() {
    m_liq = {};
    m_preview.clear();
    m_canvas.end_liquify();
    if (const auto* t = core::find_tool(m_rail.tool())) m_strip.show_tool(*t);
    m_ants_known = false;
    queue_ants();
    queue_render();
}

}  // namespace klimt
