#include "HistoryPanel.hpp"

#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/image.h>
#include <glibmm/main.h>

namespace klimt {

HistoryPanel::HistoryPanel(const core::DocJournal& journal)
    : widgets::Box("history", Gtk::Orientation::VERTICAL),
      m_journal(journal),
      m_scroll("history.scroll"),
      m_list("history.list") {
    m_list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_list.set_activate_on_single_click(true);
    m_list.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        if (m_painting || !row || !m_go) return;
        // Not here: walking the journal repaints this list, which would tear
        // down the row whose click is still being handled (jot s045). Idle.
        const auto to = static_cast<std::size_t>(row->get_index());   // row i = position i
        Glib::signal_idle().connect_once([this, to] { if (m_go) m_go(to); });
    });
    m_scroll.set_child(m_list);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_vexpand(true);
    m_scroll.set_has_frame(true);
    // s077: the snapshots, above the steps (hidden while there are none).
    m_snaps.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_snaps.set_activate_on_single_click(true);
    m_snaps.set_visible(false);
    m_snaps.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        if (m_snap_painting || !row || !m_snap) return;
        const auto i = static_cast<std::size_t>(row->get_index());
        m_picked = static_cast<int>(i);
        if (m_delete) m_delete->set_sensitive(true);
        Glib::signal_idle().connect_once([this, i] { if (m_snap) m_snap(Snap::Restore, i); });
    });
    append(m_snaps);
    append(m_scroll);
    // The footer: take one, delete the one last clicked.
    auto foot = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    foot->set_margin_top(4);
    auto spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    foot->append(*spacer);
    auto take = Gtk::make_managed<Gtk::Button>();
    take->set_icon_name("camera-photo-symbolic");
    take->set_name("history.snapshot_new");
    take->set_tooltip_text("New Snapshot -- the document as it is now, to come back to");
    take->add_css_class("flat");
    take->signal_clicked().connect([this] { if (m_snap) m_snap(Snap::Take, 0); });
    foot->append(*take);
    m_delete = Gtk::make_managed<Gtk::Button>();
    m_delete->set_icon_name("user-trash-symbolic");
    m_delete->set_name("history.snapshot_delete");
    m_delete->set_tooltip_text("Delete Snapshot -- the one last clicked (the document isn't changed)");
    m_delete->add_css_class("flat");
    m_delete->set_sensitive(false);
    m_delete->signal_clicked().connect([this] {
        if (m_snap && m_picked >= 0 && m_picked < static_cast<int>(m_names.size())) m_snap(Snap::Delete, static_cast<std::size_t>(m_picked));
    });
    foot->append(*m_delete);
    append(*foot);
    paint();
}

void HistoryPanel::set_snapshots(const std::vector<std::string>& names) {   // s077
    if (names == m_names) return;
    m_names = names;
    if (m_picked >= static_cast<int>(names.size())) m_picked = -1;
    m_snap_painting = true;
    while (auto* c = m_snaps.get_first_child()) m_snaps.remove(*c);
    for (const auto& n : names) {
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        box->set_margin_start(10);
        box->set_margin_end(10);
        box->set_margin_top(6);
        box->set_margin_bottom(6);
        auto icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("camera-photo-symbolic");
        box->append(*icon);
        auto l = Gtk::make_managed<Gtk::Label>(n);
        l->set_xalign(0.0f);
        l->set_ellipsize(Pango::EllipsizeMode::END);
        box->append(*l);
        row->set_child(*box);
        row->set_tooltip_text("Back to “" + n + "” -- one step; Ctrl+Z undoes it");
        m_snaps.append(*row);
    }
    m_snaps.unselect_all();
    m_snaps.set_visible(!names.empty());
    if (m_delete) m_delete->set_sensitive(m_picked >= 0);
    m_snap_painting = false;
}

void HistoryPanel::repaint() {
    if (m_queued) return;
    m_queued = true;
    Glib::signal_idle().connect_once([this] {
        m_queued = false;
        paint();
    });
}

void HistoryPanel::paint() {
    m_painting = true;
    while (auto* c = m_list.get_first_child()) m_list.remove(*c);
    const auto at = m_journal.position();
    const auto add = [&](const std::string& text, std::size_t i) {
        auto row = Gtk::make_managed<Gtk::ListBoxRow>();
        auto l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(0.0f);
        l->set_margin_start(10);
        l->set_margin_end(10);
        l->set_margin_top(6);
        l->set_margin_bottom(6);
        l->set_ellipsize(Pango::EllipsizeMode::END);
        if (i > at) l->add_css_class("dim-label");   // undone: redoable
        row->set_child(*l);
        m_list.append(*row);
        if (i == at) m_list.select_row(*row);
    };
    add("Opened", 0);
    for (std::size_t i = 0; i < m_journal.size(); ++i) add(m_journal.label(i), i + 1);
    m_painting = false;
}

}  // namespace klimt
