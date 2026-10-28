// Shell_fade.cpp -- EDIT > FADE (s020). docs/fade.md.
//
//   * Edit > Fade <what>… (Ctrl+Shift+F): offered while the last step done
//     changed one pixel layer's pixels (core::fade::source);
//   * the dialog previews live -- the fade made on a worker a moment after the
//     last change, shown in place of the layer's pixels (m_preview);
//   * OK: the fade as a job whose landing is ONE step, "Fade <what>".
#include "FadeDialog.hpp"
#include "Log.hpp"
#include "Shell.hpp"
#include "core/Fade.hpp"
#include "core/Live.hpp"
#include "core/Selection.hpp"

namespace klimt {

void Shell::on_fade() {
    if (doc_busy(true)) return;
    const auto src = core::fade::source(m_journal, m_doc);
    if (!src) { toast("Fade: the last step didn't change a layer's pixels"); return; }
    m_fade_src = src;
    if (!m_fade_dialog) {
        m_fade_dialog = std::make_unique<FadeDialog>(*this);
        m_fade_dialog->signal_changed().connect(sigc::mem_fun(*this, &Shell::fade_preview_soon));
        m_fade_dialog->signal_closed().connect(sigc::mem_fun(*this, &Shell::fade_preview_stop));
        m_fade_dialog->signal_ok().connect(sigc::mem_fun(*this, &Shell::fade_run));
    }
    m_fade_dialog->ask(src->what, m_doc.image().format.mode == pixel::Mode::RGB);
    fade_preview_soon();
}

void Shell::fade_preview_stop() {
    if (m_fade_soon.connected()) m_fade_soon.disconnect();
    if (m_fade_job) { m_jobs.cancel(m_fade_job); m_fade_job = 0; }
    if (m_fade_dialog) m_fade_dialog->set_busy(false);
    if (m_fade_landing) return;   // OK: the preview stays up until the fade lands
    if (m_fade_src && m_preview.erase(m_fade_src->id)) queue_render();
}

void Shell::fade_preview_soon() {
    if (m_fade_soon.connected()) m_fade_soon.disconnect();
    if (m_fade_job) { m_jobs.cancel(m_fade_job); m_fade_job = 0; }
    if (!m_fade_dialog || !m_fade_src || !m_fade_dialog->preview()) {
        if (m_fade_src && m_preview.erase(m_fade_src->id)) queue_render();
        return;
    }
    m_fade_dialog->set_busy(true);
    m_fade_soon = Glib::signal_timeout().connect([this] {
        if (!m_fade_dialog || !m_fade_src) return false;
        const auto src = *m_fade_src;
        const auto l = m_doc.get(src.id);
        if (!l || l->pixels != src.after) return false;
        const double o = m_fade_dialog->opacity();
        const auto mode = m_fade_dialog->mode();
        const auto stack = l->filters;   // s018: the preview shows the layer as it will LOOK
        const pixel::Rect view = m_canvas.visible();
        m_fade_job = start_job("Preview", [this, src, o, mode, stack, view](core::Jobs::Token& t) -> core::Jobs::Landing {
            const auto stop = [&t] { return t.cancelled(); };
            auto out = core::fade::faded(src.before, *src.after, o, mode, stop);
            if (out && !stack.empty()) out = core::live::run(*out, stack, &view, stop);
            if (!out || t.cancelled()) return [] {};
            auto res = std::make_shared<const pixel::TileMap>(std::move(*out));
            return [this, res, src] {
                m_fade_job = 0;
                if (m_fade_dialog) m_fade_dialog->set_busy(false);
                const auto now = m_doc.get(src.id);
                if (!m_fade_dialog || !m_fade_dialog->get_visible() || !m_fade_dialog->preview() || !now || now->pixels != src.after) return;
                m_preview[src.id] = res;
                queue_render();
            };
        });
        return false;
    }, 120);
}

void Shell::fade_run() {
    if (!m_fade_dialog || !m_fade_src) return;
    const auto src = *m_fade_src;
    const double o = m_fade_dialog->opacity();
    const auto mode = m_fade_dialog->mode();
    const std::string label = "Fade " + src.what;
    m_fade_landing = start_job(label, [this, src, o, mode, label](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto out = core::fade::faded(src.before, *src.after, o, mode, [&t] { return t.cancelled(); });
        if (!out || t.cancelled()) return [this, src] { m_fade_landing = 0; if (m_preview.erase(src.id)) queue_render(); };
        auto res = std::make_shared<pixel::TileMap>(std::move(*out));
        return [this, src, res, label] {
            m_fade_landing = 0;
            if (m_preview.erase(src.id)) queue_render();
            const auto now = m_doc.get(src.id);
            if (!now || now->pixels != src.after) { toast(label + ": the layer changed meanwhile -- nothing done"); return; }
            if (!core::layers::set_pixels_if(m_journal, m_doc, src.id, src.after, std::move(*res), label))
                toast(label + ": nothing to change at 100 % Normal");
        };
    });
}

}  // namespace klimt
