#include "Shell.hpp"
#include "Log.hpp"
#include "core/Commands.hpp"
#include "core/Selection.hpp"

#include <glibmm/main.h>
#include <gtkmm/window.h>

#include <cstdlib>
#include <set>

// Shell_sweep.cpp -- THE REGRESSION SWEEP (s090; docs/regression.md).
// `KLIMT_SWEEP=1 build/klimt`: the demo document, then every command the
// palette knows run in turn -- each one's dialog (if it opens one) closed
// again, its step undone -- with a log line before and after, so a crash, a
// GLib warning or a hang names the command it came from. The window closes
// when it's done. File choosers and quitting are skipped (they'd wait on a
// person); `KLIMT_SWEEP_ONLY=filter` limits it to actions whose name holds
// that text.

namespace klimt {

namespace {
bool skipped(const std::string& action) {
    static const std::set<std::string> skip = {
        "win.quit", "win.tab-close", "win.open", "win.save", "win.save-as", "win.export", "win.place-embedded",
        "win.place-linked", "win.smart-replace", "win.new", "win.open-demo", "win.dump-registry",
    };
    return skip.count(action) > 0;
}
}  // namespace

void Shell::start_sweep() {
    open_demo();
    sweep_setup();
    const char* only = std::getenv("KLIMT_SWEEP_ONLY");
    m_sweep.clear();
    for (const auto& c : core::commands())
        if (!skipped(c.action) && (!only || c.action.find(only) != std::string::npos)) m_sweep.push_back(&c);
    m_sweep_at = 0;
    m_sweep_failed = 0;
    if (auto lg = log::get(log::Area::Shell)) lg->info("sweep: {} commands", m_sweep.size());
    Glib::signal_timeout().connect_once([this] { sweep_step(); }, 800);
}

// The demo set up for the verbs to find what they need: a pixel layer picked
// (the demo's top is type); KLIMT_SWEEP=2 -- a selection too (Fill, Crop,
// Feather, Content-Aware Fill ...).
void Shell::sweep_setup() {
    m_sweep_layer.clear();
    for (const auto& id : m_doc.walk())
        if (const auto l = m_doc.get(id); l && l->kind == core::Kind::Pixel && l->pixels) { m_sweep_layer = id; break; }
    if (!m_sweep_layer.empty()) m_layers.select({m_sweep_layer}, false);
    const char* s = std::getenv("KLIMT_SWEEP");
    if (s && std::string(s) == "2") {
        const auto& im = m_doc.image();
        core::select::rect(m_journal, m_doc, {im.width / 4, im.height / 4, im.width / 2, im.height / 2});
    }
    update_layer_actions();
    if (auto lg = log::get(log::Area::Shell)) lg->info("sweep: picked {}{}", m_sweep_layer, s && std::string(s) == "2" ? ", a selection made" : "");
}

void Shell::sweep_step() {
    auto lg = log::get(log::Area::Shell);
    // Wait for the last command's job (a filter, a canvas turn).
    if (m_jobs.active() > 0) {
        if (++m_sweep_waits < 300) { Glib::signal_timeout().connect_once([this] { sweep_step(); }, 100); return; }
        if (lg) lg->warn("sweep:   STILL RUNNING after 30 s -- cancelled");   // a hang, named by the line above
        ++m_sweep_failed;
        m_jobs.cancel_all();
    }
    m_sweep_waits = 0;
    // Close what it opened: every other window of ours (a dialog, an alert).
    for (auto* w : Gtk::Window::list_toplevels())
        if (w != this && w->get_visible()) {
            if (lg) lg->info("sweep:   closed \"{}\"", std::string(w->get_title()));
            w->close();
        }
    // Its step undone, so the next command finds the demo as it was.
    if (m_sweep_at > 0 && m_journal.mark() != m_sweep_mark) {
        while (m_journal.can_undo() && m_journal.mark() != m_sweep_mark) m_journal.undo();
    }
    if (m_sweep_at >= m_sweep.size()) {
        if (lg) lg->info("sweep: done -- {} commands, {} refused to run", m_sweep.size(), m_sweep_failed);
        Glib::signal_timeout().connect_once([this] { m_closing = true; close(); }, 500);
        return;
    }
    if (!has_image()) { open_demo(); sweep_setup(); }   // a command closed the picture
    if (!m_sweep_layer.empty() && m_doc.get(m_sweep_layer) && m_layers.primary() != m_sweep_layer) m_layers.select({m_sweep_layer}, false);
    // KLIMT_SWEEP=2: a selection before every command (a late job's landing
    // can leave the picture without one), and the greying brought up to date.
    if (const char* s = std::getenv("KLIMT_SWEEP"); s && std::string(s) == "2" && !m_doc.selection()) {
        const auto& im = m_doc.image();
        core::select::rect(m_journal, m_doc, {im.width / 4, im.height / 4, im.width / 2, im.height / 2});
    }
    update_layer_actions();
    const auto* c = m_sweep[m_sweep_at++];
    m_sweep_mark = m_journal.mark();
    const std::string name = c->action.substr(c->action.find('.') + 1);
    auto* group = G_ACTION_GROUP(gobj());
    if (!g_action_group_has_action(group, name.c_str())) {
        if (lg) lg->warn("sweep: [{}/{}] {} -- NO SUCH ACTION ({})", m_sweep_at, m_sweep.size(), c->action, c->title);
        ++m_sweep_failed;
    } else if (!g_action_group_get_action_enabled(group, name.c_str())) {
        if (lg) lg->info("sweep: [{}/{}] {} -- greyed here ({})", m_sweep_at, m_sweep.size(), c->action, c->title);
    } else {
        if (lg) lg->info("sweep: [{}/{}] {} ({})", m_sweep_at, m_sweep.size(), c->action, c->title);
        GVariant* p = c->target.empty() ? nullptr : g_variant_new_string(c->target.c_str());
        g_action_group_activate_action(group, name.c_str(), p);
    }
    Glib::signal_timeout().connect_once([this] { sweep_step(); }, 350);
}

}  // namespace klimt
