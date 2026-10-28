#include "AdjustDialog.hpp"
#include "Registry.hpp"

#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/headerbar.h>

namespace klimt {

AdjustDialog::AdjustDialog(Gtk::Window& parent)
    : m_ok_button("shell.adjust.ok", "OK"), m_cancel_button("shell.adjust.cancel", "Cancel") {
    set_name("shell.adjust");
    registry::add("shell.adjust", this);
    set_modal(true);
    set_resizable(false);
    set_transient_for(parent);
    set_hide_on_close(true);
    set_default_size(340, -1);
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(false);
    m_ok_button.add_css_class("suggested-action");
    hb->pack_start(m_cancel_button);
    hb->pack_end(m_ok_button);
    set_titlebar(*hb);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_margin(16);
    box->set_size_request(320, -1);
    box->append(m_editor);
    m_preview.set_name("shell.adjust.preview");
    m_preview.set_active(true);
    m_preview.set_tooltip_text("Show it on the canvas while you choose");
    box->append(m_preview);
    set_child(*box);

    m_editor.signal_changed().connect([this](const core::Adjust&) { m_changed.emit(); });
    m_preview.signal_toggled().connect([this] { m_changed.emit(); });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        const core::Adjust a = core::adjust::normalised(m_editor.value());
        close();
        m_ok.emit(a);
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

AdjustDialog::~AdjustDialog() { registry::remove(this); }

void AdjustDialog::ask(core::AdjustKind k, const std::string& layer) {
    set_title(std::string(core::adjust::label(k)) + " -- “" + layer + "”");
    m_editor.set(core::Adjust{});   // fresh controls each time
    m_editor.set(core::adjust::fresh(k));
    present();
}

}  // namespace klimt
