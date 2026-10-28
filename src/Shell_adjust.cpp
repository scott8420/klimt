// Shell_adjust.cpp -- ADJUSTMENTS (s013). docs/adjustments.md.
//
//   * Layer > New Adjustment Layer (and the Layers panel's half-moon button):
//     one step; the new layer is picked, so Properties shows its controls;
//   * a Levels / Curves layer's histogram -- of what is below it -- is a job;
//   * Image > Adjustments: a dialog with a live preview on the layer, then a
//     job whose landing is the step.
#include "AdjustDialog.hpp"
#include "Log.hpp"
#include "Shell.hpp"
#include "core/Selection.hpp"

namespace klimt {

namespace L = core::layers;

void Shell::on_adjust_new(core::AdjustKind k) {
    if (!has_image()) return;
    const auto id = L::add_adjustment(m_journal, m_doc, k, m_layers.primary());
    if (id.empty()) { toast("New Adjustment Layer: refused"); return; }
    m_journal.reselect({id});
    m_layers.select({id}, false);
    if (m_doc.selection()) toast("“" + m_doc.get(id)->name + "” -- only inside the selection (its mask); paint the mask to change where");
}

void Shell::on_histogram(const core::Document::Id& id) {
    const auto im = m_doc.image();
    auto layers = std::make_shared<const core::Layers>(core::below(m_doc.snapshot(), id));
    start_job("Histogram", [this, id, im, layers](core::Jobs::Token& t) -> core::Jobs::Landing {
        const auto h = core::histogram_of(im, *layers, 64, [&t] { return t.cancelled(); });
        if (t.cancelled()) return [] {};
        return [this, id, h] { m_properties.set_histogram(id, h); };
    });
}

void Shell::on_adjust_apply(core::AdjustKind k) {
    const std::string verb(core::adjust::label(k));
    const auto id = m_layers.primary();
    const auto l = m_doc.get(id);
    if (!l) { toast(verb + ": select a layer first"); return; }
    if (l->kind != core::Kind::Pixel) {
        toast(verb + ": " + L::not_pixels(m_doc, id));
        return;
    }
    if (m_doc.locked_all(id) || (l->locks & core::lock::Pixels)) { toast(verb + ": “" + l->name + "” is locked"); return; }
    if (!l->pixels) { toast(verb + ": “" + l->name + "” is empty"); return; }
    const auto mode = m_doc.image().format.mode;
    if (mode != pixel::Mode::RGB && mode != pixel::Mode::Gray) {
        toast(verb + ": RGB and Gray documents only, for now (CMYK and Lab wait on colour management)");
        return;
    }
    if (!m_adjust_dialog) {
        m_adjust_dialog = std::make_unique<AdjustDialog>(*this);
        m_adjust_dialog->signal_changed().connect([this] {
            m_adjust_preview = m_adjust_dialog->preview() ? std::optional<core::Adjust>(m_adjust_dialog->value()) : std::nullopt;
            queue_render();
        });
        m_adjust_dialog->signal_closed().connect([this] {
            m_adjust_preview.reset();
            queue_render();
        });
        m_adjust_dialog->signal_ok().connect([this](const core::Adjust& a) {
            const auto target = m_adjust_target;
            const auto cur = m_doc.get(target);
            if (!cur || !cur->pixels || core::adjust::identity(a)) return;
            const auto before = cur->pixels;
            const auto sel = m_doc.selection_ptr();
            const std::string label(core::adjust::label(a.kind));
            start_job(label, [this, target, before, sel, a, label](core::Jobs::Token& t) -> core::Jobs::Landing {
                auto out = core::adjust::applied(*before, a, sel.get(), [&t] { return t.cancelled(); });
                if (!out || t.cancelled()) return [] {};
                auto res = std::make_shared<pixel::TileMap>(std::move(*out));
                return [this, target, before, sel, res, label] {
                    if (m_doc.selection_ptr() != sel || !L::set_pixels_if(m_journal, m_doc, target, before, std::move(*res), label))
                        toast(label + ": the layer changed meanwhile -- nothing done");
                };
            });
        });
    }
    m_adjust_target = id;
    m_adjust_preview = core::adjust::fresh(k);
    m_adjust_dialog->ask(k, l->name);
    // Levels / Curves: the layer's own histogram, behind the controls.
    if (k == core::AdjustKind::Levels || k == core::AdjustKind::Curves) {
        core::Layer alone = *l;
        alone.parent.clear();
        alone.visible = true;
        alone.opacity = alone.fill = 100;
        alone.blend = core::Blend::Normal;
        alone.clipped = false;
        auto layers = std::make_shared<const core::Layers>(core::Layers{{id, alone}});
        const auto im = m_doc.image();
        start_job("Histogram", [this, im, layers](core::Jobs::Token& t) -> core::Jobs::Landing {
            const auto h = core::histogram_of(im, *layers, 64, [&t] { return t.cancelled(); });
            if (t.cancelled()) return [] {};
            return [this, h] { if (m_adjust_dialog) m_adjust_dialog->set_histogram(h); };
        });
    }
    queue_render();
}

}  // namespace klimt
