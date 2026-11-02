#include "RawDialog.hpp"
#include "Registry.hpp"

#include <gdkmm/memorytexture.h>
#include <glibmm/bytes.h>
#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>

#include <cstdio>
#include <filesystem>

// RawDialog.cpp -- Develop (s085). See RawDialog.hpp.

namespace klimt {

namespace R = core::raw;

namespace {
void add_row(Gtk::Grid& g, int row, const char* label, Gtk::Widget& w) {
    auto* l = Gtk::make_managed<Gtk::Label>(label);
    l->set_xalign(1.0f);
    l->add_css_class("dim-label");
    g.attach(*l, 0, row);
    w.set_hexpand(true);
    g.attach(w, 1, row);
}
void scale(Gtk::Scale& s, double lo, double hi, double step, int digits) {
    s.set_range(lo, hi);
    s.set_increments(step, step * 5);
    s.set_digits(digits);
    s.set_draw_value(true);
    s.set_value_pos(Gtk::PositionType::RIGHT);
    s.set_size_request(220, -1);
}
}  // namespace

RawDialog::RawDialog(Gtk::Window& parent)
    : m_picture("raw.picture"),
      m_info(widgets::unregistered, "raw.info"),
      m_what(widgets::unregistered, "raw.what"),
      m_balance("raw.balance"),
      m_highlights("raw.highlights"),
      m_space("raw.space"),
      m_warmth("raw.warmth", Gtk::Orientation::HORIZONTAL),
      m_exposure("raw.exposure", Gtk::Orientation::HORIZONTAL),
      m_noise("raw.noise", Gtk::Orientation::HORIZONTAL),
      m_auto("raw.auto_bright"),
      m_ok_button("raw.ok", "Open"),
      m_cancel_button("raw.cancel", "Cancel"),
      m_reset("raw.reset", "Reset") {
    set_name("raw");
    registry::add("raw", this);
    set_title("Develop");
    set_modal(true);
    set_transient_for(parent);
    set_hide_on_close(true);
    set_default_size(980, 600);
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(false);
    m_ok_button.add_css_class("suggested-action");
    hb->pack_start(m_cancel_button);
    hb->pack_end(m_ok_button);
    set_titlebar(*hb);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 16);
    body->set_margin(16);
    auto* left = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    left->set_hexpand(true);
    m_picture.set_content_fit(Gtk::ContentFit::CONTAIN);
    m_picture.set_can_shrink(true);
    m_picture.set_hexpand(true);
    m_picture.set_vexpand(true);
    m_picture.set_size_request(560, 400);
    left->append(m_picture);
    m_what.set_xalign(0.0f);
    m_what.add_css_class("dim-label");
    m_what.add_css_class("caption");
    left->append(m_what);
    body->append(*left);

    auto* g = Gtk::make_managed<Gtk::Grid>();
    g->set_row_spacing(10);
    g->set_column_spacing(12);
    g->set_valign(Gtk::Align::START);
    int row = 0;
    m_info.set_xalign(0.0f);
    m_info.set_wrap(true);
    m_info.set_max_width_chars(36);
    m_info.add_css_class("caption");
    g->attach(m_info, 0, row++, 2, 1);
    m_balance.set_model(Gtk::StringList::create({R::balance_label(R::Balance::AsShot), R::balance_label(R::Balance::Auto),
                                                 R::balance_label(R::Balance::Daylight)}));
    m_balance.set_tooltip_text("As Shot: the camera's own; Auto: the picture's average made grey; Daylight: the sensor's daylight");
    add_row(*g, row++, "White Balance", m_balance);
    scale(m_warmth, -100, 100, 1, 0);
    m_warmth.set_tooltip_text("Cooler (left) or warmer (right) than the white balance chosen -- not with Auto");
    add_row(*g, row++, "Warmth", m_warmth);
    scale(m_exposure, -2, 3, 0.1, 1);
    m_exposure.set_tooltip_text("Stops of light, the highlights kept when brightening");
    add_row(*g, row++, "Exposure (EV)", m_exposure);
    m_auto.set_label("Auto brightness -- the brightest 1 % to white");
    g->attach(m_auto, 1, row++);
    m_highlights.set_model(Gtk::StringList::create({R::highlights_label(R::Highlights::Clip), R::highlights_label(R::Highlights::Blend),
                                                    R::highlights_label(R::Highlights::Rebuild)}));
    m_highlights.set_tooltip_text("Clip: blown highlights white; Blend: eased into colour; Rebuild: their colour guessed from around them");
    add_row(*g, row++, "Highlights", m_highlights);
    scale(m_noise, 0, 1000, 10, 0);
    m_noise.set_tooltip_text("Wavelet noise reduction before the colour is made -- 100 to 300 for a high ISO; 0 off");
    add_row(*g, row++, "Noise Reduction", m_noise);
    m_space.set_model(Gtk::StringList::create({R::space_label(R::Space::SRGB), R::space_label(R::Space::AdobeRGB),
                                               R::space_label(R::Space::ProPhoto)}));
    m_space.set_tooltip_text("The picture's colour: sRGB for screens; Adobe RGB for print; ProPhoto keeps every colour the camera saw "
                             "(the profile goes with the picture). 16 bits a channel always.");
    add_row(*g, row++, "Colour", m_space);
    m_reset.set_halign(Gtk::Align::START);
    m_reset.set_tooltip_text("Back to the starting settings");
    g->attach(m_reset, 1, row++);
    body->append(*g);
    set_child(*body);

    for (auto* d : {&m_balance, &m_highlights, &m_space}) d->property_selected().signal_changed().connect([this] { changed(); });
    for (auto* s : {&m_warmth, &m_exposure, &m_noise}) s->signal_value_changed().connect([this] { changed(); });
    m_auto.signal_toggled().connect([this] { changed(); });
    m_reset.signal_clicked().connect([this] { set({}); changed(); });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        m_opening = true;
        close();
        m_opening = false;
        m_open.emit(m_path, current());
    });
    signal_hide().connect([this] { if (!m_opening) m_gone.emit(); });
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Escape) { close(); return true; }
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { m_ok_button.activate(); return true; }
        return false;
    }, false);
    add_controller(keys);
}

RawDialog::~RawDialog() { registry::remove(this); }

void RawDialog::set(const R::Develop& d) {
    m_setting = true;
    m_balance.set_selected(static_cast<guint>(d.balance));
    m_warmth.set_value(d.warmth);
    m_exposure.set_value(d.exposure);
    m_auto.set_active(d.auto_bright);
    m_highlights.set_selected(static_cast<guint>(d.highlights));
    m_noise.set_value(d.noise);
    m_space.set_selected(static_cast<guint>(d.space));
    m_warmth.set_sensitive(d.balance != R::Balance::Auto);
    m_setting = false;
}

R::Develop RawDialog::current() const {
    R::Develop d;
    d.balance = static_cast<R::Balance>(std::min<guint>(m_balance.get_selected(), 2));
    d.warmth = static_cast<int>(std::lround(m_warmth.get_value()));
    d.exposure = std::round(m_exposure.get_value() * 10) / 10;
    d.auto_bright = m_auto.get_active();
    d.highlights = static_cast<R::Highlights>(std::min<guint>(m_highlights.get_selected(), 2));
    d.noise = static_cast<int>(std::lround(m_noise.get_value()));
    d.space = static_cast<R::Space>(std::min<guint>(m_space.get_selected(), 2));
    return d;
}

void RawDialog::changed() {
    if (m_setting) return;
    m_warmth.set_sensitive(current().balance != R::Balance::Auto);
    m_changed.emit(current());
}

void RawDialog::ask(const std::string& path, const R::Develop& d) {
    m_path = path;
    set_title("Develop — " + std::filesystem::path(path).filename().string());
    m_info.set_text("Reading the file…");
    m_what.set_text("");
    m_picture.set_paintable({});
    set(d);
    present();
    m_ok_button.grab_focus();
}

void RawDialog::show_info(const std::string& line) { m_info.set_text(line); }

void RawDialog::show_picture(int w, int h, std::shared_ptr<const std::vector<std::uint8_t>> rgba, const std::string& what) {
    if (!rgba || w <= 0 || h <= 0 || rgba->size() < static_cast<std::size_t>(w) * h * 4) return;
    auto bytes = Glib::Bytes::create(rgba->data(), rgba->size());
    auto tex = Gdk::MemoryTexture::create(w, h, Gdk::MemoryTexture::Format::R8G8B8A8, bytes, static_cast<gsize>(w) * 4);
    m_picture.set_paintable(tex);
    m_what.set_text(what);
}

}  // namespace klimt
