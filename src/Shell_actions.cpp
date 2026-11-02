#include "Shell.hpp"
#include "Log.hpp"

#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <giomm/file.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>

#include <filesystem>

// Shell_actions.cpp -- ACTIONS AND BATCH (s084; REQUIREMENTS "Added" 10;
// docs/actions.md). The library lives here and in its file; recording adds
// an act as each recordable verb is asked for (the hooks call rec()); Play
// runs the action on a COPY on a worker and lands ONE step; Batch runs it
// over a folder, nothing in the window touched.

namespace klimt {

namespace A = core::actions;
namespace fs = std::filesystem;

// ── the library ───────────────────────────────────────────────────────────
void Shell::build_actions() {
    m_actions_file = Glib::build_filename(Glib::get_user_config_dir(), "klimt", "actions.json");
    std::error_code ec;
    m_actions = fs::exists(m_actions_file, ec) ? A::load(m_actions_file) : A::starters();   // the first run: a few to start from
    m_actions_panel.set_on(sigc::mem_fun(*this, &Shell::on_actions));
    const auto recorder = [this](const A::Act& a) { rec(a); };
    m_layers.set_recorder(recorder);
    m_properties.set_recorder(recorder);
    sync_actions();
}

void Shell::save_actions() {
    if (!A::save(m_actions_file, m_actions)) toast("Actions: couldn't write " + m_actions_file);
}

void Shell::sync_actions() {
    m_actions_panel.show(m_actions, m_recording, m_action_playing);
    const bool rec_on = m_recording >= 0;
    if (auto it = m_layer_acts.find("action-stop"); it != m_layer_acts.end()) it->second->set_enabled(rec_on);
    if (auto it = m_layer_acts.find("action-record"); it != m_layer_acts.end()) it->second->set_enabled(!rec_on && !m_action_playing);
    if (auto it = m_layer_acts.find("action-play"); it != m_layer_acts.end())
        it->second->set_enabled(!rec_on && !m_action_playing && has_image());
}

// ── recording ─────────────────────────────────────────────────────────────
void Shell::rec(A::Act a, const core::Document::Id& made) {
    if (m_recording < 0 || m_recording >= static_cast<int>(m_actions.size())) return;
    rec_retune();
    auto& acts = m_actions[static_cast<std::size_t>(m_recording)].acts;
    // A slider walked by the keys, a menu tried twice: the last one stands.
    if (!acts.empty() && acts.back().verb == a.verb && (a.verb == "layer.opacity" || a.verb == "layer.blend")) acts.pop_back();
    acts.push_back(std::move(a));
    if (!made.empty()) m_rec_made.push_back({acts.size() - 1, made});
    if (auto lg = log::get(log::Area::Shell)) lg->info("action: recorded {} ({})", acts.back().label, acts.back().verb);
    save_actions();
    sync_actions();
}

// The adjustment / filter layers a recorded act made: their settings as they
// stand now (set in Properties after the layer was made) go into the act.
void Shell::rec_retune() {
    if (m_recording < 0 || m_recording >= static_cast<int>(m_actions.size())) return;
    auto& acts = m_actions[static_cast<std::size_t>(m_recording)].acts;
    for (const auto& [i, id] : m_rec_made) {
        if (i >= acts.size()) continue;
        const auto l = m_doc.get(id);
        if (!l) continue;
        if (l->kind == core::Kind::Adjustment) A::retune(acts[i], l->adjust);
        else if (l->kind == core::Kind::Filter && !l->filters.empty()) A::retune(acts[i], l->filters.front().f);
    }
}

// ── the panel ─────────────────────────────────────────────────────────────
void Shell::on_actions(ActionsPanel::Do d, int action, int step, const std::string& text) {
    const bool have = action >= 0 && action < static_cast<int>(m_actions.size());
    const auto ai = static_cast<std::size_t>(std::max(action, 0));
    switch (d) {
        case ActionsPanel::Do::New: {
            if (m_recording >= 0) { toast("Actions: stop recording first (■)"); return; }
            m_actions.push_back({A::next_name(m_actions), {}});
            m_recording = static_cast<int>(m_actions.size()) - 1;
            m_rec_made.clear();
            m_actions_panel.pick(m_recording);
            save_actions();
            toast("Recording “" + m_actions.back().name + "” -- filters, adjustments, sizes, selections, Fill, layers; ■ in Actions stops");
            break;
        }
        case ActionsPanel::Do::Record: {
            if (m_recording >= 0 || m_action_playing) return;
            if (!have) { on_actions(ActionsPanel::Do::New, -1, -1, {}); return; }
            m_recording = action;
            m_rec_made.clear();
            toast("Recording into “" + m_actions[ai].name + "” -- what you do next is added; ■ stops");
            break;
        }
        case ActionsPanel::Do::Stop: {
            if (m_recording < 0) return;
            rec_retune();
            const auto& a = m_actions[static_cast<std::size_t>(m_recording)];
            toast("“" + a.name + "”: " + std::to_string(a.acts.size()) + (a.acts.size() == 1 ? " step" : " steps") + " recorded");
            m_recording = -1;
            m_rec_made.clear();
            save_actions();
            break;
        }
        case ActionsPanel::Do::Play:
            if (have) play_action(ai, step < 0 ? 0 : static_cast<std::size_t>(step));
            return;
        case ActionsPanel::Do::Delete: {
            if (!have || m_action_playing) return;
            if (step >= 0 && step < static_cast<int>(m_actions[ai].acts.size())) {
                const auto label = m_actions[ai].acts[static_cast<std::size_t>(step)].label;
                m_actions[ai].acts.erase(m_actions[ai].acts.begin() + step);
                if (action == m_recording) {   // the made-layer refs after it shift down
                    std::vector<std::pair<std::size_t, core::Document::Id>> keep;
                    for (auto [i, id] : m_rec_made) {
                        if (static_cast<int>(i) == step) continue;
                        keep.push_back({static_cast<int>(i) > step ? i - 1 : i, id});
                    }
                    m_rec_made = std::move(keep);
                }
                m_actions_panel.pick(action, -1);
                toast("“" + label + "” taken out of “" + m_actions[ai].name + "”");
            } else {
                const auto name = m_actions[ai].name;
                if (action == m_recording) { m_recording = -1; m_rec_made.clear(); }
                else if (m_recording > action) --m_recording;
                m_actions.erase(m_actions.begin() + action);
                m_actions_panel.pick(-1);
                toast("Action “" + name + "” deleted");
            }
            save_actions();
            break;
        }
        case ActionsPanel::Do::Rename:
            if (!have || text.empty()) return;
            m_actions[ai].name = text;
            save_actions();
            break;
        case ActionsPanel::Do::Toggle:
            if (!have || step < 0 || step >= static_cast<int>(m_actions[ai].acts.size())) return;
            m_actions[ai].acts[static_cast<std::size_t>(step)].on = !m_actions[ai].acts[static_cast<std::size_t>(step)].on;
            save_actions();
            break;
        case ActionsPanel::Do::Batch:
            on_batch();
            return;
    }
    sync_actions();
}

// ── playing ───────────────────────────────────────────────────────────────
void Shell::play_action(std::size_t action, std::size_t from) {
    if (action >= m_actions.size()) return;
    if (m_recording >= 0) { toast("Actions: stop recording first (■)"); return; }
    if (m_action_playing) return;
    if (!has_image()) { toast("Play: no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (m_liq.active || m_xform.active || m_stroke.active || m_journal.in_gesture()) { toast("Play: finish what's under way first (Enter or Esc)"); return; }
    A::Action a = m_actions[action];
    if (from > 0 && from < a.acts.size()) a.acts.erase(a.acts.begin(), a.acts.begin() + static_cast<std::ptrdiff_t>(from));
    if (a.acts.empty()) { toast("Play: “" + a.name + "” has no steps"); return; }
    const auto start = std::make_shared<const core::snapshot::Snapshot>(core::snapshot::take(m_doc, "start"));
    const auto picked = m_layers.primary();
    const auto mark = m_journal.mark();
    const auto key = m_tabs.live_info().key;
    const std::string label = "Action “" + a.name + "”";
    m_action_playing = true;
    sync_actions();
    if (auto lg = log::get(log::Area::Shell)) lg->info("action: play {} ({} steps from {})", a.name, a.acts.size(), from + 1);
    start_job(label, [this, start, picked, a, mark, key, label](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto p = std::make_shared<A::Played>(A::play(*start, picked, a, [&t] { return t.cancelled(); }, [&t](double f) { t.progress(f); }));
        return [this, p, mark, key, label] {
            m_action_playing = false;
            sync_actions();
            if (!p->ok) {
                if (p->error != "cancelled") toast(label + " stopped at " + p->error + " -- nothing done");
                if (auto lg = log::get(log::Area::Shell)) lg->info("action: stopped -- {}", p->error);
                return;
            }
            if (m_tabs.live_info().key != key || m_journal.mark() != mark) { toast(label + ": the image changed meanwhile -- nothing done"); return; }
            if (!core::snapshot::restore(m_journal, m_doc, p->result)) { toast(label + ": nothing changed"); return; }
            if (m_doc.get(p->picked)) m_layers.select({p->picked}, false);
            queue_ants();
            update_channels();
            update_summary();
            toast(label + ": " + std::to_string(p->ran) + (p->ran == 1 ? " step" : " steps") + " -- one step here (Ctrl+Z undoes it all)");
            if (auto lg = log::get(log::Area::Shell)) lg->info("action: landed {} ({} ran)", label, p->ran);
        };
    });
}

// ── batch ─────────────────────────────────────────────────────────────────
namespace {

// File > Automate > Batch…: which action, which folder, where to, as what.
class BatchDialog : public Gtk::Window {
public:
    BatchDialog(Gtk::Window& parent, const std::vector<A::Action>& actions, int picked, const A::Batch& last,
                std::function<void(A::Batch, std::size_t)> go)
        : m_actions(actions), m_batch(last), m_go(std::move(go)) {
        m_dest_kept = m_batch.dest;
        set_title("Batch");
        set_name("batch");
        set_transient_for(parent);
        set_modal(true);
        set_default_size(520, -1);
        set_resizable(false);
        auto g = Gtk::make_managed<Gtk::Grid>();
        g->set_row_spacing(10);
        g->set_column_spacing(12);
        g->set_margin(18);
        int row = 0;
        const auto label = [&](const char* t) {
            auto l = Gtk::make_managed<Gtk::Label>(t);
            l->set_xalign(1.0f);
            l->add_css_class("dim-label");
            g->attach(*l, 0, row);
        };
        label("Action");
        std::vector<Glib::ustring> names;
        for (const auto& a : actions) names.push_back(a.name + "  (" + std::to_string(a.acts.size()) + ")");
        m_action.set_model(Gtk::StringList::create(names));
        m_action.set_name("batch.action");
        m_action.set_hexpand(true);
        if (picked >= 0 && picked < static_cast<int>(actions.size())) m_action.set_selected(static_cast<guint>(picked));
        g->attach(m_action, 1, row++, 2, 1);

        label("Pictures in");
        m_source.set_name("batch.source");
        m_source.set_hexpand(true);
        m_source.signal_clicked().connect([this] { pick_folder(true); });
        g->attach(m_source, 1, row++, 2, 1);
        m_sub.set_label("Include the folders inside it");
        m_sub.set_name("batch.subfolders");
        m_sub.set_active(m_batch.subfolders);
        m_sub.signal_toggled().connect([this] { m_batch.subfolders = m_sub.get_active(); refresh(); });
        g->attach(m_sub, 1, row++, 2, 1);

        label("Save to");
        m_beside.set_label("Beside each picture");
        m_beside.set_name("batch.beside");
        m_folder.set_label("This folder:");
        m_folder.set_group(m_beside);
        m_folder.set_name("batch.to_folder");
        (m_batch.dest.empty() ? m_beside : m_folder).set_active(true);
        m_beside.signal_toggled().connect([this] { refresh(); });
        g->attach(m_beside, 1, row++, 2, 1);
        g->attach(m_folder, 1, row);
        m_dest.set_name("batch.dest");
        m_dest.set_hexpand(true);
        m_dest.signal_clicked().connect([this] { pick_folder(false); });
        g->attach(m_dest, 2, row++);

        label("Name");
        auto name_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto same = Gtk::make_managed<Gtk::Label>("its own, then");
        same->add_css_class("dim-label");
        name_box->append(*same);
        m_suffix.set_name("batch.suffix");
        m_suffix.set_placeholder_text("-web  (nothing: the same name)");
        m_suffix.set_text(m_batch.suffix);
        m_suffix.set_hexpand(true);
        m_suffix.signal_changed().connect([this] { refresh(); });
        name_box->append(m_suffix);
        g->attach(*name_box, 1, row++, 2, 1);

        label("Save as");
        std::vector<Glib::ustring> kinds{"Klimt document (.klimt)"};
        m_kinds.push_back(formats::Kind::Klimt);
        for (const auto& ef : formats::export_formats()) {
            if (ef.kind == formats::Kind::EPS) continue;
            kinds.push_back(std::string(ef.label) + "  (" + ef.ext + ")");
            m_kinds.push_back(ef.kind);
        }
        m_kind.set_model(Gtk::StringList::create(kinds));
        m_kind.set_name("batch.kind");
        for (std::size_t i = 0; i < m_kinds.size(); ++i)
            if (m_kinds[i] == m_batch.kind) m_kind.set_selected(static_cast<guint>(i));
        m_kind.property_selected().signal_changed().connect([this] { refresh(); });
        g->attach(m_kind, 1, row++, 2, 1);
        m_replace.set_label("Replace a file that's there already (else it's skipped)");
        m_replace.set_name("batch.replace");
        m_replace.set_active(m_batch.overwrite);
        m_replace.signal_toggled().connect([this] { refresh(); });
        g->attach(m_replace, 1, row++, 2, 1);

        m_say.set_xalign(0.0f);
        m_say.set_wrap(true);
        m_say.set_max_width_chars(56);
        m_say.add_css_class("caption");
        g->attach(m_say, 0, row++, 3, 1);
        auto buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        buttons->set_halign(Gtk::Align::END);
        auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
        cancel->signal_clicked().connect([this] { close(); });
        m_run.set_label("Run");
        m_run.set_name("batch.run");
        m_run.add_css_class("suggested-action");
        m_run.signal_clicked().connect([this] {
            read();
            const auto i = static_cast<std::size_t>(m_action.get_selected());
            if (i >= m_actions.size() || !A::refusal(m_batch, m_actions[i]).empty()) return;
            auto go = m_go;
            auto b = m_batch;
            close();
            go(b, i);
        });
        buttons->append(*cancel);
        buttons->append(m_run);
        g->attach(*buttons, 0, row++, 3, 1);
        set_child(*g);
        m_action.property_selected().signal_changed().connect([this] { refresh(); });
        refresh();
    }

private:
    std::vector<A::Action> m_actions;
    A::Batch m_batch;
    std::function<void(A::Batch, std::size_t)> m_go;
    std::vector<formats::Kind> m_kinds;
    Gtk::DropDown m_action, m_kind;
    Gtk::Button m_source, m_dest, m_run;
    Gtk::CheckButton m_sub, m_beside, m_folder, m_replace;
    Gtk::Entry m_suffix;
    Gtk::Label m_say;

    void read() {
        m_batch.suffix = m_suffix.get_text();
        m_batch.overwrite = m_replace.get_active();
        m_batch.subfolders = m_sub.get_active();
        const auto k = static_cast<std::size_t>(m_kind.get_selected());
        if (k < m_kinds.size()) m_batch.kind = m_kinds[k];
        if (m_beside.get_active()) {
            if (!m_batch.dest.empty()) m_dest_kept = m_batch.dest;
            m_batch.dest.clear();
        } else if (m_batch.dest.empty()) {
            m_batch.dest = m_dest_kept;
        }
    }
    std::string m_dest_kept;
    void refresh() {
        read();
        m_source.set_label(m_batch.source.empty() ? "Choose a folder…" : fs::path(m_batch.source).filename().string() + "  —  " + m_batch.source);
        const std::string d = m_batch.dest.empty() ? m_dest_kept : m_batch.dest;
        m_dest.set_label(d.empty() ? "Choose a folder…" : fs::path(d).filename().string());
        m_dest.set_tooltip_text(d);
        m_dest.set_sensitive(m_folder.get_active());
        const auto i = static_cast<std::size_t>(m_action.get_selected());
        std::string why = i < m_actions.size() ? A::refusal(m_batch, m_actions[i]) : "pick an action";
        if (why.empty() && m_folder.get_active() && m_batch.dest.empty()) why = "choose the folder to save to";
        if (why.empty()) {
            const auto n = A::sources(m_batch).size();
            m_say.set_text(n == 0 ? "No pictures Klimt opens in that folder."
                                  : std::to_string(n) + (n == 1 ? " picture" : " pictures") + " -- the first: " +
                                        fs::path(A::sources(m_batch).front()).filename().string() + " → " +
                                        fs::path(A::target_of(m_batch, A::sources(m_batch).front())).filename().string());
            m_say.remove_css_class("error");
            m_run.set_sensitive(n > 0);
        } else {
            m_say.set_text("Can't run yet: " + why);
            m_say.add_css_class("error");
            m_run.set_sensitive(false);
        }
    }
    void pick_folder(bool source) {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title(source ? "Pictures in" : "Save to");
        const std::string at = source ? m_batch.source : (m_batch.dest.empty() ? m_dest_kept : m_batch.dest);
        if (!at.empty()) dlg->set_initial_folder(Gio::File::create_for_path(at));
        dlg->select_folder(*this, [this, dlg, source](Glib::RefPtr<Gio::AsyncResult>& res) {
            Glib::RefPtr<Gio::File> f;
            try { f = dlg->select_folder_finish(res); } catch (const Glib::Error&) { return; }
            if (!f || f->get_path().empty()) return;
            if (source) m_batch.source = f->get_path();
            else {
                m_batch.dest = f->get_path();
                m_dest_kept = m_batch.dest;
                m_folder.set_active(true);
            }
            refresh();
        });
    }
};

}  // namespace

void Shell::on_batch() {
    if (m_recording >= 0) { toast("Batch: stop recording first (■)"); return; }
    if (m_actions.empty()) { toast("Batch: no actions -- record one in the Actions tab first"); return; }
    static A::Batch last;   // the dialog as it was left, this run
    if (last.options == formats::WriteOptions{}) last.options = m_prefs.export_options;
    auto* dlg = new BatchDialog(*this, m_actions, m_actions_panel.picked_action(), last, [this](A::Batch b, std::size_t i) {
        last = b;
        b.options = m_prefs.export_options;   // the formats' settings as Export last used them
        const A::Action a = m_actions[i];
        const std::string label = "Batch “" + a.name + "”";
        if (auto lg = log::get(log::Area::Shell)) lg->info("batch: {} over {} -> {}", a.name, b.source, b.dest.empty() ? "beside" : b.dest);
        start_job(label, [this, a, b, label](core::Jobs::Token& t) -> core::Jobs::Landing {
            auto r = std::make_shared<A::BatchResult>(
                A::run_batch(b, a, [&t] { return t.cancelled(); }, [&t](double f) { t.progress(f); }));
            return [this, r, label, b] {
                std::string head = std::to_string(r->done) + (r->done == 1 ? " picture" : " pictures") + " done";
                if (r->skipped) head += ", " + std::to_string(r->skipped) + " skipped";
                if (r->failed) head += ", " + std::to_string(r->failed) + " failed";
                if (r->cancelled) head += " -- cancelled";
                if (auto lg = log::get(log::Area::Shell)) lg->info("batch: {}", head);
                if (r->lines.empty()) { toast(label + ": " + head); return; }
                std::string detail;
                for (std::size_t k = 0; k < r->lines.size() && k < 12; ++k) detail += (k ? "\n" : "") + r->lines[k];
                if (r->lines.size() > 12) detail += "\n… and " + std::to_string(r->lines.size() - 12) + " more";
                auto al = Gtk::AlertDialog::create(label + ": " + head);
                al->set_detail(detail);
                al->set_buttons({"OK"});
                al->show(*this);
            };
        });
    });
    dlg->signal_hide().connect([dlg] { Glib::signal_idle().connect_once([dlg] { delete dlg; }); });
    dlg->present();
}

}  // namespace klimt
