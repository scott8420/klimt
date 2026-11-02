#include "MakePathDialog.hpp"
#include "Registry.hpp"

#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>

namespace klimt {

MakePathDialog::MakePathDialog(Gtk::Window& parent)
    : m_source("makepath.source"), m_tolerance("makepath.tolerance"), m_count(widgets::unregistered, "makepath.count"),
      m_ok_button("makepath.ok", "Make Path"), m_cancel_button("makepath.cancel", "Cancel") {
    set_name("makepath");
    registry::add("makepath", this);
    set_title("Make Path");
    set_modal(true);
    set_resizable(false);
    set_transient_for(parent);
    set_hide_on_close(true);
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(false);
    m_ok_button.add_css_class("suggested-action");
    hb->pack_start(m_cancel_button);
    hb->pack_end(m_ok_button);
    set_titlebar(*hb);

    auto* g = Gtk::make_managed<Gtk::Grid>();
    g->set_margin(20);
    g->set_row_spacing(10);
    g->set_column_spacing(10);
    const auto dim = [](const std::string& t) {
        auto* l = Gtk::make_managed<Gtk::Label>(t);
        l->add_css_class("dim-label");
        l->set_xalign(1);
        return l;
    };
    g->attach(*dim("From"), 0, 0);
    m_source.set_hexpand(true);
    g->attach(m_source, 1, 0, 2, 1);
    g->attach(*dim("Tolerance"), 0, 1);
    m_tolerance.set_range(0.5, 10);
    m_tolerance.set_digits(1);
    m_tolerance.set_increments(0.5, 2);
    m_tolerance.set_width_chars(5);
    m_tolerance.set_tooltip_text("How far the curves may stray from the edge, in pixels -- higher is smoother, with fewer anchors");
    g->attach(m_tolerance, 1, 1);
    auto* u = Gtk::make_managed<Gtk::Label>("px");
    u->add_css_class("dim-label");
    u->set_xalign(0);
    g->attach(*u, 2, 1);
    m_count.set_xalign(0);
    m_count.add_css_class("numeric");
    g->attach(m_count, 1, 2, 2, 1);
    auto* hint = Gtk::make_managed<Gtk::Label>("Corners where the shape turns, curves with as few anchors as they need, "
                                               "straight runs without handles. A hard pixel edge's steps are read as the line they make.");
    hint->set_wrap(true);
    hint->set_max_width_chars(44);
    hint->set_xalign(0);
    hint->add_css_class("dim-label");
    hint->add_css_class("caption");
    g->attach(*hint, 0, 3, 3, 1);
    set_child(*g);

    const auto changed = [this] {
        if (m_asking) return;
        m_changed.emit(m_source.get_selected(), m_tolerance.get_value());
    };
    m_source.property_selected().signal_changed().connect(changed);
    m_tolerance.signal_value_changed().connect(changed);
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        m_tolerance.update();
        const auto i = m_source.get_selected();
        close();
        if (i < m_choices.size()) m_ok.emit(i, m_tolerance.get_value());
    });
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);   // the spin's entry would take Enter first
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Escape) { close(); return true; }
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { m_ok_button.activate(); return true; }
        return false;
    }, false);
    add_controller(keys);
}

MakePathDialog::~MakePathDialog() { registry::remove(this); }

void MakePathDialog::ask(const std::vector<Choice>& choices, std::size_t pick, double tolerance) {
    m_asking = true;
    m_choices = choices;
    std::vector<Glib::ustring> names;
    for (const auto& c : choices) names.push_back(c.label);
    m_source.set_model(Gtk::StringList::create(names));
    m_source.set_selected(static_cast<guint>(std::min(pick, choices.empty() ? 0 : choices.size() - 1)));
    m_tolerance.set_value(tolerance);
    m_count.set_text("…");
    m_asking = false;
    present();
    m_source.grab_focus();
    m_changed.emit(m_source.get_selected(), m_tolerance.get_value());
}

void MakePathDialog::set_count(const std::string& text) { m_count.set_text(text); }

}  // namespace klimt
