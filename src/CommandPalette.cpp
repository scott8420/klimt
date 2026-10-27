#include "CommandPalette.hpp"
#include "Log.hpp"
#include "core/Commands.hpp"

#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <glibmm/variant.h>

#include <vector>

namespace klimt {

namespace {
// The catalog index each row runs (rows are rebuilt per keystroke).
std::vector<const core::Command*> g_rows;
}

CommandPalette::CommandPalette(Gtk::ApplicationWindow& parent)
    : m_parent(parent), m_scroll("palette.scroll"), m_list("palette.list") {
    set_name("palette");
    set_title("Find a command");
    set_transient_for(parent);
    set_modal(true);
    set_hide_on_close(true);
    set_default_size(560, 420);
    set_resizable(false);

    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(12);
    m_query.set_placeholder_text("Type a command -- curves, sort, duplicate, brush ...");
    m_query.set_hexpand(true);
    m_query.signal_search_changed().connect(sigc::mem_fun(*this, &CommandPalette::refill));
    m_query.signal_activate().connect([this] {
        auto* row = m_list.get_selected_row();
        run_row(row ? row : m_list.get_row_at_index(0));
    });
    m_query.signal_stop_search().connect([this] { set_visible(false); });
    // Up / Down move the choice while the keyboard stays in the field.
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType) {
        if (keyval == GDK_KEY_Down) { move(+1); return true; }
        if (keyval == GDK_KEY_Up)   { move(-1); return true; }
        if (keyval == GDK_KEY_Escape) { set_visible(false); return true; }
        return false;
    }, false);
    add_controller(keys);
    box->append(m_query);

    m_list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_list.set_activate_on_single_click(true);
    m_list.signal_row_activated().connect(sigc::mem_fun(*this, &CommandPalette::run_row));
    m_scroll.set_child(m_list);
    m_scroll.set_vexpand(true);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_has_frame(true);
    box->append(m_scroll);
    set_child(*box);
}

void CommandPalette::open() {
    m_query.set_text("");
    refill();
    present();
    m_query.grab_focus();
}

void CommandPalette::refill() {
    while (auto* c = m_list.get_first_child()) m_list.remove(*c);
    g_rows = core::search(m_query.get_text());
    for (const auto* c : g_rows) {
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        line->set_margin_start(10);
        line->set_margin_end(10);
        line->set_margin_top(7);
        line->set_margin_bottom(7);
        auto title = Gtk::make_managed<Gtk::Label>(c->title);
        title->set_xalign(0.0f);
        line->append(*title);
        auto section = Gtk::make_managed<Gtk::Label>(c->section);
        section->add_css_class("dim-label");
        section->add_css_class("caption");
        section->set_hexpand(true);
        section->set_xalign(0.0f);
        line->append(*section);
        if (!c->keys.empty()) {
            auto keys = Gtk::make_managed<Gtk::Label>(c->keys);
            keys->add_css_class("dim-label");
            line->append(*keys);
        }
        row->set_child(*line);
        m_list.append(*row);
    }
    if (auto* first = m_list.get_row_at_index(0)) m_list.select_row(*first);
}

void CommandPalette::move(int by) {
    auto* row = m_list.get_selected_row();
    const int at = row ? row->get_index() + by : 0;
    if (auto* next = m_list.get_row_at_index(at)) {
        m_list.select_row(*next);
        next->grab_focus();
        m_query.grab_focus_without_selecting();
    }
}

void CommandPalette::run_row(Gtk::ListBoxRow* row) {
    if (!row) return;
    const auto i = static_cast<std::size_t>(row->get_index());
    if (i >= g_rows.size()) return;
    const core::Command c = *g_rows[i];
    set_visible(false);   // first: the command may open a window or take the keyboard
    const auto name = c.action.substr(c.action.find('.') + 1);   // "win.tool" -> "tool"
    if (!m_parent.lookup_action(name)) {
        if (auto lg = log::get(log::Area::Shell)) lg->warn("palette: no action {}", c.action);
        return;
    }
    if (auto lg = log::get(log::Area::Shell)) lg->info("palette: {}", c.title);
    if (c.target.empty()) m_parent.activate_action(c.action);
    else m_parent.activate_action(c.action, Glib::Variant<Glib::ustring>::create(c.target));
}

}  // namespace klimt
