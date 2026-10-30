#include "Shell.hpp"
#include "AboutWindow.hpp"
#include "ShortcutsDialog.hpp"
#include "CommandPalette.hpp"
#include "Log.hpp"
#include "Registry.hpp"
#include "core/Tools.hpp"

#include <adwaita.h>

#include <chrono>
#include <thread>

// Shell_handlers.cpp -- HANDLERS. Slot bodies: what a user action does.

namespace klimt {

void Shell::on_dump_registry() { registry::dump(); }

void Shell::on_shortcuts() {
    // Built once, re-presented; it renders by walking core::shortcut_registry()
    // (which now carries a row per tool, from the tool registry).
    if (!m_shortcuts) m_shortcuts = std::make_unique<ShortcutsDialog>();
    m_shortcuts->show(*this);
}

void Shell::on_about() {
    if (!m_about) m_about = std::make_unique<AboutWindow>(*this);
    m_about->present();
}

// Undo / redo of the document. The step hands back the selection it had; the
// panel selects it and takes the keyboard, so the next Ctrl+Z lands.
void Shell::on_undo() {
    if (m_stroke.active) return;   // s008: mid-stroke, the stroke finishes first
    if (m_xform.active) { xform_cancel(); toast("Free Transform cancelled"); return; }   // s011: nothing was written
    const auto r = m_journal.undo();
    if (!r.done) return;
    toast("Undid " + r.label);
    m_layers.select(r.selection, !focus_is_text());
    if (auto lg = log::get(log::Area::Shell)) lg->info("undo: {}", r.label);
}

void Shell::on_redo() {
    if (m_stroke.active || m_xform.active) return;
    const auto r = m_journal.redo();
    if (!r.done) return;
    toast("Redid " + r.label);
    m_layers.select(r.selection, !focus_is_text());
    if (auto lg = log::get(log::Area::Shell)) lg->info("redo: {}", r.label);
}

// Sort by Name -- a verb that runs as a JOB (s003). The worker sorts a COPY of
// the names; the landing calls core::layers::arrange, which refuses if the
// group changed meanwhile. `slow` pads it to 3 s so progress and Cancel show.
void Shell::on_sort(bool slow) {
    const auto p = m_layers.primary();
    const std::string parent = m_doc.is_group(p) ? p : (m_doc.get(p) ? m_doc.get(p)->parent : std::string{});
    std::vector<core::layers::Named> rows;
    for (const auto& id : m_doc.panel(parent)) rows.push_back({id, m_doc.get(id)->name});
    if (rows.size() < 2) return;
    const std::string where = parent.empty() ? std::string("Layers") : "“" + m_doc.get(parent)->name + "”";
    const std::string label = "Sort " + where + " by Name";
    start_job(label, [this, rows, parent, label, slow](core::Jobs::Token& t) -> core::Jobs::Landing {
        if (slow)
            for (int i = 1; i <= 60 && !t.cancelled(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                t.progress(i / 60.0);
            }
        auto order = core::layers::sorted_by_name(rows);
        t.progress(1.0);
        return [this, parent, order = std::move(order), label] {
            if (core::layers::arrange(m_journal, m_doc, parent, order, label)) {
                toast(label, true);
                m_layers.select(m_layers.selection(), false);
            } else {
                toast(label + ": already in order, or the group changed -- nothing done");
            }
        };
    });
}

void Shell::on_job_cancel() {  // leaves nothing behind
    for (const auto id : m_job_ids) m_jobs.cancel(id);
    toast("Cancelled -- nothing changed");
    poll_jobs();
}

void Shell::on_tool(const std::string& id) {  // view state: not a step
    if (m_xform.active) xform_commit();   // s011: another tool applies the transform (Photoshop asks; we apply)
    if (const auto* t = core::find_tool(id)) {
        m_strip.show_tool(*t);
        m_canvas.set_tool(id, m_strip.variant(id));   // s007: Hand pans, Zoom zooms; s008: Select selects
        update_paths();   // s035: the Pen shows the active path
        m_canvas.set_brush_size(m_strip.brush(id).size);
        m_canvas.set_hint_mode(id == "refine" ? 1 : 0, m_strip.brush("refine").size / 2.0);   // s038: the Refine brush paints hints
        if (refine_overlay() != m_shown_qm_on) queue_render();   // s039 m3: the Refine overlay comes / goes with the tool
        { const auto o = m_strip.brush(id); m_canvas.set_brush_shape(o.tip, o.angle, o.roundness / 100.0); }   // s017
        if (auto lg = log::get(log::Area::Shell)) lg->info("tool: {}", id);
    }
}

void Shell::on_toggle_panels() {
    m_panels = !m_panels;
    set_rail(m_panels);
    if (m_strip_box) m_strip_box->set_visible(m_panels);
    set_dock(m_panels);
    if (!m_panels) toast("Panels hidden -- Tab on the canvas, or the edge tabs, bring them back");
}

// The hiding tabs (s006) and Tab both come through these two.
void Shell::set_dock(bool open) {
    if (open && m_dock_width < 260) m_dock_width = 340;
    adw_overlay_split_view_set_min_sidebar_width(m_split, m_dock_width);
    adw_overlay_split_view_set_max_sidebar_width(m_split, m_dock_width);
    adw_overlay_split_view_set_show_sidebar(m_split, open);
    m_dock_tab.set_open(open);
    if (auto lg = log::get(log::Area::Shell)) lg->info("dock: {}", open ? "open" : "hidden");
}

void Shell::set_rail(bool open) {
    m_rail.set_visible(open);
    m_rail_tab.set_open(open);
    if (auto lg = log::get(log::Area::Shell)) lg->info("rail: {}", open ? "open" : "hidden");
}

// A History row: walk the journal to that point, one undo / redo at a time
// (the one door), then select what the last step hands back.
void Shell::on_history_go(std::size_t position) {
    core::DocJournal::Result last;
    while (m_journal.position() > position) last = m_journal.undo();
    while (m_journal.position() < position) last = m_journal.redo();
    if (last.done) m_layers.select(last.selection, false);
}

void Shell::on_palette() {
    if (!m_palette) m_palette = std::make_unique<CommandPalette>(*this);
    m_palette->open();
}

}  // namespace klimt
