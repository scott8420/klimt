#include "NumberDialog.hpp"
#include "Registry.hpp"

#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>

namespace klimt {

NumberDialog::NumberDialog(Gtk::Window& parent, const std::string& name, const std::string& title,
                           const std::string& label, const std::string& unit, double lo, double hi, double value,
                           int digits)
    : m_value(name + ".value"), m_ok_button(name + ".ok", "OK"), m_cancel_button(name + ".cancel", "Cancel") {
    set_name(name);
    registry::add(name, this);
    set_title(title);
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

    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    row->set_margin(20);
    auto* l = Gtk::make_managed<Gtk::Label>(label);
    l->add_css_class("dim-label");
    row->append(*l);
    m_value.set_range(lo, hi);
    m_value.set_digits(static_cast<guint>(digits));
    m_value.set_increments(digits ? 0.5 : 1, 10);
    m_value.set_value(value);
    m_value.set_width_chars(7);
    row->append(m_value);
    auto* u = Gtk::make_managed<Gtk::Label>(unit);
    u->add_css_class("dim-label");
    row->append(*u);
    set_child(*row);

    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        m_value.update();
        close();
        m_ok.emit(m_value.get_value());
    });
    // CAPTURE: the number's entry would take Enter before a bubbling
    // controller heard it (seen under Xvfb: Enter did nothing).
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Escape) { close(); return true; }
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { m_ok_button.activate(); return true; }
        return false;
    }, false);
    add_controller(keys);
}

NumberDialog::~NumberDialog() { registry::remove(this); }

void NumberDialog::ask() {
    present();
    m_value.grab_focus();
}

}  // namespace klimt
