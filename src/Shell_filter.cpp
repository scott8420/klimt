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
#include "core/Live.hpp"   // s018

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
    // s018: from the dialog -- editing a live filter, or Live ticked: the
    // stack changes, not the pixels (one step, at once; the canvas follows).
    const bool from_dialog = m_filter_dialog && m_filter_dialog->get_visible();
    if (from_dialog && m_filter_dialog->editing()) {
        if (m_filter_edit) L::set_live(m_journal, m_doc, target, *m_filter_edit, f);
        if (m_preview.erase(target)) queue_render();
        return;
    }
    if (from_dialog) m_filter_live = m_filter_dialog->live();
    if (m_filter_live) {
        m_last_filter = f;
        update_layer_actions();
        if (!L::add_live(m_journal, m_doc, target, f)) toast(std::string(F::label(f.kind)) + ": refused");
        if (m_preview.erase(target)) queue_render();
        return;
    }
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
        // s018: the preview shows the layer as it will LOOK -- its live stack
        // included (the edited filter in its place, or the new one on top).
        auto stack = l->filters;
        const bool editing = m_filter_dialog->editing() && m_filter_edit && *m_filter_edit < stack.size();
        const bool live = !editing && m_filter_dialog->live();
        if (editing) stack[*m_filter_edit].f = f;
        else if (live) stack.push_back({f, true});
        const int later = core::live::reach(*l);
        m_filter_job = start_job("Preview", [this, src, sel, keep_alpha, f, view, target, stack, editing, live, later](core::Jobs::Token& t) -> core::Jobs::Landing {
            const auto stop = [&t] { return t.cancelled(); };
            std::optional<pixel::TileMap> out;
            if (editing || live) out = core::live::run(*src, stack, &view, stop);
            else {   // the pixels changed, then the stack over them
                const pixel::Rect wide{view.x - later, view.y - later, view.w + 2 * later, view.h + 2 * later};
                auto base = F::filtered(*src, f, sel.get(), keep_alpha, &wide, stop, [&t](double p) { t.progress(p); });
                if (base) out = core::live::run(*base, stack, &view, stop);
            }
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
    m_filter_edit.reset();
    if (!m_filter_dialog) {
        m_filter_dialog = std::make_unique<FilterDialog>(*this);
        m_filter_dialog->signal_changed().connect(sigc::mem_fun(*this, &Shell::filter_preview_soon));
        m_filter_dialog->signal_closed().connect(sigc::mem_fun(*this, &Shell::filter_preview_stop));
        m_filter_dialog->signal_ok().connect(sigc::mem_fun(*this, &Shell::filter_run));
    }
    m_filter_dialog->set_live(m_filter_live, false);
    m_filter_dialog->ask(start, m_doc.get(m_filter_target)->name);
    filter_preview_soon();
}

// s018: double-click a live filter in Properties -- its dialog, editing it.
void Shell::on_live_edit(const core::Document::Id& id, std::size_t i) {
    const auto l = m_doc.get(id);
    if (!l || i >= l->filters.size() || id != m_layers.primary()) return;
    if (!filter_layer_ok("Edit " + std::string(F::label(l->filters[i].f.kind)))) return;
    on_filter(l->filters[i].f.kind);   // makes the dialog (and its signals) if need be
    m_filter_edit = i;
    m_filter_dialog->set_live(true, true);
    m_filter_dialog->ask(l->filters[i].f, l->name);
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

// s019: Layer > Layer Style -- an effect turned on (its defaults, or its
// last settings on this layer), Properties showing it; or Copy / Paste /
// Clear on the selected layers. Each a step (Copy: none).
void Shell::on_style(const std::string& verb) {
    const auto id = m_layers.primary();
    const auto l = m_doc.get(id);
    if (!l || l->kind != core::Kind::Pixel) { toast("Layer Style: pick a pixel layer"); return; }
    if (verb == "copy") {
        m_style_clip = l->styles;
        update_layer_actions();
        toast(l->styles.any() ? "Layer style copied" : "Copied: no style (pasting it clears)");
        return;
    }
    if (verb == "paste" || verb == "clear") {
        if (verb == "paste" && !m_style_clip) return;
        const auto s = verb == "paste" ? *m_style_clip : core::Styles{};
        if (!L::set_styles_on(m_journal, m_doc, m_layers.selection(), s, verb == "paste" ? "Paste Layer Style" : "Clear Layer Style"))
            toast(verb == "paste" ? "Paste Layer Style: nothing to change" : "Clear Layer Style: no style here");
        return;
    }
    if (verb == "rasterize") {
        if (!L::bake_live(m_journal, m_doc, id)) toast("Rasterize Layer Style: no style or live filter here");
        return;
    }
    core::Styles s = l->styles;
    if (verb == "drop-shadow") s.drop.on = true;
    else if (verb == "outer-glow") s.outer.on = true;
    else if (verb == "inner-shadow") s.inner.on = true;
    else if (verb == "bevel-emboss") s.bevel.on = true;   // s021
    else if (verb == "inner-glow") s.inner_glow.on = true;   // s023
    else if (verb == "colour-overlay") s.overlay.on = true;
    else if (verb == "stroke") s.stroke.on = true;
    if (!L::set_styles(m_journal, m_doc, id, s)) toast("Layer Style: already on -- its settings are in Properties");
}

}  // namespace klimt
