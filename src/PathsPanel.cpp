#include "PathsPanel.hpp"

#include <gtkmm/entry.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/image.h>
#include <gtkmm/popover.h>
#include <gtkmm/scrolledwindow.h>
#include <glibmm/main.h>
#include <glibmm/markup.h>

namespace klimt {

PathsPanel::PathsPanel() : Gtk::Box(Gtk::Orientation::VERTICAL, 6) {
    set_name("dock.paths");
    set_margin_start(8);
    set_margin_end(8);
    set_margin_bottom(8);

    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_vexpand(true);
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_list.set_name("dock.paths.list");
    m_list.set_selection_mode(Gtk::SelectionMode::SINGLE);
    m_list.add_css_class("navigation-sidebar");
    scroll->set_child(m_list);
    append(*scroll);
    m_empty.set_text("No paths. Draw one with the Pen (P) -- click for corners, drag for curves -- "
                     "or select something and make a work path from it (the pen button below).");
    m_empty.set_wrap(true);
    m_empty.set_xalign(0.0f);
    m_empty.add_css_class("dim-label");
    append(m_empty);

    // Make Work Path's tolerance (Photoshop's: 0.5 .. 10 px).
    auto* tol = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto* tl = Gtk::make_managed<Gtk::Label>("Work path tolerance");
    tl->set_xalign(0.0f);
    tl->set_hexpand(true);
    tl->add_css_class("dim-label");
    tol->append(*tl);
    m_tol.set_name("dock.paths.tolerance");
    m_tol.set_range(0.5, 10);
    m_tol.set_increments(0.5, 1);
    m_tol.set_digits(1);
    m_tol.set_width_chars(4);
    m_tol.set_value(2.0);
    m_tol.set_tooltip_text("How closely a work path made from the selection follows its edge, in pixels: "
                           "smaller -- more anchors, closer; larger -- fewer, smoother");
    tol->append(m_tol);
    auto* px = Gtk::make_managed<Gtk::Label>("px");
    px->add_css_class("dim-label");
    tol->append(*px);
    append(*tol);
    m_tol.signal_value_changed().connect([this] { if (!m_syncing) m_tolerance.emit(m_tol.get_value()); });

    auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    const auto setup = [](Gtk::Button& b, const char* name, const char* icon, const char* tip) {
        b.set_name(name);
        b.set_icon_name(icon);
        b.set_tooltip_text(tip);
        b.add_css_class("flat");
    };
    setup(m_load_button, "dock.paths.load", "edit-select-all-symbolic",
          "Load the path as a selection (Ctrl+Enter; Ctrl-click a row -- Shift adds, Alt subtracts)");
    setup(m_make_button, "dock.paths.make", "klimt-tool-pen-symbolic", "Make Work Path: the selection's edge as a path");
    setup(m_mask_button, "dock.paths.mask", "klimt-layer-mask-symbolic", "Mask from Path: the picked layer shows the path's area");
    setup(m_save_button, "dock.paths.save", "list-add-symbolic", "Save the Work Path (a saved path: duplicate it)");
    setup(m_remove_button, "dock.paths.remove", "user-trash-symbolic", "Delete the path");
    auto* gap = Gtk::make_managed<Gtk::Box>();
    gap->set_hexpand(true);
    bar->append(m_load_button);
    bar->append(m_make_button);
    bar->append(m_mask_button);
    bar->append(*gap);
    bar->append(m_save_button);
    bar->append(m_remove_button);
    append(*bar);

    m_load_button.signal_clicked().connect([this] { if (picked_index() > -2) m_load.emit(picked_index(), 0); });
    m_make_button.signal_clicked().connect([this] { m_make.emit(); });
    m_mask_button.signal_clicked().connect([this] { if (picked_index() > -2) m_mask.emit(picked_index()); });
    m_save_button.signal_clicked().connect([this] { if (picked_index() > -2) m_save.emit(picked_index()); });
    m_remove_button.signal_clicked().connect([this] { if (picked_index() > -2) m_remove.emit(picked_index()); });

    // A click picks (Ctrl: loads, the modifiers say how); a double-click renames.
    auto click = Gtk::GestureClick::create();
    click->set_button(GDK_BUTTON_PRIMARY);
    click->signal_released().connect([this, click](int n, double, double y) {
        auto* row = m_list.get_row_at_y(static_cast<int>(y));
        if (!row) { m_list.unselect_all(); m_picked = -2; m_pick.emit(-2); return; }   // below the rows: none
        const int r = row->get_index();
        if (r < 0 || r >= static_cast<int>(m_items.size())) return;
        const int index = m_items[static_cast<std::size_t>(r)].index;
        if (n == 2 && index >= 0) { rename_row(r); return; }
        if (n != 1) return;
        const auto st = click->get_current_event_state();
        if ((st & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{}) {
            const bool shift = (st & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
            const bool alt = (st & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
            m_load.emit(index, shift && alt ? 3 : shift ? 1 : alt ? 2 : 0);
            return;
        }
        m_picked = index;
        m_pick.emit(index);
    });
    m_list.add_controller(click);
    show({}, -2, false, false, 2.0);
}

int PathsPanel::picked_index() const { return m_picked; }

void PathsPanel::show(const std::vector<Item>& items, int picked, bool can_trace, bool can_mask, double tolerance) {
    m_syncing = true;
    m_tol.set_value(tolerance);
    m_syncing = false;
    const bool same = items.size() == m_items.size() &&
                      std::equal(items.begin(), items.end(), m_items.begin(), [](const Item& a, const Item& b) {
                          return a.index == b.index && a.name == b.name && a.anchors == b.anchors;
                      });
    if (!same) {
        m_items = items;
        while (auto* c = m_list.get_first_child()) m_list.remove(*c);
        for (const auto& it : m_items) {
            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            row->set_margin(4);
            auto* icon = Gtk::make_managed<Gtk::Image>();
            icon->set_from_icon_name("klimt-tool-pen-symbolic");
            row->append(*icon);
            auto* l = Gtk::make_managed<Gtk::Label>();
            // The Work Path in italics, as Photoshop's (it's temporary until saved).
            const std::string name = Glib::Markup::escape_text(it.name);
            l->set_markup(it.index < 0 ? "<i>" + name + "</i>" : name);
            l->set_xalign(0.0f);
            l->set_hexpand(true);
            l->set_ellipsize(Pango::EllipsizeMode::END);
            row->append(*l);
            auto* n = Gtk::make_managed<Gtk::Label>(std::to_string(it.anchors));
            n->add_css_class("dim-label");
            n->set_tooltip_text("Anchors");
            row->append(*n);
            m_list.append(*row);
        }
    }
    m_picked = -2;
    m_list.unselect_all();
    for (std::size_t i = 0; i < m_items.size(); ++i)
        if (m_items[i].index == picked) {
            m_picked = picked;
            m_list.select_row(*m_list.get_row_at_index(static_cast<int>(i)));
        }
    m_empty.set_visible(m_items.empty());
    const bool any = m_picked > -2;
    m_load_button.set_sensitive(any);
    m_mask_button.set_sensitive(any && can_mask);
    m_save_button.set_sensitive(any);
    m_remove_button.set_sensitive(any);
    m_make_button.set_sensitive(can_trace);
}

void PathsPanel::rename_row(int r) {
    auto* row = m_list.get_row_at_index(r);
    if (!row) return;
    auto* pop = Gtk::make_managed<Gtk::Popover>();
    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(m_items[static_cast<std::size_t>(r)].name);
    entry->set_width_chars(18);
    pop->set_child(*entry);
    pop->set_parent(*row);
    const int index = m_items[static_cast<std::size_t>(r)].index;
    entry->signal_activate().connect([this, pop, entry, index] {
        const std::string name = entry->get_text();
        pop->popdown();
        if (!name.empty()) m_rename.emit(index, name);
    });
    pop->signal_closed().connect([pop] { Glib::signal_idle().connect_once([pop] { pop->unparent(); }); });
    pop->popup();
    entry->grab_focus();
}

}  // namespace klimt
