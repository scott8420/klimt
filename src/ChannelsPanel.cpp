#include "ChannelsPanel.hpp"
#include "Registry.hpp"

#include <gtkmm/entry.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/image.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <glibmm/main.h>

namespace klimt {

ChannelsPanel::ChannelsPanel() : Gtk::Box(Gtk::Orientation::VERTICAL, 6) {
    set_name("dock.channels");
    set_margin_start(8);
    set_margin_end(8);
    set_margin_bottom(8);
    // s026: the colour channels.
    m_colour.set_name("dock.channels.colour");
    m_colour.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_colour.add_css_class("navigation-sidebar");
    append(m_colour);
    auto colour_click = Gtk::GestureClick::create();
    colour_click->set_button(GDK_BUTTON_PRIMARY);
    colour_click->signal_released().connect([this, colour_click](int n, double x, double y) {
        auto* row = m_colour.get_row_at_y(static_cast<int>(y));
        if (!row || n != 1) return;
        // Its own buttons handle themselves.
        if (auto* w = row->pick(x - row->get_allocation().get_x(), y - row->get_allocation().get_y()))
            for (auto* p = w; p && p != row; p = p->get_parent())
                if (dynamic_cast<Gtk::Button*>(p)) return;
        const int k = row->get_index() - 1;   // row 0: the picture
        const auto st = colour_click->get_current_event_state();
        const bool ctrl = (st & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
        if (ctrl && k >= 0) {
            const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
            const bool alt = (st & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
            m_load_colour.emit(k, shift && alt ? 3 : shift ? 1 : alt ? 2 : 0);
            return;
        }
        m_view.emit(k);
    });
    m_colour.add_controller(colour_click);
    auto* sep = Gtk::make_managed<Gtk::Label>();
    sep->set_markup("<small>Saved selections</small>");
    sep->set_xalign(0.f);
    sep->add_css_class("dim-label");
    append(*sep);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_vexpand(true);
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_list.set_name("dock.channels.list");
    m_list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_list.add_css_class("navigation-sidebar");
    scroll->set_child(m_list);
    append(*scroll);
    m_empty.set_text("No saved selections. Select something, then the save button below "
                     "(or Select > Save Selection) keeps it here.");
    m_empty.set_wrap(true);
    m_empty.set_xalign(0.0f);
    m_empty.add_css_class("dim-label");
    append(m_empty);

    auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    m_save_button.set_name("dock.channels.save");
    m_save_button.set_icon_name("list-add-symbolic");
    m_save_button.set_tooltip_text("Save the selection as a channel");
    m_save_button.add_css_class("flat");
    m_remove_button.set_name("dock.channels.remove");
    m_remove_button.set_icon_name("user-trash-symbolic");
    m_remove_button.set_tooltip_text("Delete the picked channel");
    m_remove_button.add_css_class("flat");
    auto* gap = Gtk::make_managed<Gtk::Box>();
    gap->set_hexpand(true);
    bar->append(m_save_button);
    bar->append(*gap);
    bar->append(m_remove_button);
    append(*bar);

    m_save_button.signal_clicked().connect([this] { m_save.emit(); });
    m_remove_button.signal_clicked().connect([this] {
        if (auto* r = m_list.get_selected_row(); r && r->get_index() >= 0 && r->get_index() < static_cast<int>(m_items.size()))
            m_remove.emit(m_items[static_cast<std::size_t>(r->get_index())].id);
    });
    m_list.signal_row_selected().connect([this](Gtk::ListBoxRow* r) { m_remove_button.set_sensitive(r != nullptr); });

    // A click loads (the modifiers say how); a double-click renames.
    auto click = Gtk::GestureClick::create();
    click->set_button(GDK_BUTTON_PRIMARY);
    click->signal_released().connect([this, click](int n, double, double y) {
        auto* row = m_list.get_row_at_y(static_cast<int>(y));
        if (!row) return;
        const int i = row->get_index();
        if (i < 0 || i >= static_cast<int>(m_items.size())) return;
        if (n == 2) { rename_row(i); return; }
        if (n != 1) return;
        const auto st = click->get_current_event_state();
        const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
        const bool alt = (st & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
        const int op = shift && alt ? 3 : shift ? 1 : alt ? 2 : 0;   // select::Op: Replace, Add, Subtract, Intersect
        m_load.emit(m_items[static_cast<std::size_t>(i)].id, op);
    });
    m_list.add_controller(click);
    show({}, false);
}

void ChannelsPanel::show(const std::vector<Item>& items, bool can_save) {
    m_save_button.set_sensitive(can_save);
    const bool same = items.size() == m_items.size() &&
                      std::equal(items.begin(), items.end(), m_items.begin(),
                                 [](const Item& a, const Item& b) { return a.id == b.id && a.name == b.name; });
    if (same) return;
    std::string picked;
    if (auto* r = m_list.get_selected_row(); r && r->get_index() >= 0 && r->get_index() < static_cast<int>(m_items.size()))
        picked = m_items[static_cast<std::size_t>(r->get_index())].id;
    m_items = items;
    while (auto* c = m_list.get_first_child()) m_list.remove(*c);
    for (const auto& it : m_items) {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin(4);
        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("klimt-layer-mask-symbolic");
        row->append(*icon);
        auto* l = Gtk::make_managed<Gtk::Label>(it.name);
        l->set_xalign(0.0f);
        l->set_hexpand(true);
        l->set_ellipsize(Pango::EllipsizeMode::END);
        row->append(*l);
        m_list.append(*row);
    }
    for (std::size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].id == picked) m_list.select_row(*m_list.get_row_at_index(static_cast<int>(i)));
    m_empty.set_visible(m_items.empty());
    m_remove_button.set_sensitive(m_list.get_selected_row() != nullptr);
}

void ChannelsPanel::show_colour(const std::vector<std::string>& names, const std::string& composite, int viewing) {
    if (names != m_colour_names) {
        m_colour_names = names;
        while (auto* c = m_colour.get_first_child()) m_colour.remove(*c);
        for (int k = -1; k < static_cast<int>(names.size()); ++k) {
            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            row->set_margin(2);
            auto* l = Gtk::make_managed<Gtk::Label>(k < 0 ? composite : names[static_cast<std::size_t>(k)]);
            l->set_xalign(0.0f);
            l->set_hexpand(true);
            row->append(*l);
            if (k >= 0) {
                const auto button = [&](const char* icon, const char* tip, auto act) {
                    auto* b = Gtk::make_managed<Gtk::Button>();
                    b->set_icon_name(icon);
                    b->add_css_class("flat");
                    b->set_tooltip_text(tip);
                    b->signal_clicked().connect(act);
                    row->append(*b);
                };
                button("edit-select-all-symbolic", "Load this channel as a selection (or Ctrl-click the row; Shift adds, Alt subtracts)",
                       [this, k] { m_load_colour.emit(k, 0); });
                button("edit-copy-symbolic", "Duplicate: this channel as a saved channel below (to push with Quick Mask)",
                       [this, k] { m_dup_colour.emit(k); });
            }
            m_colour.append(*row);
        }
        if (names.empty()) m_colour.set_visible(false);
        else m_colour.set_visible(true);
    }
    if (auto* r = m_colour.get_row_at_index(viewing + 1)) m_colour.select_row(*r);
}

void ChannelsPanel::rename_row(int index) {
    auto* row = m_list.get_row_at_index(index);
    if (!row) return;
    auto* pop = Gtk::make_managed<Gtk::Popover>();
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(m_items[static_cast<std::size_t>(index)].name);
    entry->set_width_chars(18);
    pop->set_child(*entry);
    pop->set_parent(*row);
    const std::string id = m_items[static_cast<std::size_t>(index)].id;
    entry->signal_activate().connect([this, pop, entry, id] {
        const std::string name = entry->get_text();
        pop->popdown();
        if (!name.empty()) m_rename.emit(id, name);
    });
    pop->signal_closed().connect([pop] { Glib::signal_idle().connect_once([pop] { pop->unparent(); }); });
    pop->popup();
    entry->grab_focus();
}

}  // namespace klimt
