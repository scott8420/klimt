// Shell_channels.cpp -- THE COLOUR CHANNELS in the Channels tab (s026).
// docs/selection.md.
//   * a channel clicked: the canvas shows it alone, as grey (view state);
//   * Ctrl-click / its button: loaded as a selection -- a job, ONE step;
//   * its copy button: a saved channel of it -- a job, ONE step.
#include "Shell.hpp"
#include "core/ColourChannels.hpp"

namespace klimt {

namespace S = core::select;

void Shell::on_channel_view(int k) {
    if (!has_image()) return;
    const auto names = S::colour_channels(m_doc.image().format);
    const int want = (k == m_channel_view || k < 0 || k >= static_cast<int>(names.size())) ? -1 : k;
    if (want == m_channel_view) { update_channels(); return; }
    m_channel_view = want;
    m_dirty = {};   // every tile again, the other way
    m_shown = {};
    queue_render();
    update_channels();
    if (want >= 0) toast("Viewing the " + names[static_cast<std::size_t>(want)] + " channel -- click the picture's row to go back");
}

void Shell::on_colour_load(int k, int op) {
    if (!has_image() || doc_busy(true)) return;
    const auto names = S::colour_channels(m_doc.image().format);
    if (k < 0 || k >= static_cast<int>(names.size())) return;
    const auto im = m_doc.image();
    auto layers = m_doc.snapshot();
    const auto mf = m_doc.mask_format();
    const auto sel = m_doc.selection_ptr();
    const std::string label = "Load " + names[static_cast<std::size_t>(k)] + " Channel";
    start_job(label, [this, k, op, im, layers = std::move(layers), mf, sel, label](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto m = S::colour_channel_mask(im, layers, k, mf, [&t] { return t.cancelled(); });
        if (!m || t.cancelled()) return [] {};
        auto res = std::make_shared<pixel::TileMap>(S::combine(sel.get(), *m, static_cast<S::Op>(op)));
        return [this, res, sel, label] {
            if (m_doc.selection_ptr() != sel) { toast(label + ": the selection changed meanwhile -- nothing done"); return; }
            if (!S::set(m_journal, m_doc, std::move(*res), label)) toast(label + ": nothing to select");
        };
    });
}

void Shell::on_colour_dup(int k) {
    if (!has_image() || doc_busy(true)) return;
    const auto names = S::colour_channels(m_doc.image().format);
    if (k < 0 || k >= static_cast<int>(names.size())) return;
    const auto im = m_doc.image();
    auto layers = m_doc.snapshot();
    const auto mf = m_doc.mask_format();
    const std::string name = names[static_cast<std::size_t>(k)];
    start_job("Duplicate " + name + " Channel", [this, k, im, layers = std::move(layers), mf, name](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto m = S::colour_channel_mask(im, layers, k, mf, [&t] { return t.cancelled(); });
        if (!m || t.cancelled()) return [] {};
        auto res = std::make_shared<const pixel::TileMap>(std::move(*m));
        return [this, res, name] {
            if (S::save_channel_mask(m_journal, m_doc, res, name + " copy", "Duplicate " + name + " Channel").empty())
                toast("Duplicate " + name + ": refused");
            else toast("“" + name + " copy” is in the saved channels -- load it, then Q to work on it in Quick Mask");
        };
    });
}

}  // namespace klimt
