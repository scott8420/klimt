#include "ActionsPanel.hpp"

#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/editablelabel.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/separator.h>

// ActionsPanel.cpp -- the Actions tab (s084). Rows are rebuilt from the
// library on every show() (it is small); a click picks, the arrow opens an
// action onto its steps, a double-click on a name renames it in place.

namespace klimt {

namespace {
Gtk::Button* foot_button(const char* icon, const char* name, const char* tip) {
    auto b = Gtk::make_managed<Gtk::Button>();
    b->set_icon_name(icon);
    b->set_name(name);
    b->set_tooltip_text(tip);
    b->add_css_class("flat");
    return b;
}
}  // namespace

ActionsPanel::ActionsPanel()
    : widgets::Box("actions", Gtk::Orientation::VERTICAL),
      m_scroll("actions.scroll"),
      m_list("actions.list"),
      m_status(widgets::unregistered, "actions.status") {
    m_list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_list.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (m_painting || !row) return;
        const auto i = static_cast<std::size_t>(row->get_index());
        if (i >= m_rows.size()) return;
        m_action = m_rows[i].first;
        m_step = m_rows[i].second;
        sync_buttons();
    });
    // Enter / a double-click on a row: play it (Photoshop's).
    // s093 (the canvas sweep): GTK 4's ListBox activates on a SINGLE click by
    // default -- a click to pick "Half Size for the Web" halved the picture.
    // And a double-click on the name renames (its tooltip says so) -- it
    // played as well, and the play's repaint rebuilt the row under the
    // double-click (s092's Layers crash, the same shape).
    m_list.set_activate_on_single_click(false);
    m_list.signal_row_activated().connect([this](Gtk::ListBoxRow*) {
        if (m_painting) return;
        Glib::signal_timeout().connect_once([this] { if (!m_renaming) ask(Do::Play); }, 50);   // the name's editing starts first
    });
    m_scroll.set_child(m_list);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_vexpand(true);
    m_scroll.set_has_frame(true);
    append(m_scroll);
    m_status.set_xalign(0.0f);
    m_status.add_css_class("dim-label");
    m_status.add_css_class("caption");
    m_status.set_margin_top(4);
    m_status.set_wrap(true);
    append(m_status);

    auto foot = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
    foot->set_margin_top(4);
    m_stop = foot_button("media-playback-stop-symbolic", "actions.stop", "Stop recording");
    m_record = foot_button("media-record-symbolic", "actions.record",
                           "Record -- what you do next is added to the picked action (a new one when none is picked)");
    m_play = foot_button("media-playback-start-symbolic", "actions.play",
                         "Play the picked action on this image -- one step (Ctrl+Z undoes it all); a step picked: from that step on");
    m_stop->signal_clicked().connect([this] { ask(Do::Stop); });
    m_record->signal_clicked().connect([this] { ask(Do::Record); });
    m_play->signal_clicked().connect([this] { ask(Do::Play); });
    foot->append(*m_stop);
    foot->append(*m_record);
    foot->append(*m_play);
    auto spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    foot->append(*spacer);
    auto add = foot_button("list-add-symbolic", "actions.new", "New Action -- and start recording it");
    add->signal_clicked().connect([this] { ask(Do::New); });
    foot->append(*add);
    m_batch = foot_button("folder-symbolic", "actions.batch", "Batch… -- the picked action over a folder of pictures");
    m_batch->signal_clicked().connect([this] { ask(Do::Batch); });
    foot->append(*m_batch);
    m_delete = foot_button("user-trash-symbolic", "actions.delete", "Delete the picked action, or the picked step");
    m_delete->signal_clicked().connect([this] { ask(Do::Delete); });
    foot->append(*m_delete);
    append(*foot);
    sync_buttons();
}

void ActionsPanel::ask(Do d, const std::string& text) {
    if (m_on) m_on(d, m_action, m_step, text);
}

void ActionsPanel::close_all() {
    m_open.clear();
    m_renaming = false;
    paint();
}

void ActionsPanel::pick(int action, int step) {
    m_action = action;
    m_step = step;
    if (action >= 0) m_open.insert(action);
    paint();
}

void ActionsPanel::show(const std::vector<core::actions::Action>& all, int recording, bool playing) {
    const bool grew = recording >= 0 && recording < static_cast<int>(all.size()) &&
                      (recording >= static_cast<int>(m_all.size()) || all[static_cast<std::size_t>(recording)].acts.size() != m_all[static_cast<std::size_t>(recording)].acts.size());
    m_all = all;
    m_recording = recording;
    m_playing = playing;
    if (m_action >= static_cast<int>(m_all.size())) { m_action = -1; m_step = -1; }
    if (m_action >= 0 && m_step >= static_cast<int>(m_all[static_cast<std::size_t>(m_action)].acts.size())) m_step = -1;
    if (recording >= 0) {
        m_open.insert(recording);   // recording: its steps show as they come
        if (grew) { m_action = recording; m_step = -1; }
    }
    paint();
}

void ActionsPanel::paint() {
    m_painting = true;
    while (auto* c = m_list.get_first_child()) m_list.remove(*c);
    m_rows.clear();
    Gtk::ListBoxRow* picked = nullptr;
    for (std::size_t ai = 0; ai < m_all.size(); ++ai) {
        const auto& a = m_all[ai];
        const int ia = static_cast<int>(ai);
        const bool open = m_open.count(ia) > 0;
        // The action's own row: the arrow, its name (double-click renames), its steps' count.
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        box->set_margin_start(4);
        box->set_margin_end(8);
        box->set_margin_top(3);
        box->set_margin_bottom(3);
        auto arrow = Gtk::make_managed<Gtk::Button>();
        arrow->set_icon_name(open ? "pan-down-symbolic" : "pan-end-symbolic");
        arrow->add_css_class("flat");
        arrow->set_tooltip_text(open ? "Hide its steps" : "Show its steps");
        arrow->signal_clicked().connect([this, ia] {
            Glib::signal_idle().connect_once([this, ia] {
                if (m_open.count(ia)) m_open.erase(ia); else m_open.insert(ia);
                paint();
            });
        });
        box->append(*arrow);
        if (ia == m_recording) {
            auto dot = Gtk::make_managed<Gtk::Image>();
            dot->set_from_icon_name("media-record-symbolic");
            dot->add_css_class("error");
            dot->set_tooltip_text("Recording");
            box->append(*dot);
        }
        auto name = Gtk::make_managed<Gtk::EditableLabel>(a.name);
        name->set_hexpand(true);
        name->set_tooltip_text("Double-click to rename");
        name->property_editing().signal_changed().connect([this, name, ia] {
            m_renaming = name->get_editing();
            if (m_renaming) return;
            const std::string t = name->get_text();
            if (ia < static_cast<int>(m_all.size()) && !t.empty() && t != m_all[static_cast<std::size_t>(ia)].name) {
                m_action = ia;
                m_step = -1;
                Glib::signal_idle().connect_once([this, t] { ask(Do::Rename, t); });
            }
        });
        box->append(*name);
        auto count = Gtk::make_managed<Gtk::Label>(std::to_string(a.acts.size()) + (a.acts.size() == 1 ? " step" : " steps"));
        count->add_css_class("dim-label");
        count->add_css_class("caption");
        box->append(*count);
        row->set_child(*box);
        m_list.append(*row);
        m_rows.push_back({ia, -1});
        if (ia == m_action && m_step < 0) picked = row;
        if (!open) continue;
        for (std::size_t si = 0; si < a.acts.size(); ++si) {
            const auto& act = a.acts[si];
            const int is = static_cast<int>(si);
            auto srow = Gtk::make_managed<Gtk::ListBoxRow>();
            auto sbox = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            sbox->set_margin_start(30);
            sbox->set_margin_end(8);
            sbox->set_margin_top(2);
            sbox->set_margin_bottom(2);
            auto tick = Gtk::make_managed<Gtk::CheckButton>();
            tick->set_active(act.on);
            tick->set_tooltip_text(act.on ? "Switch this step off -- the action skips it" : "Switch this step on");
            tick->signal_toggled().connect([this, ia, is] {
                if (m_painting) return;
                m_action = ia;
                m_step = is;
                Glib::signal_idle().connect_once([this] { ask(Do::Toggle); });
            });
            sbox->append(*tick);
            auto l = Gtk::make_managed<Gtk::Label>(act.label);
            l->set_xalign(0.0f);
            l->set_hexpand(true);
            l->set_ellipsize(Pango::EllipsizeMode::END);
            if (!act.on) l->add_css_class("dim-label");
            if (!core::actions::known(act.verb)) l->set_tooltip_text("A step this Klimt doesn't know (“" + act.verb + "”) -- it stops the action");
            sbox->append(*l);
            srow->set_child(*sbox);
            m_list.append(*srow);
            m_rows.push_back({ia, is});
            if (ia == m_action && is == m_step) picked = srow;
        }
    }
    if (m_all.empty()) {
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto l = Gtk::make_managed<Gtk::Label>("No actions -- + makes one and starts recording");
        l->add_css_class("dim-label");
        l->set_margin_top(12);
        l->set_margin_bottom(12);
        row->set_child(*l);
        row->set_selectable(false);
        row->set_activatable(false);
        m_list.append(*row);
        m_rows.push_back({-1, -1});
    }
    if (picked) m_list.select_row(*picked);
    else m_list.unselect_all();
    m_painting = false;
    sync_buttons();
}

void ActionsPanel::sync_buttons() {
    const bool rec = m_recording >= 0;
    const bool have = m_action >= 0 && m_action < static_cast<int>(m_all.size());
    m_stop->set_sensitive(rec);
    m_record->set_sensitive(!rec && !m_playing);
    m_play->set_sensitive(!rec && !m_playing && have && !m_all[static_cast<std::size_t>(m_action)].acts.empty());
    m_delete->set_sensitive(have && !m_playing);
    m_batch->set_sensitive(!rec && !m_playing);
    if (rec && m_recording < static_cast<int>(m_all.size())) {
        const auto& a = m_all[static_cast<std::size_t>(m_recording)];
        m_status.set_text("Recording “" + a.name + "” -- " + std::to_string(a.acts.size()) +
                          " steps. Filters, adjustments, Image / Canvas Size, turns, selections, Fill, layers and masks are kept; ■ stops.");
        m_status.remove_css_class("dim-label");
        m_status.add_css_class("error");
    } else {
        m_status.remove_css_class("error");
        m_status.add_css_class("dim-label");
        m_status.set_text(m_playing ? "Playing…" : have ? "▶ plays “" + m_all[static_cast<std::size_t>(m_action)].name + "” on this image -- one step"
                                                        : "Pick an action to play, or + to record a new one");
    }
}

}  // namespace klimt
