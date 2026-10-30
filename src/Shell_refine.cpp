// Shell -- REFINE (s038; Scott, s037 -> s038: "could it like pixelmator";
// "a crude pen boundary or selection could be the foundation. a click doesn't
// really give much context").
//
// Select > Refine to Edges (Ctrl+Alt+R; the strip's button with the Select
// and Refine tools): the selection -- a rough Lasso round the subject is
// enough -- or, with nothing selected, the picked path layer's area, snapped
// to the subject's real edge (core::isolate). The Refine brush (W) paints
// hints on top: a stroke says "this belongs", an Alt stroke "this doesn't";
// each runs the cut again from the SAME rough area with every stroke so far
// (core::refine::Session). Every run is a job, landing as ONE step.
// docs/selection.md "Refine".
#include "Shell.hpp"
#include "Log.hpp"
#include "core/Isolate.hpp"

namespace klimt {

namespace P = core::paths;

void Shell::on_refine_edges() {
    if (!has_image()) return;
    auto sel = m_doc.selection_ptr();
    if (sel) {
        m_refine.start(sel, sel);
        run_refine("Refine to Edges");
        return;
    }
    // Nothing selected: the picked path layer's area (and its s037 strokes).
    const auto id = path_layer();
    const auto p = id.empty() ? nullptr : P::of(m_doc, id);
    if (p && !p->empty()) {
        const auto& im = m_doc.image();
        const pixel::Format ff{pixel::Depth::F32, pixel::Mode::Gray, false};
        auto rough = std::make_shared<const pixel::TileMap>(P::coverage(*p, ff, im.width, im.height));
        const auto lay = m_doc.get(id);
        m_refine.start(rough, nullptr, lay && lay->hints ? *lay->hints : core::path::Hints{});
        run_refine("Refine to Edges");
        return;
    }
    toast("Refine to Edges: select roughly round the subject first -- the Lasso (L) is enough -- or paint over it with the Refine brush (W)");
}

void Shell::on_refine_stroke(const core::path::Stroke& st) {
    if (!has_image()) return;
    const auto now = m_doc.selection_ptr();
    const bool carried = m_refine.stroke(st, now);
    if (auto lg = log::get(log::Area::Shell))
        lg->info("refine stroke: {} r {} ({} points) -- {}, {} strokes", st.figure ? "add" : "take away", st.radius, st.pts.size(),
                 carried ? "carried on" : "a fresh session", m_refine.hints().strokes.size());
    if (!m_refine.rough() && !m_refine.hints().any(true)) {   // nothing to start from: Alt on nothing
        toast("Refine: paint over the subject first (Alt-painting takes away from a selection)");
        m_refine.reset();
        return;
    }
    run_refine("Refine Brush");
}

void Shell::run_refine(const std::string& label) {
    if (m_refine_job) m_jobs.cancel(m_refine_job);   // a newer stroke: the older run is moot
    const auto expect = m_doc.selection_ptr();
    const core::Image im = m_doc.image();
    const auto layers = m_doc.snapshot();
    const auto mf = m_doc.mask_format();
    const auto rough = m_refine.rough();
    auto hints = std::make_shared<const core::path::Hints>(m_refine.hints());
    const auto ro = m_strip.refine();
    // No rough area: the strokes alone are the hint (each looks
    // core::isolate::kStrokeReach round itself).
    const double band = ro.band;
    const double soft = ro.soft;
    m_refine_job = start_job(label, [this, im, layers, mf, rough, hints, band, soft, expect, label](core::Jobs::Token& t) -> core::Jobs::Landing {
        const auto pic = P::picture_for_isolate(im, layers);
        if (!pic || t.cancelled()) return {};
        core::isolate::Options o;
        o.band = band;
        o.soft = soft;
        o.hints = hints.get();
        const auto area = rough ? *rough : pixel::TileMap(mf, im.width, im.height);
        auto m = core::isolate::isolated(*pic, area, mf, o, [&t] { return t.cancelled(); }, [&t](double f) { t.progress(f); });
        if (t.cancelled()) return {};
        if (!m) return [this, label] {   // WEAKNESS 52: say so, never silently nothing
            m_refine_job = 0;
            toast(label + ": no subject found there -- paint over it with the Refine brush (W) to show what belongs");
        };
        return [this, expect, hints, label, mask = std::move(*m)]() mutable {
            m_refine_job = 0;
            const bool none = core::select::empty(mask);
            if (m_doc.selection_ptr() != expect) { toast(label + ": the selection changed meanwhile -- try again"); return; }
            const bool changed = core::select::set(m_journal, m_doc, none ? std::nullopt : std::optional<pixel::TileMap>(std::move(mask)), label);
            m_refine.landed(m_doc.selection_ptr(), *hints);   // unchanged: the same selection, now with these strokes
            if (none) toast(label + ": nothing left selected", true);
            else if (!changed) toast(label + ": no change");
        };
    });
}

}  // namespace klimt
