#include "SizeDialog.hpp"
#include "Registry.hpp"
#include "formats/Formats.hpp"

#include <gtkmm/adjustment.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/popover.h>
#include <gtkmm/stringlist.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <tuple>
#include <algorithm>

namespace klimt {

namespace {
std::string name_of(SizeDialog::Mode m) { return m == SizeDialog::Mode::Image ? "shell.image_size" : "shell.canvas_size"; }
}  // namespace

SizeDialog::SizeDialog(Gtk::Window& parent, Mode mode)
    : m_mode(mode), m_width(name_of(mode) + ".width"), m_height(name_of(mode) + ".height"),
      m_dpi(name_of(mode) + ".dpi"), m_link(name_of(mode) + ".link"), m_filter(name_of(mode) + ".filter"),
      m_info(name_of(mode) + ".info"), m_ok_button(name_of(mode) + ".ok", "OK"),
      m_cancel_button(name_of(mode) + ".cancel", "Cancel"), m_units(name_of(mode) + ".units"),
      m_resample(name_of(mode) + ".resample") {
    const std::string name = name_of(mode);
    set_name(name);
    registry::add(name, this);
    set_title(mode == Mode::Image ? "Image Size" : "Canvas Size");
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

    for (auto* s : {&m_width, &m_height}) {
        s->set_range(1, formats::kMaxSide);
        s->set_increments(1, 100);
        s->set_digits(0);
        s->set_width_chars(8);
        s->set_numeric(true);
    }
    {   // s099: px, in, cm, mm, % -- both sides
        std::vector<Glib::ustring> u;
        for (int i = 0; i <= static_cast<int>(core::canvas::Unit::Percent); ++i) u.push_back(core::canvas::unit_name(static_cast<core::canvas::Unit>(i)));
        m_units.set_model(Gtk::StringList::create(u));
        m_units.set_tooltip_text("The unit Width and Height are in -- inches / cm / mm at the Resolution; % of the size now");
        m_units.set_valign(Gtk::Align::CENTER);
    }
    m_resample.set_label("Resample");
    m_resample.set_active(true);
    m_resample.set_tooltip_text("On: a new size makes new pixels (Width × Resolution).\n"
                                "Off: the pixels stay -- a print size or Resolution typed changes the other (dpi only)");
    m_dpi.set_range(1, 10000);
    m_dpi.set_increments(1, 50);
    m_dpi.set_digits(0);
    m_dpi.set_width_chars(7);
    m_link.set_icon_name("insert-link-symbolic");
    m_link.set_active(mode == Mode::Image);   // Image Size keeps the proportions by default
    m_link.set_tooltip_text("Keep the proportions");
    m_link.set_valign(Gtk::Align::CENTER);
    {
        std::vector<Glib::ustring> f{core::canvas::filter_name(core::canvas::Filter::Smooth),
                                     core::canvas::filter_name(core::canvas::Filter::Nearest)};
        m_filter.set_model(Gtk::StringList::create(f));
    }
    m_info.add_css_class("dim-label");
    m_info.set_halign(Gtk::Align::START);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(10);
    grid->set_column_spacing(12);
    grid->set_margin(20);
    int row = 0;
    const auto label = [&](const char* text) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_halign(Gtk::Align::END);
        l->add_css_class("dim-label");
        grid->attach(*l, 0, row);
    };
    const auto unit = [&](const char* text) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_halign(Gtk::Align::START);
        l->add_css_class("dim-label");
        grid->attach(*l, 2, row);
    };
    (void)unit;
    label("Width");  grid->attach(m_width, 1, row); grid->attach(m_units, 2, row);
    grid->attach(m_link, 3, row, 1, 2); ++row;
    label("Height"); grid->attach(m_height, 1, row);
    m_unit_h = Gtk::make_managed<Gtk::Label>("px");
    m_unit_h->set_halign(Gtk::Align::START);
    m_unit_h->add_css_class("dim-label");
    grid->attach(*m_unit_h, 2, row); ++row;
    if (mode == Mode::Image) {
        label("Resolution"); grid->attach(m_dpi, 1, row);
        {
            auto* l = Gtk::make_managed<Gtk::Label>("dpi");
            l->set_halign(Gtk::Align::START);
            l->add_css_class("dim-label");
            grid->attach(*l, 2, row);
        }
        ++row;
        grid->attach(m_resample, 0, row);
        m_resample.set_halign(Gtk::Align::END);
        grid->attach(m_filter, 1, row, 3); ++row;
    } else {
        label("Anchor");
        auto* g = Gtk::make_managed<Gtk::Grid>();
        g->set_row_spacing(2);
        g->set_column_spacing(2);
        g->set_halign(Gtk::Align::START);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x) {
                auto* b = Gtk::make_managed<Gtk::ToggleButton>();
                b->set_size_request(28, 28);
                b->set_tooltip_text(std::string(y == 0 ? "Top" : y == 1 ? "Middle" : "Bottom") + " " +
                                    (x == 0 ? "left" : x == 1 ? "centre" : "right"));
                b->signal_clicked().connect([this, x, y] { set_anchor(x, y); });
                m_anchors[static_cast<std::size_t>(y * 3 + x)] = b;
                g->attach(*b, x, y);
            }
        grid->attach(*g, 1, row, 3);
        ++row;
    }
    grid->attach(m_info, 0, row, 4);
    set_child(*grid);

    m_width.signal_value_changed().connect(sigc::mem_fun(*this, &SizeDialog::on_width));
    m_height.signal_value_changed().connect(sigc::mem_fun(*this, &SizeDialog::on_height));
    m_dpi.signal_value_changed().connect(sigc::mem_fun(*this, &SizeDialog::on_dpi));
    m_units.property_selected().signal_changed().connect([this] {
        if (!m_syncing) set_unit(static_cast<core::canvas::Unit>(m_units.get_selected()));
    });
    m_resample.signal_toggled().connect([this] { set_resample(m_resample.get_active()); });
    // As you type, not only on Enter / leaving the field: the linked side and
    // the line under the fields follow each keystroke.
    // s099: the typed number goes straight to the form -- the field itself
    // isn't set while it's being typed in (GTK warns, and "2" became "2.000").
    const auto typed = [this](widgets::SpinButton& s, bool width) {
        s.signal_changed().connect([this, &s, width] {
            if (m_syncing) return;
            try {
                const double v = std::stod(s.get_text());
                if (v >= s.get_adjustment()->get_lower() && v <= s.get_adjustment()->get_upper()) {
                    m_f.link = m_link.get_active();
                    if (width) m_f.type_width(v); else m_f.type_height(v);
                    show_fields(&s);
                    refresh();
                }
            } catch (...) {}
        });
    };
    typed(m_width, true);
    typed(m_height, false);
    m_link.signal_toggled().connect([this] { if (m_link.get_active()) on_width(); });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        m_width.update();
        m_height.update();
        m_dpi.update();
        Answer a;
        a.width = std::clamp(m_f.out_w(), 1, formats::kMaxSide);   // s099: the form's answer
        a.height = std::clamp(m_f.out_h(), 1, formats::kMaxSide);
        a.dpi = m_mode == Mode::Image ? m_f.dpi : m_f.dpi0;
        a.filter = m_filter.get_selected() == 1 ? core::canvas::Filter::Nearest : core::canvas::Filter::Smooth;
        a.anchor_x = m_ax;
        a.anchor_y = m_ay;
        close();
        m_ok.emit(a);
    });
    // CAPTURE, as NumberDialog: the spin button's entry takes Enter first.
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Escape) { close(); return true; }
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) {
            // s099: Return in an open list (Units, Resample's filter) picks there, not OK.
            for (auto* w = get_focus(); w; w = w->get_parent())
                if (dynamic_cast<Gtk::Popover*>(w)) return false;
            m_ok_button.activate();
            return true;
        }
        return false;
    }, false);
    add_controller(keys);
}

SizeDialog::~SizeDialog() { registry::remove(this); }

void SizeDialog::ask(int width, int height, double dpi) {
    m_w0 = width;
    m_h0 = height;
    const auto unit = m_f.unit;   // kept between asks, as Resample is
    m_f.ask(width, height, dpi);
    m_f.unit = unit;
    m_f.link = m_link.get_active();
    m_syncing = true;
    m_dpi.set_value(m_f.dpi);
    m_syncing = false;
    set_resample(m_mode == Mode::Canvas || m_resample.get_active());
    set_anchor(1, 1);
    show_fields();
    refresh();
    present();
    m_width.grab_focus();
}

// s099: the form -> the fields, in its unit (the one being typed in keeps its text).
void SizeDialog::show_fields(const widgets::SpinButton* skip) {
    namespace C = core::canvas;
    const int dg = C::unit_digits(m_f.unit);
    const double step = dg == 0 ? 1 : dg == 1 ? 0.1 : dg == 2 ? 0.01 : 0.001;
    m_syncing = true;
    for (auto [s, v, base] : {std::tuple{&m_width, m_f.width(), double(m_f.w0)}, std::tuple{&m_height, m_f.height(), double(m_f.h0)}}) {
        s->set_sensitive(m_f.editable());
        if (s == skip) continue;
        s->set_digits(static_cast<guint>(dg));
        s->set_range(C::to_unit(1, m_f.unit, m_f.dpi, base) * 0.999, C::to_unit(formats::kMaxSide, m_f.unit, m_f.dpi, base));
        s->set_increments(step, step * 10);
        s->set_value(v);
    }
    if (std::abs(m_dpi.get_value() - m_f.dpi) > 1e-9) m_dpi.set_value(m_f.dpi);
    m_syncing = false;
    if (m_unit_h) m_unit_h->set_text(C::unit_name(m_f.unit));
}

void SizeDialog::set_unit(core::canvas::Unit u) {
    m_f.unit = u;
    if (m_units.get_selected() != static_cast<guint>(u)) {
        m_syncing = true;
        m_units.set_selected(static_cast<guint>(u));
        m_syncing = false;
    }
    show_fields();
    refresh();
}

void SizeDialog::set_resample(bool on) {
    if (m_mode == Mode::Canvas) on = true;   // Canvas Size never resamples
    if (m_resample.get_active() != on) { m_resample.set_active(on); return; }   // toggled -> here again
    m_f.set_resample(on);
    m_filter.set_sensitive(on);
    if (!on) { m_syncing = true; m_link.set_active(true); m_syncing = false; }
    m_link.set_sensitive(on);
    show_fields();
    refresh();
}

void SizeDialog::on_width() {
    if (m_syncing) return;
    m_f.link = m_link.get_active();
    m_f.type_width(m_width.get_value());
    show_fields(&m_width);
    refresh();
}

void SizeDialog::on_height() {
    if (m_syncing) return;
    m_f.link = m_link.get_active();
    m_f.type_height(m_height.get_value());
    show_fields(&m_height);
    refresh();
}

void SizeDialog::on_dpi() {
    if (m_syncing) return;
    m_f.type_dpi(m_dpi.get_value());
    show_fields();
    refresh();
}

void SizeDialog::set_anchor(int x, int y) {
    m_ax = x;
    m_ay = y;
    for (int i = 0; i < 9; ++i)
        if (auto* b = m_anchors[static_cast<std::size_t>(i)]) {
            b->set_active(i == y * 3 + x);
            b->set_icon_name(i == y * 3 + x ? "media-record-symbolic" : "");
        }
}

void SizeDialog::refresh() {
    const int w = std::clamp(m_f.out_w(), 1, formats::kMaxSide), h = std::clamp(m_f.out_h(), 1, formats::kMaxSide);
    char buf[200];
    if (m_mode == Mode::Image) {
        const double dpi = std::max(1.0, m_f.dpi);
        if (!m_f.resample)
            std::snprintf(buf, sizeof buf, "%d × %d px, unchanged.  At %.0f dpi: %.2f × %.2f in.", w, h, dpi, w / dpi, h / dpi);
        else
            std::snprintf(buf, sizeof buf, "%d × %d px now -> %d × %d px.  At %.0f dpi: %.2f × %.2f in.", m_w0, m_h0, w, h, dpi, w / dpi, h / dpi);
    } else {
        std::snprintf(buf, sizeof buf, "%d × %d px now -> %d × %d px.  %+d × %+d px.", m_w0, m_h0, w, h, w - m_w0, h - m_h0);
    }
    m_info.set_text(buf);
}

}  // namespace klimt
