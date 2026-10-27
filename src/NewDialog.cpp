#include "NewDialog.hpp"
#include "Registry.hpp"

#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/stringlist.h>

#include <cmath>
#include <cstdio>

namespace klimt {

namespace {
struct Preset { const char* name; int w, h; double dpi; };
// The first is "Custom": choosing a preset fills the fields; editing a field
// goes back to Custom.
const Preset kPresets[] = {
    {"Custom", 0, 0, 0},
    {"Screen 1920 × 1080", 1920, 1080, 72},
    {"Screen 3840 × 2160 (4K)", 3840, 2160, 72},
    {"Square 2048 × 2048", 2048, 2048, 72},
    {"Photo 6000 × 4000 (24 MP)", 6000, 4000, 300},
    {"Letter 8.5 × 11 in, 300 dpi", 2550, 3300, 300},
    {"A4 210 × 297 mm, 300 dpi", 2480, 3508, 300},
    {"Tabloid 11 × 17 in, 600 dpi", 6600, 10200, 600},
};
const char* kModes[] = {"RGB", "Grayscale", "CMYK", "Lab"};
const pixel::Mode kModeOf[] = {pixel::Mode::RGB, pixel::Mode::Gray, pixel::Mode::CMYK, pixel::Mode::Lab};
const char* kDepths[] = {"8-bit", "16-bit", "32-bit float"};
const pixel::Depth kDepthOf[] = {pixel::Depth::U8, pixel::Depth::U16, pixel::Depth::F32};
const char* kBackgrounds[] = {"White", "Black", "Transparent"};

Glib::RefPtr<Gtk::StringList> list_of(std::initializer_list<const char*> items) {
    std::vector<Glib::ustring> v(items.begin(), items.end());
    return Gtk::StringList::create(v);
}

std::string bytes_text(double b) {
    char buf[64];
    if (b >= 1024.0 * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.2f GB", b / (1024.0 * 1024 * 1024));
    else std::snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024));
    return buf;
}
}  // namespace

NewDialog::NewDialog(Gtk::Window& parent)
    : m_preset("shell.new.preset"), m_width("shell.new.width"), m_height("shell.new.height"),
      m_dpi("shell.new.dpi"), m_mode("shell.new.mode"), m_depth("shell.new.depth"),
      m_background("shell.new.background"), m_cost("shell.new.cost"),
      m_create_button("shell.new.create", "Create"), m_cancel_button("shell.new.cancel", "Cancel"),
      m_swap("shell.new.swap") {
    set_name("shell.new");
    registry::add("shell.new", this);
    set_title("New Image");
    set_modal(true);
    set_resizable(false);
    set_transient_for(parent);
    set_hide_on_close(true);

    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(false);
    m_create_button.add_css_class("suggested-action");
    hb->pack_start(m_cancel_button);
    hb->pack_end(m_create_button);
    set_titlebar(*hb);

    {
        std::vector<Glib::ustring> names;
        for (const auto& p : kPresets) names.emplace_back(p.name);
        m_preset.set_model(Gtk::StringList::create(names));
    }
    m_mode.set_model(list_of({kModes[0], kModes[1], kModes[2], kModes[3]}));
    m_depth.set_model(list_of({kDepths[0], kDepths[1], kDepths[2]}));
    m_background.set_model(list_of({kBackgrounds[0], kBackgrounds[1], kBackgrounds[2]}));

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
    m_width.set_value(1920);
    m_height.set_value(1080);
    m_dpi.set_value(72);
    m_preset.set_selected(1);
    m_swap.set_icon_name("object-flip-horizontal-symbolic");
    m_swap.set_tooltip_text("Swap width and height");
    m_swap.set_has_frame(false);

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
    const auto unit = [&](const char* text, int col) {
        auto* l = Gtk::make_managed<Gtk::Label>(text);
        l->set_halign(Gtk::Align::START);
        l->add_css_class("dim-label");
        grid->attach(*l, col, row);
    };
    label("Preset");        grid->attach(m_preset, 1, row, 3); ++row;
    label("Width");         grid->attach(m_width, 1, row); unit("px", 2);
    grid->attach(m_swap, 3, row, 1, 2); ++row;
    label("Height");        grid->attach(m_height, 1, row); unit("px", 2); ++row;
    label("Resolution");    grid->attach(m_dpi, 1, row); unit("dpi", 2); ++row;
    label("Mode");          grid->attach(m_mode, 1, row, 3); ++row;
    label("Depth");         grid->attach(m_depth, 1, row, 3); ++row;
    label("Background");    grid->attach(m_background, 1, row, 3); ++row;
    m_cost.set_halign(Gtk::Align::START);
    m_cost.set_wrap(true);
    m_cost.set_max_width_chars(44);
    m_cost.add_css_class("dim-label");
    m_cost.set_margin_top(6);
    grid->attach(m_cost, 0, row, 4);
    set_child(*grid);

    m_preset.property_selected().signal_changed().connect(sigc::mem_fun(*this, &NewDialog::on_preset));
    const auto edited = [this] {
        if (!m_applying && m_preset.get_selected() != 0) {
            // Still the preset? Leave it; otherwise the fields are Custom now.
            const auto& p = kPresets[m_preset.get_selected()];
            if (static_cast<int>(m_width.get_value()) != p.w || static_cast<int>(m_height.get_value()) != p.h ||
                std::lround(m_dpi.get_value()) != std::lround(p.dpi)) {
                m_applying = true;
                m_preset.set_selected(0);
                m_applying = false;
            }
        }
        refresh();
    };
    m_width.signal_value_changed().connect(edited);
    m_height.signal_value_changed().connect(edited);
    m_dpi.signal_value_changed().connect(edited);
    for (auto* d : {&m_mode, &m_depth, &m_background})
        d->property_selected().signal_changed().connect([this] { refresh(); });
    m_swap.signal_clicked().connect([this] {
        const double w = m_width.get_value();
        m_width.set_value(m_height.get_value());
        m_height.set_value(w);
    });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_create_button.signal_clicked().connect([this] {
        const auto s = spec();
        if (!core::file::check(s).empty()) return;
        close();
        m_create.emit(s);
    });
    // Enter anywhere creates; Escape cancels.
    auto keys = Gtk::EventControllerKey::create();
    // s012: CAPTURE, as NumberDialog -- the size field took Enter first and
    // Create never heard it (seen under Xvfb).
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Escape) { close(); return true; }
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) {
            m_width.update();
            m_height.update();
            m_dpi.update();
            m_create_button.activate();
            return true;
        }
        return false;
    }, false);
    add_controller(keys);
    refresh();
}

NewDialog::~NewDialog() { registry::remove(this); }

void NewDialog::open() {
    refresh();
    present();
    m_width.grab_focus();
}

void NewDialog::on_preset() {
    const auto i = m_preset.get_selected();
    if (m_applying || i == 0 || i >= std::size(kPresets)) { refresh(); return; }
    m_applying = true;
    m_width.set_value(kPresets[i].w);
    m_height.set_value(kPresets[i].h);
    m_dpi.set_value(kPresets[i].dpi);
    m_applying = false;
    refresh();
}

core::file::NewSpec NewDialog::spec() const {
    core::file::NewSpec s;
    s.width = static_cast<int>(m_width.get_value());
    s.height = static_cast<int>(m_height.get_value());
    s.dpi = m_dpi.get_value();
    const auto mi = std::min<guint>(m_mode.get_selected(), 3), di = std::min<guint>(m_depth.get_selected(), 2),
               bi = std::min<guint>(m_background.get_selected(), 2);
    s.format = {kDepthOf[di], kModeOf[mi], true};
    s.background = bi == 0 ? core::file::Background::White : bi == 1 ? core::file::Background::Black
                                                                       : core::file::Background::Transparent;
    return s;
}

void NewDialog::refresh() {
    const auto s = spec();
    const auto bad = core::file::check(s);
    m_create_button.set_sensitive(bad.empty());
    if (!bad.empty()) { m_cost.set_text("Can't make that: " + bad + "."); return; }
    const double px = static_cast<double>(s.width) * s.height;
    const double layer = px * s.format.bytes_per_pixel();
    char inches[96];
    std::snprintf(inches, sizeof inches, "%.2f × %.2f in", s.width / s.dpi, s.height / s.dpi);
    m_cost.set_text(std::string(inches) + "  ·  " + std::to_string(static_cast<long long>(std::lround(px / 1e6 * 10)) / 10) +
                    "." + std::to_string(static_cast<long long>(std::lround(px / 1e6 * 10)) % 10) + " Mpx\n" +
                    "A painted layer up to " + bytes_text(layer) + "; the background costs almost nothing until painted.");
}

}  // namespace klimt
