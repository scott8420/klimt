#include "Shell.hpp"
#include "Log.hpp"

#include <adwaita.h>

#include <algorithm>
#include <set>

// Shell_tabs.cpp -- TABS (s020). Several documents in one window
// (docs/tabs.md). The model is core::Tabs: ONE live document + journal (the
// pair every panel holds by reference), the others parked. The bar is an
// AdwTabBar over an AdwTabView whose pages have EMPTY children -- the view
// is only the bar's model; the canvas stays where it is. The model leads:
// sync_tabs() makes the pages follow it; a click on the bar asks the model
// (show_tab) and the pages are put back if it says no.

namespace klimt {

namespace {
std::uint64_t key_of(const std::map<std::uint64_t, AdwTabPage*>& pages, AdwTabPage* page) {
    for (const auto& [k, p] : pages) if (p == page) return k;
    return 0;
}
}  // namespace

void Shell::build_tabs(Gtk::Box& into) {
    m_tab_view = ADW_TAB_VIEW(adw_tab_view_new());
    // Our own keys (win.tab-*): the view's would take Ctrl+Home / End and
    // Alt+digits from the canvas and the text boxes.
    adw_tab_view_set_shortcuts(m_tab_view, ADW_TAB_VIEW_SHORTCUT_NONE);
    gtk_widget_set_vexpand(GTK_WIDGET(m_tab_view), FALSE);
    gtk_widget_set_name(GTK_WIDGET(m_tab_view), "shell.tabs.view");
    m_tab_bar = ADW_TAB_BAR(adw_tab_bar_new());
    adw_tab_bar_set_view(m_tab_bar, m_tab_view);
    adw_tab_bar_set_autohide(m_tab_bar, TRUE);   // one document: no bar (the title names it)
    gtk_widget_set_name(GTK_WIDGET(m_tab_bar), "shell.tabs.bar");
    gtk_box_append(GTK_BOX(into.gobj()), GTK_WIDGET(m_tab_bar));
    gtk_box_append(GTK_BOX(into.gobj()), GTK_WIDGET(m_tab_view));

    g_signal_connect(m_tab_view, "notify::selected-page", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer self) {
        auto* sh = static_cast<Shell*>(self);
        if (sh->m_tab_sync) return;
        auto* page = adw_tab_view_get_selected_page(sh->m_tab_view);
        const auto i = sh->m_tabs.by_key(key_of(sh->m_pages, page));
        if (!i || *i == sh->m_tabs.live()) return;
        if (!sh->show_tab(*i)) sh->sync_tabs();   // refused: the bar goes back
    }), this);
    // The tab's x: never closed by the view itself -- the model decides (and
    // may ask about unsaved changes first); sync_tabs() then drops the page.
    g_signal_connect(m_tab_view, "close-page", G_CALLBACK(+[](AdwTabView* v, AdwTabPage* page, gpointer self) -> gboolean {
        auto* sh = static_cast<Shell*>(self);
        if (sh->m_tab_sync) { adw_tab_view_close_page_finish(v, page, TRUE); return TRUE; }
        adw_tab_view_close_page_finish(v, page, FALSE);
        const auto key = key_of(sh->m_pages, page);
        Glib::signal_idle().connect_once([sh, key] {
            if (const auto i = sh->m_tabs.by_key(key)) sh->close_tab(*i);
        });
        return TRUE;
    }), this);
    g_signal_connect(m_tab_view, "page-reordered", G_CALLBACK(+[](AdwTabView*, AdwTabPage* page, int position, gpointer self) {
        auto* sh = static_cast<Shell*>(self);
        if (sh->m_tab_sync) return;
        if (const auto i = sh->m_tabs.by_key(key_of(sh->m_pages, page)); i && position >= 0)
            sh->m_tabs.reorder(*i, static_cast<std::size_t>(position));
    }), this);
    sync_tabs();
}

AdwTabPage* Shell::page_of(std::uint64_t key) const {
    const auto it = m_pages.find(key);
    return it == m_pages.end() ? nullptr : it->second;
}

void Shell::sync_tabs() {
    if (!m_tab_view) return;
    m_tab_sync = true;
    // Pages whose tab is gone.
    for (auto it = m_pages.begin(); it != m_pages.end();) {
        if (!m_tabs.by_key(it->first)) {
            adw_tab_view_close_page(m_tab_view, it->second);   // close-page finishes it at once (m_tab_sync)
            it = m_pages.erase(it);
        } else ++it;
    }
    for (std::size_t i = 0; i < m_tabs.count(); ++i) {
        const auto& info = m_tabs.info(i);
        AdwTabPage* page = page_of(info.key);
        if (!page) {
            page = adw_tab_view_insert(m_tab_view, gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), static_cast<int>(i));
            m_pages[info.key] = page;
        } else if (adw_tab_view_get_page_position(m_tab_view, page) != static_cast<int>(i)) {
            adw_tab_view_reorder_page(m_tab_view, page, static_cast<int>(i));
        }
        const std::string title = (m_tabs.dirty(i) ? "• " : "") + info.name;
        adw_tab_page_set_title(page, title.c_str());
        adw_tab_page_set_tooltip(page, info.path.empty() ? info.name.c_str() : info.path.c_str());
    }
    if (auto* live = page_of(m_tabs.live_info().key)) adw_tab_view_set_selected_page(m_tab_view, live);
    m_tab_sync = false;
}

// A switch, a new tab or a close waits for anything working on the live
// document: its landing writes through the live journal, and would land in
// whichever document is live by then.
bool Shell::doc_busy(bool say) {
    using S = core::Jobs::State;
    std::string what;
    if (m_stroke.active || m_move.active || m_patch.active) what = "the stroke to end";
    else if (m_xform.active) what = "Free Transform -- Enter or Escape first";
    else if (m_adjust_preview || !m_preview.empty() || m_filter_landing || m_fade_landing) what = "the preview to finish";
    else
        for (const auto id : m_job_ids) {
            const auto st = m_jobs.state(id);
            if (st == S::Queued || st == S::Running || st == S::Done) { what = "“" + m_jobs.label(id) + "” to finish"; break; }
        }
    if (what.empty()) return false;
    if (say) toast("Wait for " + what);
    return true;
}

bool Shell::tab_reusable() const { return !has_image() && !m_tabs.dirty(m_tabs.live()); }

void Shell::remember_tab() {
    auto& tv = m_tab_views[m_tabs.live_info().key];
    tv.view = m_canvas.view();
    tv.selection = m_layers.selection();
    tv.mask_view = m_mask_view;
    tv.quick_mask = m_quick_mask;
    tv.clone = m_clone;
    tv.known = true;
}

bool Shell::show_tab(std::size_t i) {
    if (i >= m_tabs.count()) return false;
    if (i == m_tabs.live()) return true;
    if (doc_busy(true)) return false;
    remember_tab();
    if (!m_tabs.show(i)) { toast("Can't switch now -- finish what's under way"); return false; }
    after_switch();
    sync_tabs();
    if (auto lg = log::get(log::Area::Shell)) lg->info("tab: {} ({} of {})", m_tabs.live_info().name, i + 1, m_tabs.count());
    return true;
}

bool Shell::new_tab() {
    if (doc_busy(true)) return false;
    remember_tab();
    const auto at = m_tabs.add(m_tabs.live());
    if (!m_tabs.show(at)) { m_tabs.close(at); return false; }
    after_switch();
    sync_tabs();
    return true;
}

void Shell::step_tab(int by) {
    const auto n = static_cast<int>(m_tabs.count());
    if (n < 2) return;
    show_tab(static_cast<std::size_t>(((static_cast<int>(m_tabs.live()) + by) % n + n) % n));
}

// Everything that follows the document, for the one now live: the canvas
// (its tiles, its view), the panels, the title, the ants. What only the old
// one had in flight is dropped (renders, the ants' trace, nudges).
void Shell::after_switch() {
    for (const auto id : m_renders) m_jobs.cancel(id);
    m_renders.clear();
    if (m_ants_job) { m_jobs.cancel(m_ants_job); m_ants_job = 0; }
    m_ants_for.reset();
    m_ants_known = false;
    m_nudge = {};
    m_stroke_last_valid = false;
    m_preview.clear();
    m_shown = {};
    m_shown_image = {};
    m_shown_qm.reset();

    const auto key = m_tabs.live_info().key;
    const TabView tv = m_tab_views.count(key) ? m_tab_views[key] : TabView{};
    m_layers.forget_view();
    m_mask_view = tv.mask_view;
    m_layers.set_mask_viewing(m_mask_view);
    m_quick_mask = tv.quick_mask && has_image();
    m_clone = tv.clone;
    show_clone_marker();

    m_canvas.set_document(m_doc.image().width, m_doc.image().height);
    m_dirty = {};
    if (tv.known) m_canvas.set_view(tv.view);
    else m_canvas.fit();

    std::vector<core::Document::Id> sel;
    for (const auto& id : tv.selection) if (m_doc.get(id)) sel.push_back(id);
    if (sel.empty()) {
        const auto top = m_doc.panel("");
        if (!top.empty()) sel.push_back(top.front());
    }
    m_layers.select(sel, false);

    queue_render();
    queue_ants();
    update_channels();
    m_history.repaint();
    update_undo_ui();
    update_title();
    update_summary();
    update_layer_actions();
    update_select_actions();
}

void Shell::close_tab(std::size_t i) {
    if (i >= m_tabs.count()) return;
    if (file_job_busy()) return;
    if (i != m_tabs.live() && !show_tab(i)) return;
    if (doc_busy(true)) return;
    if (m_tabs.count() == 1 && tab_reusable()) return;   // nothing to close
    const auto key = m_tabs.live_info().key;
    ask_unsaved("closing it", [this, key] {
        const auto j = m_tabs.by_key(key);
        if (!j) return;
        if (*j != m_tabs.live() && !show_tab(*j)) return;
        if (doc_busy(true)) return;
        m_tab_views.erase(key);
        if (!m_tabs.close(*j)) { toast("Can't close now -- finish what's under way"); return; }
        after_switch();   // the neighbour, or the last tab emptied
        sync_tabs();
    });
}

// Quitting: each tab with unsaved changes is shown and asks (Save / Discard
// / Cancel); Cancel anywhere stops the quit. A tab answered is passed over
// (not closed: the window is about to go, and a cancel later keeps it as it
// was, changes and all).
void Shell::close_all_then(std::function<void()> done, std::set<std::uint64_t> answered) {
    std::optional<std::size_t> d;
    const auto wants = [&](std::size_t i) { return m_tabs.dirty(i) && !answered.count(m_tabs.info(i).key); };
    if (wants(m_tabs.live())) d = m_tabs.live();
    else
        for (std::size_t i = 0; i < m_tabs.count() && !d; ++i)
            if (wants(i)) d = i;
    if (!d) { done(); return; }
    if (!show_tab(*d)) return;
    const auto key = m_tabs.live_info().key;
    ask_unsaved("closing", [this, key, done, answered]() mutable {
        answered.insert(key);
        close_all_then(done, std::move(answered));
    });
}

}  // namespace klimt
