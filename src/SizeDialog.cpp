#include "SizeDialog.hpp"
#include "Registry.hpp"
#include "formats/Formats.hpp"

#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/stringlist.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace klimt {

namespace {
std::string name_of(SizeDialog::Mode m) { return m == SizeDialog::Mode::Image ? "shell.image_size" : "shell.canvas_size"; }
}  // namespace

SizeDialog::SizeDialog(Gtk::Window& parent, Mode mode)
    : m_mode(mode), m_width(name_of(mode) + ".width"), m_height(name_of(mode) + ".height"),
      m_dpi(name_of(mode) + ".dpi"), m_link(name_of(mode) + ".link"), m_filter(name_of(mode) + ".filter"),
      m_info(name_of(mode) + ".info"), m_ok_button(name_of(mode) + ".ok", "OK"),
      m_cancel_button(name_of(mode) + ".cancel", "Cancel") {
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
        s->set_width_chars(7);
        s->set_numeric(true);
    }
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
    label("Width");  grid->attach(m_width, 1, row); unit("px");
    grid->attach(m_link, 3, row, 1, 2); ++row;
    label("Height"); grid->attach(m_height, 1, row); unit("px"); ++row;
    if (mode == Mode::Image) {
        label("Resolution"); grid->attach(m_dpi, 1, row); unit("dpi"); ++row;
        label("Resample");   grid->attach(m_filter, 1, row, 3); ++row;
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
    m_dpi.signal_value_changed().connect([this] { refresh(); });
    // As you type, not only on Enter / leaving the field: the linked side and
    // the line under the fields follow each keystroke.
    const auto typed = [this](widgets::SpinButton& s, void (SizeDialog::*then)()) {
        s.signal_changed().connect([this, &s, then] {
            if (m_syncing) return;
            try {
                const double v = std::stod(s.get_text());
                if (v >= 1 && v <= formats::kMaxSide && v != s.get_value()) {
                    m_syncing = true;
                    s.set_value(v);
                    m_syncing = false;
                    (this->*then)();
                }
            } catch (...) {}
        });
    };
    typed(m_width, &SizeDialog::on_width);
    typed(m_height, &SizeDialog::on_height);
    m_link.signal_toggled().connect([this] { if (m_link.get_active()) on_width(); });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        m_width.update();
        m_height.update();
        m_dpi.update();
        Answer a;
        a.width = static_cast<int>(m_width.get_value());
        a.height = static_cast<int>(m_height.get_value());
        a.dpi = m_dpi.get_value();
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
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { m_ok_button.activate(); return true; }
        return false;
    }, false);
    add_controller(keys);
}

SizeDialog::~SizeDialog() { registry::remove(this); }

void SizeDialog::ask(int width, int height, double dpi) {
    m_w0 = width;
    m_h0 = height;
    m_syncing = true;
    m_width.set_value(width);
    m_height.set_value(height);
    m_dpi.set_value(dpi);
    m_syncing = false;
    set_anchor(1, 1);
    refresh();
    present();
    m_width.grab_focus();
}

void SizeDialog::on_width() {
    if (m_syncing) return;
    if (m_link.get_active() && m_w0 > 0) {
        m_syncing = true;
        m_height.set_value(std::max(1.0, std::round(m_width.get_value() * m_h0 / m_w0)));
        m_syncing = false;
    }
    refresh();
}

void SizeDialog::on_height() {
    if (m_syncing) return;
    if (m_link.get_active() && m_h0 > 0) {
        m_syncing = true;
        m_width.set_value(std::max(1.0, std::round(m_height.get_value() * m_w0 / m_h0)));
        m_syncing = false;
    }
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
    const int w = static_cast<int>(m_width.get_value()), h = static_cast<int>(m_height.get_value());
    char buf[160];
    if (m_mode == Mode::Image) {
        const double dpi = std::max(1.0, m_dpi.get_value());
        std::snprintf(buf, sizeof buf, "Now %d × %d px.  At %.0f dpi: %.2f × %.2f in.", m_w0, m_h0, dpi, w / dpi, h / dpi);
    } else {
        std::snprintf(buf, sizeof buf, "Now %d × %d px.  %+d × %+d px.", m_w0, m_h0, w - m_w0, h - m_h0);
    }
    m_info.set_text(buf);
}

}  // namespace klimt
