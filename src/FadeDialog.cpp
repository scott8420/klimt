#include "FadeDialog.hpp"
#include "Registry.hpp"
#include "core/Document.hpp"   // blend_modes()

#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>

namespace klimt {

FadeDialog::FadeDialog(Gtk::Window& parent)
    : m_what("fade.what"), m_slider("fade.opacity"), m_number("fade.opacity_n"), m_mode("fade.mode"),
      m_busy("fade.busy"), m_ok_button("fade.ok", "OK"), m_cancel_button("fade.cancel", "Cancel") {
    set_name("fade");
    registry::add("fade", this);
    set_title("Fade");
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

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    body->set_margin(20);
    m_what.set_xalign(0.f);
    m_what.add_css_class("heading");
    body->append(m_what);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(10);
    grid->set_column_spacing(10);
    const auto label = [&](const char* text, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(0.f);
        l->add_css_class("dim-label");
        grid->attach(*l, 0, row);
    };
    label("Opacity", 0);
    m_slider.set_range(0, 100);
    m_slider.set_increments(1, 10);
    m_slider.set_draw_value(false);
    m_slider.set_hexpand(true);
    m_slider.set_size_request(240, -1);
    m_slider.set_value(100);
    grid->attach(m_slider, 1, 0);
    m_number.set_range(0, 100);
    m_number.set_increments(1, 10);
    m_number.set_digits(0);
    m_number.set_width_chars(4);
    m_number.set_value(100);
    grid->attach(m_number, 2, 0);
    auto* pct = Gtk::make_managed<Gtk::Label>("%");
    pct->add_css_class("dim-label");
    grid->attach(*pct, 3, 0);

    label("Mode", 1);
    std::vector<Glib::ustring> labels;
    for (const auto& b : core::blend_modes()) {
        if (b.mode == pixel::Blend::PassThrough) continue;   // groups only
        m_modes.push_back(b.mode);
        labels.emplace_back(b.label);
    }
    m_mode.set_model(Gtk::StringList::create(labels));
    grid->attach(m_mode, 1, 1, 3, 1);
    body->append(*grid);

    auto* foot = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    m_preview.set_active(true);
    foot->append(m_preview);
    m_busy.set_hexpand(true);
    m_busy.set_xalign(1.f);
    m_busy.add_css_class("dim-label");
    foot->append(m_busy);
    body->append(*foot);
    set_child(*body);

    m_slider.signal_value_changed().connect([this] {
        if (m_syncing) return;
        m_syncing = true;
        m_number.set_value(m_slider.get_value());
        m_syncing = false;
        m_changed.emit();
    });
    m_number.signal_value_changed().connect([this] {
        if (m_syncing) return;
        m_syncing = true;
        m_slider.set_value(m_number.get_value());
        m_syncing = false;
        m_changed.emit();
    });
    m_mode.property_selected().signal_changed().connect([this] { if (!m_syncing) m_changed.emit(); });
    m_preview.signal_toggled().connect([this] { m_changed.emit(); });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        m_number.update();
        m_ok.emit();
        close();
    });
    signal_hide().connect([this] { m_closed.emit(); });
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Escape) { close(); return true; }
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { m_ok_button.activate(); return true; }
        return false;
    }, false);
    add_controller(keys);
}

FadeDialog::~FadeDialog() { registry::remove(this); }

void FadeDialog::ask(const std::string& what, bool rgb) {
    set_title("Fade " + what);
    m_what.set_text("Fade “" + what + "”");
    // Non-RGB: Normal only (the other modes' maths are RGB's for now).
    m_syncing = true;
    if (!rgb) m_mode.set_selected(0);
    m_syncing = false;
    m_mode.set_sensitive(rgb);
    m_mode.set_tooltip_text(rgb ? "" : "Normal only in Gray, CMYK and Lab documents, for now");
    set_busy(false);
    present();
    m_number.grab_focus();
}

double FadeDialog::opacity() const { return m_number.get_value() / 100.0; }

pixel::Blend FadeDialog::mode() const {
    const auto i = m_mode.get_selected();
    return i < m_modes.size() ? m_modes[i] : pixel::Blend::Normal;
}

void FadeDialog::set_busy(bool b) { m_busy.set_text(b ? "Previewing…" : ""); }

}  // namespace klimt
