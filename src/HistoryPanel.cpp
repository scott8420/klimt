#include "HistoryPanel.hpp"

#include <gtkmm/label.h>
#include <gtkmm/listboxrow.h>
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
    append(m_scroll);
    paint();
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
