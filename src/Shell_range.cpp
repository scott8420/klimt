// Shell_range.cpp -- SELECT > COLOR RANGE (s024). docs/selection.md.
//
//   * the dialog is not modal: the Eyedropper picks on the canvas and the
//     first sample follows the foreground colour;
//   * the preview is the range shown as Quick Mask's red (m_range_preview),
//     made on a worker a moment after the last change;
//   * OK: the mask as a job, landing as ONE step ("Color Range").
#include "ColourRangeDialog.hpp"
#include "Shell.hpp"
#include "core/ColourRange.hpp"

namespace klimt {

namespace {
std::array<float, 3> rgb_of(ToolRail::RGB c) { return {c.r, c.g, c.b}; }
}

void Shell::on_colour_range() {
    if (!has_image()) { toast("Color Range: no image"); return; }
    if (doc_busy(true)) return;
    if (!m_range_dialog) {
        m_range_dialog = std::make_unique<ColourRangeDialog>(*this);
        m_range_dialog->signal_changed().connect(sigc::mem_fun(*this, &Shell::range_preview_soon));
        m_range_dialog->signal_closed().connect([this] {
            if (m_range_soon.connected()) m_range_soon.disconnect();
            if (m_range_job) { m_jobs.cancel(m_range_job); m_range_job = 0; }
            if (m_range_preview) { m_range_preview.reset(); m_dirty.add_all(); queue_render(); }
        });
        m_range_dialog->signal_ok().connect(sigc::mem_fun(*this, &Shell::range_run));
        m_rail.signal_colours().connect([this] {
            if (m_range_dialog && m_range_dialog->get_visible()) m_range_dialog->follow(rgb_of(m_rail.foreground()));
        });
    }
    m_range_dialog->ask(rgb_of(m_rail.foreground()));
    on_tool("eyedropper");   // ready to pick
    m_rail.set_tool("eyedropper");
    range_preview_soon();
}

void Shell::range_preview_soon() {
    if (m_range_soon.connected()) m_range_soon.disconnect();
    if (m_range_job) { m_jobs.cancel(m_range_job); m_range_job = 0; }
    if (!m_range_dialog || !m_range_dialog->preview()) {
        if (m_range_preview) { m_range_preview.reset(); m_dirty.add_all(); queue_render(); }
        if (m_range_dialog) m_range_dialog->set_busy(false);
        return;
    }
    m_range_dialog->set_busy(true);
    m_range_soon = Glib::signal_timeout().connect([this] {
        if (!m_range_dialog) return false;
        const auto r = m_range_dialog->value();
        const auto op = m_range_dialog->op();
        const auto im = m_doc.image();
        auto layers = m_doc.snapshot();
        const auto mf = m_doc.mask_format();
        const auto sel = m_doc.selection_ptr();
        m_range_job = m_jobs.submit("Color Range", [this, r, op, im, layers = std::move(layers), mf, sel](core::Jobs::Token& t) -> core::Jobs::Landing {
            auto m = core::select::colour_range_mask(im, layers, r, mf, [&t] { return t.cancelled(); });
            if (!m || t.cancelled()) return [] {};
            auto shown = std::make_shared<const pixel::TileMap>(core::select::combine(sel.get(), *m, op));
            return [this, shown] {
                m_range_job = 0;
                if (!m_range_dialog || !m_range_dialog->get_visible() || !m_range_dialog->preview()) return;
                m_range_dialog->set_busy(false);
                m_range_preview = shown;
                m_dirty.add_all();
                queue_render();
            };
        });
        return false;
    }, 150);
}

void Shell::range_run() {
    if (!m_range_dialog) return;
    const auto r = m_range_dialog->value();
    const auto op = m_range_dialog->op();
    const auto im = m_doc.image();
    auto layers = m_doc.snapshot();
    const auto mf = m_doc.mask_format();
    const auto sel = m_doc.selection_ptr();
    start_job("Color Range", [this, r, op, im, layers = std::move(layers), mf, sel](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto m = core::select::colour_range_mask(im, layers, r, mf, [&t] { return t.cancelled(); });
        if (!m || t.cancelled()) return [] {};
        auto res = std::make_shared<pixel::TileMap>(core::select::combine(sel.get(), *m, op));
        return [this, res, sel] {
            if (m_doc.selection_ptr() != sel) { toast("Color Range: the selection changed meanwhile -- nothing done"); return; }
            if (!core::select::set(m_journal, m_doc, std::move(*res), "Color Range")) toast("Color Range: nothing in that range");
        };
    });
}

}  // namespace klimt
