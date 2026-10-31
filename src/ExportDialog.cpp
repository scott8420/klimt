// ExportDialog -- the format's own settings (s050). See ExportDialog.hpp.
#include "ExportDialog.hpp"
#include "Registry.hpp"

#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/stringlist.h>

#include <cmath>

namespace klimt {

namespace {
Gtk::Label* dim(const std::string& text, bool hint = false) {
    auto* l = Gtk::make_managed<Gtk::Label>(text);
    l->add_css_class("dim-label");
    l->set_xalign(hint ? 0.0f : 1.0f);
    if (hint) {
        l->set_wrap(true);
        l->set_max_width_chars(46);
        l->add_css_class("caption");
    }
    return l;
}

Gtk::Grid* page() {
    auto* g = Gtk::make_managed<Gtk::Grid>();
    g->set_row_spacing(10);
    g->set_column_spacing(12);
    return g;
}

void scale_row(Gtk::Grid& g, int row, const std::string& label, Gtk::Scale& s, Gtk::Label& value, double lo,
               double hi) {
    if (!label.empty()) g.attach(*dim(label), 0, row);   // else the caller's own label
    s.set_range(lo, hi);
    s.set_increments(1, 10);
    s.set_digits(0);
    s.set_round_digits(0);
    s.set_draw_value(false);
    s.set_hexpand(true);
    s.set_size_request(240, -1);
    g.attach(s, 1, row);
    value.set_width_chars(4);
    value.set_xalign(1);
    value.add_css_class("numeric");
    g.attach(value, 2, row);
}

int as_int(const Gtk::Scale& s) { return static_cast<int>(std::lround(s.get_value())); }
}  // namespace

ExportDialog::ExportDialog(Gtk::Window& parent)
    : m_heading("export.heading"),
      m_pages("export.pages"),
      m_jpeg_quality("export.jpeg.quality"),
      m_webp_quality("export.webp.quality"),
      m_png_level("export.png.level"),
      m_jpeg_value(widgets::unregistered, "export.jpeg.value"),
      m_webp_value(widgets::unregistered, "export.webp.value"),
      m_webp_what(widgets::unregistered, "export.webp.what", "Quality"),
      m_webp_hint(widgets::unregistered, "export.webp.hint"),
      m_png_value(widgets::unregistered, "export.png.value"),
      m_webp_lossless("export.webp.lossless"),
      m_tiff_compression("export.tiff.compression"),
      m_ora_list("export.ora.list", Gtk::Orientation::VERTICAL, 6),
      m_ok_button("export.ok", "Export"),
      m_cancel_button("export.cancel", "Cancel") {
    set_name("export");
    registry::add("export", this);
    set_title("Export");
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

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 14);
    box->set_margin(20);
    m_heading.set_xalign(0);
    m_heading.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    m_heading.set_max_width_chars(40);
    m_heading.add_css_class("heading");
    box->append(m_heading);
    m_pages.set_transition_type(Gtk::StackTransitionType::NONE);
    m_pages.set_vhomogeneous(false);
    m_pages.set_hhomogeneous(true);
    box->append(m_pages);

    {   // JPEG
        auto* g = page();
        scale_row(*g, 0, "Quality", m_jpeg_quality, m_jpeg_value, 1, 100);
        g->attach(*dim("Higher is truer and bigger; 85 - 95 is a photo's sweet spot. No transparency in a JPEG -- "
                       "what shows through is white, as on paper.", true), 0, 1, 3, 1);
        m_pages.add(*g, "jpeg");
    }
    {   // WebP
        auto* g = page();
        g->attach(*dim("Lossless"), 0, 0);
        m_webp_lossless.set_halign(Gtk::Align::START);
        g->attach(m_webp_lossless, 1, 0, 2, 1);
        m_webp_what.add_css_class("dim-label");
        m_webp_what.set_xalign(1);
        g->attach(m_webp_what, 0, 1);
        scale_row(*g, 1, "", m_webp_quality, m_webp_value, 0, 100);   // "Quality" or "Effort": m_webp_what
        m_webp_hint.add_css_class("dim-label");
        m_webp_hint.add_css_class("caption");
        m_webp_hint.set_wrap(true);
        m_webp_hint.set_max_width_chars(46);
        m_webp_hint.set_xalign(0);
        g->attach(m_webp_hint, 0, 2, 3, 1);
        m_pages.add(*g, "webp");
    }
    {   // PNG
        auto* g = page();
        scale_row(*g, 0, "Compression", m_png_level, m_png_value, 0, 9);
        g->attach(*dim("Lossless at every level -- higher is smaller and slower to write. 6 is zlib's balance.", true),
                  0, 1, 3, 1);
        m_pages.add(*g, "png");
    }
    {   // TIFF
        auto* g = page();
        g->attach(*dim("Compression"), 0, 0);
        m_tiff_compression.set_model(Gtk::StringList::create({"Deflate (lossless, smaller)", "None"}));
        m_tiff_compression.set_hexpand(true);
        g->attach(m_tiff_compression, 1, 0);
        g->attach(*dim("TIFF keeps the document's depth and mode as they are.", true), 0, 1, 2, 1);
        m_pages.add(*g, "tiff");
    }
    {   // ORA
        auto* g = page();
        m_ora_list.set_hexpand(true);
        g->attach(m_ora_list, 0, 0);
        m_pages.add(*g, "ora");
    }
    set_child(*box);

    m_jpeg_quality.signal_value_changed().connect([this] { sync(); });
    m_webp_quality.signal_value_changed().connect([this] { sync(); });
    m_png_level.signal_value_changed().connect([this] { sync(); });
    m_webp_lossless.property_active().signal_changed().connect([this] { sync(); });

    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        const auto o = read();
        close();
        m_ok.emit(o);
    });
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Escape) { close(); return true; }
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { m_ok_button.activate(); return true; }
        return false;
    }, false);
    add_controller(keys);
}

ExportDialog::~ExportDialog() { registry::remove(this); }

void ExportDialog::sync() {
    m_jpeg_value.set_text(std::to_string(as_int(m_jpeg_quality)));
    m_webp_value.set_text(std::to_string(as_int(m_webp_quality)));
    m_png_value.set_text(std::to_string(as_int(m_png_level)));
    const bool lossless = m_webp_lossless.get_active();
    m_webp_what.set_text(lossless ? "Effort" : "Quality");
    m_webp_hint.set_text(lossless ? "Every pixel exactly as it is. Effort only trades write time for a smaller file."
                                  : "Lossy, like a JPEG but smaller, and transparency survives. 75 - 90 for photos.");
}

void ExportDialog::fill_ora(const core::ora::Plan& plan) {
    while (auto* c = m_ora_list.get_first_child()) m_ora_list.remove(*c);
    const auto section = [this](const std::string& title, const std::vector<std::string>& lines) {
        auto* t = Gtk::make_managed<Gtk::Label>(title);
        t->set_xalign(0);
        t->add_css_class("heading");
        t->set_margin_top(m_ora_list.get_first_child() ? 6 : 0);
        m_ora_list.append(*t);
        for (const auto& s : lines) {
            auto* l = Gtk::make_managed<Gtk::Label>("•  " + s);
            l->set_xalign(0);
            l->set_wrap(true);
            l->set_max_width_chars(46);
            m_ora_list.append(*l);
        }
    };
    if (plan.baked.empty() && plan.left_out.empty()) {
        section("Everything crosses", {"Layers, groups, names, visibility, opacity and blend modes, as they are."});
    } else {
        if (!plan.baked.empty()) section("Baked into the layer's pixels -- the look survives", plan.baked);
        if (!plan.left_out.empty()) section("Not in the file -- ORA can't hold it", plan.left_out);
    }
    m_ora_list.append(*dim("Layers for Krita, GIMP and MyPaint. Your .klimt keeps everything.", true));
}

void ExportDialog::ask(formats::Kind kind, const std::string& file_name, const formats::WriteOptions& o,
                       const core::ora::Plan& plan) {
    m_kind = kind;
    m_base = o;
    const char* page = "png";
    const char* what = "PNG";
    switch (kind) {
        case formats::Kind::JPEG: page = "jpeg"; what = "JPEG"; break;
        case formats::Kind::WebP: page = "webp"; what = "WebP"; break;
        case formats::Kind::TIFF: page = "tiff"; what = "TIFF"; break;
        case formats::Kind::ORA:  page = "ora";  what = "OpenRaster"; break;
        default: break;
    }
    set_title(std::string("Export ") + what);
    m_heading.set_text("“" + file_name + "”");
    m_jpeg_quality.set_value(o.jpeg_quality);
    m_webp_quality.set_value(o.webp_quality);
    m_webp_lossless.set_active(o.webp_lossless);
    m_png_level.set_value(o.png_level);
    m_tiff_compression.set_selected(o.tiff_deflate ? 0 : 1);
    if (kind == formats::Kind::ORA) fill_ora(plan);
    m_pages.set_visible_child(page);
    sync();
    present();
    m_ok_button.grab_focus();
}

formats::WriteOptions ExportDialog::read() const {
    formats::WriteOptions o = m_base;   // the other formats' settings ride through unchanged
    switch (m_kind) {
        case formats::Kind::JPEG: o.jpeg_quality = as_int(m_jpeg_quality); break;
        case formats::Kind::WebP:
            o.webp_quality = as_int(m_webp_quality);
            o.webp_lossless = m_webp_lossless.get_active();
            break;
        case formats::Kind::PNG:  o.png_level = as_int(m_png_level); break;
        case formats::Kind::TIFF: o.tiff_deflate = m_tiff_compression.get_selected() == 0; break;
        default: break;
    }
    return o;
}

}  // namespace klimt
