// Shell_filter.cpp -- THE FILTER MENU (s014). docs/filters.md.
//
//   * Filter > ...: a dialog with a live preview -- what the view shows,
//     filtered on a worker a moment after the last change, shown in place of
//     the layer's pixels (m_preview) -- then OK: the whole layer as a job
//     whose landing is ONE step;
//   * Last Filter (Ctrl+F): the last one again, no dialog; its settings
//     (Ctrl+Alt+F): the dialog, starting from them.
#include "FilterDialog.hpp"
#include "Log.hpp"
#include "Shell.hpp"
#include "core/Selection.hpp"

namespace klimt {

namespace L = core::layers;
namespace F = core::filter;

bool Shell::filter_layer_ok(const std::string& verb) {
    const auto id = m_layers.primary();
    const auto l = m_doc.get(id);
    if (!l) { toast(verb + ": select a layer first"); return false; }
    if (l->kind != core::Kind::Pixel) { toast(verb + ": " + L::not_pixels(m_doc, id)); return false; }
    if (m_doc.locked_all(id) || (l->locks & core::lock::Pixels)) { toast(verb + ": “" + l->name + "” is locked"); return false; }
    if (!l->pixels) { toast(verb + ": “" + l->name + "” is empty"); return false; }
    const auto mode = m_doc.image().format.mode;
    if (mode != pixel::Mode::RGB && mode != pixel::Mode::Gray) {
        toast(verb + ": RGB and Gray documents only, for now (CMYK and Lab wait on colour management)");
        return false;
    }
    m_filter_target = id;
    return true;
}

void Shell::filter_run(const core::Filter& f) {
    const auto target = m_filter_target;
    const auto cur = m_doc.get(target);
    if (!cur || !cur->pixels || F::identity(f)) return;
    m_last_filter = f;
    update_layer_actions();
    const auto before = cur->pixels;
    const auto sel = m_doc.selection_ptr();
    const bool keep_alpha = (cur->locks & core::lock::Alpha) != 0;
    const std::string label(F::label(f.kind));
    m_filter_landing = start_job(label, [this, target, before, sel, f, keep_alpha, label](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto out = F::filtered(*before, f, sel.get(), keep_alpha, nullptr, [&t] { return t.cancelled(); },
                               [&t](double p) { t.progress(p); });
        if (!out || t.cancelled()) return [] {};
        auto res = std::make_shared<pixel::TileMap>(std::move(*out));
        return [this, target, before, sel, res, label] {
            m_filter_landing = 0;
            // The dialog's preview gives way to the result (unless a new dialog is up).
            if (!(m_filter_dialog && m_filter_dialog->get_visible()) && m_preview.erase(target)) queue_render();
            const auto now = m_doc.get(target);
            if (m_doc.selection_ptr() != sel || !now || now->pixels != before) {
                toast(label + ": the layer changed meanwhile -- nothing done");
                return;
            }
            if (!L::set_pixels_if(m_journal, m_doc, target, before, std::move(*res), label))
                toast(label + ": nothing to change here");   // a flat layer sharpened, a median run twice...
        };
    });
}

void Shell::filter_preview_stop() {
    if (m_filter_soon.connected()) m_filter_soon.disconnect();
    if (m_filter_job) { m_jobs.cancel(m_filter_job); m_filter_job = 0; }
    if (m_filter_dialog) m_filter_dialog->set_busy(false);
    if (m_filter_landing) return;   // OK: the preview stays up until the result lands (m4)
    if (m_preview.erase(m_filter_target)) queue_render();
}

void Shell::filter_preview_soon() {
    if (m_filter_soon.connected()) m_filter_soon.disconnect();
    if (m_filter_job) { m_jobs.cancel(m_filter_job); m_filter_job = 0; }
    if (!m_filter_dialog || !m_filter_dialog->preview()) { filter_preview_stop(); return; }
    m_filter_dialog->set_busy(true);
    // A moment after the last change: a slider drag doesn't start a job a pixel.
    m_filter_soon = Glib::signal_timeout().connect([this] {
        const auto l = m_doc.get(m_filter_target);
        if (!l || !l->pixels || !m_filter_dialog) return false;
        const auto src = l->pixels;
        const auto sel = m_doc.selection_ptr();
        const bool keep_alpha = (l->locks & core::lock::Alpha) != 0;
        const core::Filter f = F::normalised(m_filter_dialog->value());
        const pixel::Rect view = m_canvas.visible();
        const auto target = m_filter_target;
        m_filter_job = start_job("Preview", [this, src, sel, keep_alpha, f, view, target](core::Jobs::Token& t) -> core::Jobs::Landing {
            auto out = F::filtered(*src, f, sel.get(), keep_alpha, &view, [&t] { return t.cancelled(); },
                                   [&t](double p) { t.progress(p); });
            if (!out || t.cancelled()) return [] {};
            auto res = std::make_shared<const pixel::TileMap>(std::move(*out));
            return [this, res, target, src] {
                m_filter_job = 0;
                if (m_filter_dialog) m_filter_dialog->set_busy(false);
                // Still the dialog's layer, untouched, preview still wanted?
                const auto now = m_doc.get(target);
                if (!m_filter_dialog || !m_filter_dialog->get_visible() || !m_filter_dialog->preview() || !now || now->pixels != src) return;
                m_preview[target] = res;
                queue_render();
            };
        });
        return false;
    }, 120);
}

void Shell::on_filter(core::FilterKind k) {
    if (!filter_layer_ok(std::string(F::label(k)))) return;
    core::Filter start = (m_last_filter && m_last_filter->kind == k) ? *m_last_filter : F::fresh(k);
    start.seed = ++m_filter_seed;   // Add Noise: new noise each time the dialog opens
    if (!m_filter_dialog) {
        m_filter_dialog = std::make_unique<FilterDialog>(*this);
        m_filter_dialog->signal_changed().connect(sigc::mem_fun(*this, &Shell::filter_preview_soon));
        m_filter_dialog->signal_closed().connect(sigc::mem_fun(*this, &Shell::filter_preview_stop));
        m_filter_dialog->signal_ok().connect(sigc::mem_fun(*this, &Shell::filter_run));
    }
    m_filter_dialog->ask(start, m_doc.get(m_filter_target)->name);
    filter_preview_soon();
}

void Shell::on_filter_last(bool dialog) {
    if (!m_last_filter) { toast("Last Filter: no filter yet this session"); return; }
    if (dialog) { on_filter(m_last_filter->kind); return; }
    if (!filter_layer_ok(std::string(F::label(m_last_filter->kind)))) return;
    core::Filter f = *m_last_filter;
    f.seed = ++m_filter_seed;
    filter_run(f);
}

}  // namespace klimt
