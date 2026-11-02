#include "ClipDialog.hpp"
#include "Registry.hpp"
#include "core/Clip.hpp"

#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>

namespace klimt {

ClipDialog::ClipDialog(Gtk::Window& parent)
    : m_path("clip.path"), m_flatness("clip.flatness"), m_ok_button("clip.ok", "OK"), m_cancel_button("clip.cancel", "Cancel") {
    set_name("clip");
    registry::add("clip", this);
    set_title("Clipping Path");
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
    g->attach(*dim("Path"), 0, 0);
    m_path.set_hexpand(true);
    g->attach(m_path, 1, 0, 2, 1);
    g->attach(*dim("Flatness"), 0, 1);
    m_flatness.set_range(0, core::clip::kMaxFlatness);
    m_flatness.set_digits(1);
    m_flatness.set_increments(0.5, 2);
    m_flatness.set_width_chars(6);
    g->attach(m_flatness, 1, 1);
    auto* u = Gtk::make_managed<Gtk::Label>("device pixels");
    u->add_css_class("dim-label");
    u->set_xalign(0);
    g->attach(*u, 2, 1);
    auto* hint = Gtk::make_managed<Gtk::Label>("Where the picture is placed (InDesign, Composix), only what's inside the path shows. "
                                               "EPS, TIFF and PSD exports carry it. Flatness 0 lets the printer choose; "
                                               "1 - 3 for a desktop printer, 8 - 10 for a high-res press.");
    hint->set_wrap(true);
    hint->set_max_width_chars(46);
    hint->set_xalign(0);
    hint->add_css_class("dim-label");
    hint->add_css_class("caption");
    g->attach(*hint, 0, 2, 3, 1);
    set_child(*g);

    m_path.property_selected().signal_changed().connect([this] { m_flatness.set_sensitive(m_path.get_selected() > 0); });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        m_flatness.update();
        const auto i = m_path.get_selected();
        const std::string id = i > 0 && i <= m_paths.size() ? m_paths[i - 1].id : std::string{};
        close();
        m_ok.emit(id, m_flatness.get_value());
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

ClipDialog::~ClipDialog() { registry::remove(this); }

void ClipDialog::ask(const std::vector<Choice>& paths, const std::string& current, double flatness) {
    m_paths = paths;
    std::vector<Glib::ustring> names{"None"};
    guint pick = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        names.push_back(paths[i].name);
        if (paths[i].id == current) pick = static_cast<guint>(i + 1);
    }
    m_path.set_model(Gtk::StringList::create(names));
    m_path.set_selected(pick);
    m_flatness.set_value(flatness);
    m_flatness.set_sensitive(pick > 0);
    present();
    m_path.grab_focus();
}

}  // namespace klimt
