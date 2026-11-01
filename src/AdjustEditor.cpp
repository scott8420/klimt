#include "AdjustEditor.hpp"

#include <gtkmm/button.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/grid.h>
#include <gtkmm/stringlist.h>
#include <glibmm/markup.h>

#include <algorithm>
#include <cmath>

namespace klimt {

namespace A = core::adjust;
using core::AdjustKind;

namespace {
constexpr double kPad = 8;
constexpr double kGap = 1.0 / 255;   // two curve points are at least a level apart

Gdk::RGBA fg_of(Gtk::Widget& w) {
    GdkRGBA c;
    gtk_widget_get_color(w.gobj(), &c);
    return Gdk::RGBA(&c, true);
}

// s062: Photoshop's Photo Filter presets (their published colours).
namespace s062 {
struct Preset { const char* name; int r, g, b; };
constexpr Preset kFilters[] = {
    {"Warming Filter (85)", 236, 138, 0},   {"Warming Filter (LBA)", 250, 150, 0}, {"Warming Filter (81)", 235, 177, 19},
    {"Cooling Filter (80)", 0, 109, 255},   {"Cooling Filter (LBB)", 0, 93, 255},  {"Cooling Filter (82)", 0, 181, 255},
    {"Red", 234, 26, 26},       {"Orange", 243, 132, 23},   {"Yellow", 249, 227, 28},    {"Green", 25, 201, 25},
    {"Cyan", 29, 203, 234},     {"Blue", 29, 53, 234},      {"Violet", 155, 29, 234},    {"Magenta", 227, 24, 227},
    {"Sepia", 172, 122, 51},    {"Deep Red", 255, 0, 0},    {"Deep Blue", 0, 34, 205},   {"Deep Emerald", 0, 140, 0},
    {"Deep Yellow", 255, 213, 0}, {"Underwater", 0, 194, 177},
};
constexpr guint kCustom = std::size(kFilters);
guint preset_of(const std::array<double, 3>& c) {
    for (guint i = 0; i < kCustom; ++i)
        if (std::lround(c[0] * 255) == kFilters[i].r && std::lround(c[1] * 255) == kFilters[i].g && std::lround(c[2] * 255) == kFilters[i].b) return i;
    return kCustom;
}
}  // namespace s062
using namespace s062;

// s063: Gradient Map presets.
namespace s063 {
struct GradPreset { const char* name; std::vector<core::GradStop> stops; };
const std::vector<GradPreset>& grad_presets() {
    const auto c = [](int r, int g, int b) { return std::array<double, 3>{r / 255.0, g / 255.0, b / 255.0}; };
    static const std::vector<GradPreset> v{
        {"Black, White", {{0, 0.5, c(0, 0, 0)}, {1, 0.5, c(255, 255, 255)}}},
        {"White, Black", {{0, 0.5, c(255, 255, 255)}, {1, 0.5, c(0, 0, 0)}}},
        {"Sepia", {{0, 0.5, c(0, 0, 0)}, {0.45, 0.5, c(112, 66, 20)}, {1, 0.5, c(255, 244, 222)}}},
        {"Blue Duotone", {{0, 0.5, c(0, 0, 0)}, {0.5, 0.5, c(30, 80, 160)}, {1, 0.5, c(255, 255, 255)}}},
        {"Violet, Orange", {{0, 0.5, c(41, 10, 89)}, {1, 0.5, c(255, 124, 0)}}},
        {"Blue, Red, Yellow", {{0, 0.5, c(10, 0, 178)}, {0.5, 0.5, c(255, 0, 0)}, {1, 0.5, c(255, 252, 0)}}},
    };
    return v;
}
guint grad_preset_of(const std::vector<core::GradStop>& g) {
    const auto& v = grad_presets();
    for (guint i = 0; i < v.size(); ++i) if (v[i].stops == g) return i;
    return static_cast<guint>(v.size());
}
constexpr double kBarTop = 4, kBarBottom = 30, kMidY = 36, kStopTop = 40, kStripH = 58, kGradMargin = 10;
}  // namespace s063
using namespace s063;

void tri_up(const Cairo::RefPtr<Cairo::Context>& cr, double x, double y, double s) {
    cr->move_to(x, y);
    cr->line_to(x - s, y + s * 1.6);
    cr->line_to(x + s, y + s * 1.6);
    cr->close_path();
}

Gtk::SpinButton* spin(double lo, double hi, double step, int digits) {
    auto s = Gtk::make_managed<Gtk::SpinButton>();
    s->set_range(lo, hi);
    s->set_increments(step, step * 10);
    s->set_digits(static_cast<guint>(digits));
    s->set_width_chars(digits ? 4 : 3);
    s->set_numeric(true);
    return s;
}

std::vector<core::CurvePt>& curve_of(core::Adjust& a, int ch) {
    auto& c = a.curves[static_cast<std::size_t>(ch)];
    if (c.size() < 2) c = {{0, 0}, {1, 1}};
    return c;
}
}  // namespace

AdjustEditor::AdjustEditor() : Gtk::Box(Gtk::Orientation::VERTICAL, 8) {
    set_name("adjust.editor");
    append(m_body);
}

void AdjustEditor::set(const core::Adjust& a) {
    const bool kind_changed = a.kind != m_built;
    m_a = a;
    if (kind_changed) { m_point = -1; rebuild(); }
    paint();
}

void AdjustEditor::set_histogram(const std::optional<core::adjust::Histogram>& h) {
    m_hist = h;
    if (m_auto) m_auto->set_sensitive(h.has_value());   // s064
    if (m_graph) m_graph->queue_draw();
}

int AdjustEditor::channel() const {
    return m_channel ? static_cast<int>(std::min<guint>(m_channel->get_selected(), 3)) : 0;
}

void AdjustEditor::begin_end_on(Gtk::Widget& w) {
    auto drag = Gtk::GestureDrag::create();
    drag->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    drag->signal_drag_begin().connect([this](double, double) { m_begin.emit(); });
    drag->signal_drag_end().connect([this](double, double) { m_end.emit(); });
    drag->signal_cancel().connect([this](const Gdk::EventSequence*) { m_end.emit(); });
    w.add_controller(drag);
}

Gtk::Scale* AdjustEditor::slider(const char* name, const char* label, double lo, double hi, int* field) {
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->set_xalign(0.0f);
    l->set_size_request(76, -1);
    l->add_css_class("dim-label");
    row->append(*l);
    auto s = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
    s->set_name(name);
    s->set_range(lo, hi);
    s->set_increments(1, 10);
    s->set_digits(0);
    s->set_round_digits(0);
    s->set_draw_value(true);
    s->set_value_pos(Gtk::PositionType::RIGHT);
    s->set_hexpand(true);
    if (lo < 0 && hi > 0) s->add_mark(0, Gtk::PositionType::BOTTOM, "");
    // s063: through m_sliders, so a page dropdown can rebind the field.
    const std::size_t idx = m_sliders.size();
    s->signal_value_changed().connect([this, s, idx] {
        if (m_painting || idx >= m_sliders.size()) return;
        *m_sliders[idx].field = static_cast<int>(std::lround(s->get_value()));
        emit();
    });
    begin_end_on(*s);
    row->append(*s);
    m_body.append(*row);
    m_sliders.push_back({s, field});
    return s;
}

Gtk::Scale* AdjustEditor::pair_slider(const char* name, const char* left, const char* right, double lo, double hi, int* field) {
    auto* s = slider(name, left, lo, hi, field);
    auto r = Gtk::make_managed<Gtk::Label>(right);
    r->set_xalign(0.0f);
    r->set_size_request(56, -1);
    r->add_css_class("dim-label");
    r->insert_after(*s->get_parent(), *s);   // into the row, after the scale
    s->get_prev_sibling()->set_size_request(56, -1);
    return s;
}

Gtk::DropDown* AdjustEditor::pager(const char* name, const char* label, const std::vector<Glib::ustring>& pages, guint start,
                                   std::function<int*(guint, std::size_t)> field_of) {
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->set_xalign(0.0f);
    l->set_size_request(76, -1);
    l->add_css_class("dim-label");
    row->append(*l);
    m_page = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(pages));
    m_page->set_name(name);
    m_page->set_hexpand(true);
    m_page->set_selected(start);
    m_page->property_selected().signal_changed().connect([this] {
        if (m_painting) return;
        repage();
        paint();
    });
    row->append(*m_page);
    m_body.append(*row);
    m_page_field = std::move(field_of);
    return m_page;
}

void AdjustEditor::repage() {
    if (!m_page || !m_page_field) return;
    const guint p = m_page->get_selected();
    for (std::size_t i = 0; i < m_paged.size(); ++i)
        if (m_paged[i] < m_sliders.size())
            if (int* f = m_page_field(p, i)) m_sliders[m_paged[i]].field = f;
}

Gtk::Scale* AdjustEditor::dslider(const char* name, const char* label, double lo, double hi, double step, int digits, double* field) {
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->set_xalign(0.0f);
    l->set_size_request(76, -1);
    l->add_css_class("dim-label");
    row->append(*l);
    auto s = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
    s->set_name(name);
    s->set_range(lo, hi);
    s->set_increments(step, step * 10);
    s->set_digits(digits);
    s->set_round_digits(digits);
    s->set_draw_value(true);
    s->set_value_pos(Gtk::PositionType::RIGHT);
    s->set_hexpand(true);
    if (lo < 0 && hi > 0) s->add_mark(0, Gtk::PositionType::BOTTOM, "");
    s->signal_value_changed().connect([this, s, field, digits] {
        if (m_painting) return;
        const double k = std::pow(10.0, digits);
        *field = std::round(s->get_value() * k) / k;
        emit();
    });
    begin_end_on(*s);
    row->append(*s);
    m_body.append(*row);
    m_sliders.push_back({s, nullptr, field});
    return s;
}

void AdjustEditor::rebuild() {
    while (auto* c = m_body.get_first_child()) m_body.remove(*c);
    m_sliders.clear();
    m_colorize = nullptr;
    m_tint = nullptr;
    m_tint_h = m_tint_s = nullptr;
    m_preset = nullptr;
    m_filter_colour = nullptr;
    m_luma = nullptr;
    m_channel = nullptr;
    m_page = nullptr;            // s063
    m_paged.clear();
    m_page_field = nullptr;
    m_mono = nullptr;
    m_total = nullptr;
    m_relative = m_absolute = nullptr;
    m_huesat_ranges = false;   // s064
    m_auto = nullptr;
    m_drop_btn = {};
    if (m_dropper != A::Dropper::None) { m_dropper = A::Dropper::None; m_dropper_sig.emit(m_dropper); }
    m_band_handle = -1;
    m_grad_preset = nullptr;
    m_stop_colour = nullptr;
    m_stop_at = nullptr;
    m_stop_del = nullptr;
    m_reverse = m_dither = nullptr;
    m_stop = 0;
    m_grad_drag = 0;
    m_stop_off = false;
    m_graph = nullptr;
    for (auto& p : m_lv) p = nullptr;
    m_cin = m_cout = nullptr;
    m_built = m_a.kind;
    if (m_a.kind == AdjustKind::None) return;

    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto t = Gtk::make_managed<Gtk::Label>();
    t->set_markup("<b>" + Glib::Markup::escape_text(std::string(A::label(m_a.kind))) + "</b>");
    t->set_xalign(0.0f);
    t->set_hexpand(true);
    head->append(*t);
    if (m_a.kind == AdjustKind::Levels || m_a.kind == AdjustKind::Curves) {
        m_channel = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"RGB", "Red", "Green", "Blue"}));
        m_channel->set_name("adjust.channel");
        m_channel->set_tooltip_text("Which channel the controls change (RGB: all three)");
        m_channel->property_selected().signal_changed().connect([this] {
            if (m_painting) return;
            m_point = -1;
            paint();
        });
        head->append(*m_channel);
        // s064: Auto -- each channel stretched to black and white.
        m_auto = Gtk::make_managed<Gtk::Button>("Auto");
        m_auto->set_name("adjust.auto");
        m_auto->add_css_class("flat");
        m_auto->set_tooltip_text("Auto -- stretch each of red, green and blue so its darkest is black and its lightest white "
                                 "(the extreme 0.1 % let go), as Photoshop's");
        m_auto->signal_clicked().connect([this] {
            if (!m_hist) return;
            m_a = A::auto_tone(m_a, *m_hist);
            m_point = -1;
            emit();
            paint();
        });
        head->append(*m_auto);
    }
    auto reset = Gtk::make_managed<Gtk::Button>();
    reset->set_icon_name("edit-undo-symbolic");
    reset->set_tooltip_text("Reset -- back to changing nothing");
    reset->add_css_class("flat");
    reset->set_name("adjust.reset");
    reset->signal_clicked().connect(sigc::mem_fun(*this, &AdjustEditor::reset));
    head->append(*reset);
    m_body.append(*head);

    switch (m_a.kind) {
        case AdjustKind::BrightCon:
            slider("adjust.brightness", "Brightness", -150, 150, &m_a.brightness);
            slider("adjust.contrast", "Contrast", -50, 100, &m_a.contrast);
            break;
        // s061 (WEAKNESSES 34).
        case AdjustKind::Exposure: {
            dslider("adjust.exposure", "Exposure", -20, 20, 0.01, 2, &m_a.exposure)->set_tooltip_text("Stops of light: +1 doubles it (worked in linear light)");
            dslider("adjust.offset", "Offset", -0.5, 0.5, 0.0001, 4, &m_a.offset)->set_tooltip_text("Added to every pixel's light: lifts or sinks the shadows most");
            dslider("adjust.exp-gamma", "Gamma", 0.01, 9.99, 0.01, 2, &m_a.exp_gamma)->set_tooltip_text("Bends the middle: over 1 lightens, under 1 darkens");
            break;
        }
        case AdjustKind::Posterize:
            slider("adjust.posterize", "Levels", 2, 255, &m_a.posterize)->set_tooltip_text("Tones per colour channel");
            break;
        case AdjustKind::Threshold: {
            // s062: the histogram (luminosity) behind the level, as Photoshop's:
            // left of the line turns black, right of it white. Click or drag.
            m_graph = Gtk::make_managed<Gtk::DrawingArea>();
            m_graph->set_name("adjust.threshold-graph");
            m_graph->set_content_height(120);
            m_graph->set_hexpand(true);
            m_graph->set_tooltip_text("Click or drag: lighter than the line turns white, darker black");
            m_graph->set_draw_func(sigc::mem_fun(*this, &AdjustEditor::draw_threshold));
            auto drag = Gtk::GestureDrag::create();
            drag->signal_drag_begin().connect([this](double x, double y) { m_begin.emit(); m_press_x = x; m_press_y = y; threshold_at(x); });
            drag->signal_drag_update().connect([this](double dx, double) { threshold_at(m_press_x + dx); });
            drag->signal_drag_end().connect([this](double, double) { m_end.emit(); });
            m_graph->add_controller(drag);
            m_body.append(*m_graph);
            slider("adjust.threshold", "Level", 1, 255, &m_a.threshold)->set_tooltip_text("Lighter than this turns white, darker black");
            break;
        }
        // s062.
        case AdjustKind::Vibrance:
            slider("adjust.vibrance", "Vibrance", -100, 100, &m_a.vibrance)->set_tooltip_text("More colour where there is little; strong colours left be");
            slider("adjust.vib-saturation", "Saturation", -100, 100, &m_a.saturation)->set_tooltip_text("More (or less) colour everywhere, evenly");
            break;
        case AdjustKind::BlackWhite: {
            const char* names[6] = {"Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
            const char* ids[6] = {"adjust.bw-reds", "adjust.bw-yellows", "adjust.bw-greens", "adjust.bw-cyans", "adjust.bw-blues", "adjust.bw-magentas"};
            for (int i = 0; i < 6; ++i)
                slider(ids[i], names[i], -200, 300, &m_a.bw[static_cast<std::size_t>(i)])->set_tooltip_text("How light this hue turns, percent");
            m_tint = Gtk::make_managed<Gtk::CheckButton>("Tint");
            m_tint->set_name("adjust.bw-tint");
            m_tint->set_tooltip_text("Colour the grey -- the hue and saturation below");
            m_tint->signal_toggled().connect([this] {
                if (m_painting) return;
                m_a.bw_tint = m_tint->get_active();
                emit();
                paint();
            });
            m_body.append(*m_tint);
            m_tint_h = slider("adjust.bw-tint-hue", "Hue", 0, 359, &m_a.tint_hue);
            m_tint_s = slider("adjust.bw-tint-sat", "Saturation", 0, 100, &m_a.tint_sat);
            break;
        }
        case AdjustKind::PhotoFilter: {
            auto names = Gtk::StringList::create({});
            for (const auto& p : kFilters) names->append(p.name);
            names->append("Custom");
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            m_preset = Gtk::make_managed<Gtk::DropDown>(names);
            m_preset->set_name("adjust.filter-preset");
            m_preset->set_hexpand(true);
            m_preset->set_tooltip_text("Photoshop's filters -- or Custom, the colour beside it");
            m_preset->property_selected().signal_changed().connect([this] {
                if (m_painting) return;
                const guint i = m_preset->get_selected();
                if (i >= kCustom) return;
                m_a.filter = {kFilters[i].r / 255.0, kFilters[i].g / 255.0, kFilters[i].b / 255.0};
                emit();
                paint();
            });
            row->append(*m_preset);
            m_filter_colour = Gtk::make_managed<Gtk::ColorDialogButton>(Gtk::ColorDialog::create());
            m_filter_colour->set_name("adjust.filter-colour");
            m_filter_colour->set_tooltip_text("The filter's colour");
            m_filter_colour->property_rgba().signal_changed().connect([this] {
                if (m_painting) return;
                const auto c = m_filter_colour->get_rgba();
                m_a.filter = {c.get_red(), c.get_green(), c.get_blue()};
                emit();
                paint();
            });
            row->append(*m_filter_colour);
            m_body.append(*row);
            slider("adjust.density", "Density", 1, 100, &m_a.density)->set_tooltip_text("How strongly the filter colours, percent");
            m_luma = Gtk::make_managed<Gtk::CheckButton>("Preserve Luminosity");
            m_luma->set_name("adjust.filter-luma");
            m_luma->set_tooltip_text("Keep each pixel as light as it was -- only the colour changes");
            m_luma->signal_toggled().connect([this] {
                if (m_painting) return;
                m_a.keep_luma = m_luma->get_active();
                emit();
            });
            m_body.append(*m_luma);
            break;
        }
        // s063.
        case AdjustKind::ColourBalance: {
            pager("adjust.balance-tone", "Tone", {"Shadows", "Midtones", "Highlights"}, 1,
                  [this](guint p, std::size_t i) { return &m_a.balance[std::min<guint>(p, 2)][std::min<std::size_t>(i, 2)]; });
            const char* ends[3][2] = {{"Cyan", "Red"}, {"Magenta", "Green"}, {"Yellow", "Blue"}};
            const char* ids[3] = {"adjust.balance-cyan-red", "adjust.balance-magenta-green", "adjust.balance-yellow-blue"};
            for (std::size_t i = 0; i < 3; ++i) {
                m_paged.push_back(m_sliders.size());
                pair_slider(ids[i], ends[i][0], ends[i][1], -100, 100, &m_a.balance[1][i])
                    ->set_tooltip_text("Shift this tone's colour toward one end or the other");
            }
            repage();
            m_luma = Gtk::make_managed<Gtk::CheckButton>("Preserve Luminosity");
            m_luma->set_name("adjust.balance-luma");
            m_luma->set_tooltip_text("Keep each pixel as light as it was -- only the colour changes");
            m_luma->signal_toggled().connect([this] {
                if (m_painting) return;
                m_a.keep_luma = m_luma->get_active();
                emit();
            });
            m_body.append(*m_luma);
            break;
        }
        case AdjustKind::ChannelMixer: {
            pager("adjust.mixer-output", "Output", {"Red", "Green", "Blue"}, 0, [this](guint p, std::size_t i) {
                return m_a.monochrome ? &m_a.mono[std::min<std::size_t>(i, 3)] : &m_a.mixer[std::min<guint>(p, 2)][std::min<std::size_t>(i, 3)];
            });
            m_page->set_tooltip_text("Which channel the sliders make (Monochrome: the one grey)");
            const char* names[3] = {"Red", "Green", "Blue"};
            const char* ids[3] = {"adjust.mixer-red", "adjust.mixer-green", "adjust.mixer-blue"};
            for (std::size_t i = 0; i < 3; ++i) {
                m_paged.push_back(m_sliders.size());
                slider(ids[i], names[i], -200, 200, &m_a.mixer[0][i])->set_tooltip_text("How much of this channel goes into the output, percent");
            }
            m_total = Gtk::make_managed<Gtk::Label>();
            m_total->set_name("adjust.mixer-total");
            m_total->set_xalign(1.0f);
            m_total->add_css_class("dim-label");
            m_total->set_tooltip_text("What the three add up to: over 100 % the output can burn out to white");
            m_body.append(*m_total);
            m_paged.push_back(m_sliders.size());
            slider("adjust.mixer-constant", "Constant", -200, 200, &m_a.mixer[0][3])->set_tooltip_text("Added to the output: 100 is white, -100 black");
            repage();
            m_mono = Gtk::make_managed<Gtk::CheckButton>("Monochrome");
            m_mono->set_name("adjust.mixer-mono");
            m_mono->set_tooltip_text("One grey made from the three channels");
            m_mono->signal_toggled().connect([this] {
                if (m_painting) return;
                m_a.monochrome = m_mono->get_active();
                repage();
                emit();
                paint();
            });
            m_body.append(*m_mono);
            break;
        }
        case AdjustKind::SelectiveColour: {
            pager("adjust.selective-colours", "Colours", {"Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas", "Whites", "Neutrals", "Blacks"}, 0,
                  [this](guint p, std::size_t i) { return &m_a.selective[std::min<guint>(p, 8)][std::min<std::size_t>(i, 3)]; });
            m_page->set_tooltip_text("Which colours the inks below change");
            const char* names[4] = {"Cyan", "Magenta", "Yellow", "Black"};
            const char* ids[4] = {"adjust.selective-cyan", "adjust.selective-magenta", "adjust.selective-yellow", "adjust.selective-black"};
            for (std::size_t i = 0; i < 4; ++i) {
                m_paged.push_back(m_sliders.size());
                slider(ids[i], names[i], -100, 100, &m_a.selective[0][i])->set_tooltip_text("Ink added to (or taken from) these colours, percent");
            }
            repage();
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            m_relative = Gtk::make_managed<Gtk::CheckButton>("Relative");
            m_relative->set_name("adjust.selective-relative");
            m_relative->set_tooltip_text("Change the ink by a share of what is there (white stays white)");
            m_absolute = Gtk::make_managed<Gtk::CheckButton>("Absolute");
            m_absolute->set_name("adjust.selective-absolute");
            m_absolute->set_tooltip_text("Change the ink by the amount itself");
            m_absolute->set_group(*m_relative);
            m_absolute->signal_toggled().connect([this] {
                if (m_painting) return;
                m_a.selective_absolute = m_absolute->get_active();
                emit();
            });
            row->append(*m_relative);
            row->append(*m_absolute);
            m_body.append(*row);
            break;
        }
        case AdjustKind::GradientMap: {
            auto names = Gtk::StringList::create({});
            for (const auto& p : grad_presets()) names->append(p.name);
            names->append("Custom");
            auto prow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            auto pl = Gtk::make_managed<Gtk::Label>("Gradient");
            pl->set_xalign(0.0f);
            pl->set_size_request(76, -1);
            pl->add_css_class("dim-label");
            prow->append(*pl);
            m_grad_preset = Gtk::make_managed<Gtk::DropDown>(names);
            m_grad_preset->set_name("adjust.gradient-preset");
            m_grad_preset->set_hexpand(true);
            m_grad_preset->set_tooltip_text("A ready-made gradient -- or Custom, the stops below");
            m_grad_preset->property_selected().signal_changed().connect([this] {
                if (m_painting) return;
                const guint i = m_grad_preset->get_selected();
                if (i >= grad_presets().size()) return;
                m_a.grad = grad_presets()[i].stops;
                m_stop = 0;
                emit();
                paint();
            });
            prow->append(*m_grad_preset);
            m_body.append(*prow);

            m_graph = Gtk::make_managed<Gtk::DrawingArea>();
            m_graph->set_name("adjust.gradient");
            m_graph->set_content_height(static_cast<int>(kStripH));
            m_graph->set_hexpand(true);
            m_graph->set_tooltip_text("Click under the bar to add a stop; drag a stop to move it, off the strip to remove it; "
                                      "drag a diamond to move where two colours meet");
            m_graph->set_draw_func(sigc::mem_fun(*this, &AdjustEditor::draw_gradient));
            auto drag = Gtk::GestureDrag::create();
            drag->signal_drag_begin().connect([this](double x, double y) { m_begin.emit(); m_press_x = x; m_press_y = y; gradient_press(x, y); });
            drag->signal_drag_update().connect([this](double dx, double dy) { gradient_drag(m_press_x + dx, m_press_y + dy); });
            drag->signal_drag_end().connect([this](double, double) { gradient_release(); m_end.emit(); });
            m_graph->add_controller(drag);
            m_body.append(*m_graph);

            auto srow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            auto sl = Gtk::make_managed<Gtk::Label>("Stop");
            sl->set_xalign(0.0f);
            sl->set_size_request(76, -1);
            sl->add_css_class("dim-label");
            srow->append(*sl);
            m_stop_colour = Gtk::make_managed<Gtk::ColorDialogButton>(Gtk::ColorDialog::create());
            m_stop_colour->set_name("adjust.gradient-stop-colour");
            m_stop_colour->set_tooltip_text("The picked stop's colour");
            m_stop_colour->property_rgba().signal_changed().connect([this] {
                if (m_painting || m_stop < 0 || m_stop >= static_cast<int>(m_a.grad.size())) return;
                const auto c = m_stop_colour->get_rgba();
                m_a.grad[static_cast<std::size_t>(m_stop)].rgb = {c.get_red(), c.get_green(), c.get_blue()};
                emit();
                paint();
            });
            srow->append(*m_stop_colour);
            auto ll = Gtk::make_managed<Gtk::Label>("Location");
            ll->add_css_class("dim-label");
            srow->append(*ll);
            m_stop_at = spin(0, 100, 1, 0);
            m_stop_at->set_name("adjust.gradient-stop-at");
            m_stop_at->set_tooltip_text("Where the picked stop sits, percent along");
            m_stop_at->signal_value_changed().connect([this] {
                if (m_painting || m_stop < 0 || m_stop >= static_cast<int>(m_a.grad.size())) return;
                const auto i = static_cast<std::size_t>(m_stop);
                const double lo = i > 0 ? m_a.grad[i - 1].at : 0, hi = i + 1 < m_a.grad.size() ? m_a.grad[i + 1].at : 1;
                m_a.grad[i].at = std::clamp(m_stop_at->get_value() / 100, lo, hi);
                emit();
                paint();
            });
            srow->append(*m_stop_at);
            auto pct = Gtk::make_managed<Gtk::Label>("%");
            pct->add_css_class("dim-label");
            srow->append(*pct);
            m_stop_del = Gtk::make_managed<Gtk::Button>();
            m_stop_del->set_icon_name("user-trash-symbolic");
            m_stop_del->add_css_class("flat");
            m_stop_del->set_name("adjust.gradient-stop-delete");
            m_stop_del->set_tooltip_text("Remove the picked stop (two at least stay)");
            m_stop_del->signal_clicked().connect([this] {
                if (m_a.grad.size() <= 2 || m_stop < 0 || m_stop >= static_cast<int>(m_a.grad.size())) return;
                m_a.grad.erase(m_a.grad.begin() + m_stop);
                m_stop = std::max(0, m_stop - 1);
                emit();
                paint();
            });
            srow->append(*m_stop_del);
            m_body.append(*srow);

            auto crow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
            m_reverse = Gtk::make_managed<Gtk::CheckButton>("Reverse");
            m_reverse->set_name("adjust.gradient-reverse");
            m_reverse->set_tooltip_text("Darks take the right end's colours, lights the left's");
            m_reverse->signal_toggled().connect([this] {
                if (m_painting) return;
                m_a.grad_reverse = m_reverse->get_active();
                emit();
            });
            m_dither = Gtk::make_managed<Gtk::CheckButton>("Dither");
            m_dither->set_name("adjust.gradient-dither");
            m_dither->set_tooltip_text("Kept with the layer (and in PSD) -- not drawn yet");
            m_dither->signal_toggled().connect([this] {
                if (m_painting) return;
                m_a.grad_dither = m_dither->get_active();
                emit();
            });
            crow->append(*m_reverse);
            crow->append(*m_dither);
            m_body.append(*crow);
            break;
        }
        case AdjustKind::Invert: {
            auto l = Gtk::make_managed<Gtk::Label>("No settings -- every colour turns to its opposite.");
            l->set_xalign(0.0f);
            l->set_wrap(true);
            l->add_css_class("dim-label");
            m_body.append(*l);
            break;
        }
        case AdjustKind::HueSat: {
            // s064: Master or one of the six colour ranges.
            m_huesat_ranges = true;
            pager("adjust.hue-range", "Edit", {"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"}, 0,
                  [this](guint p, std::size_t i) -> int* {
                      if (p == 0 || m_a.colorize) return i == 0 ? &m_a.hue : i == 1 ? &m_a.saturation : &m_a.lightness;
                      auto& rg = m_a.ranges[std::min<guint>(p, 6) - 1];
                      return i == 0 ? &rg.hue : i == 1 ? &rg.saturation : &rg.lightness;
                  });
            m_page->set_tooltip_text("Master changes every colour; a range only its own hues (the bar below shows which)");
            m_paged.push_back(m_sliders.size());
            slider("adjust.hue", "Hue", -180, 180, &m_a.hue);
            m_paged.push_back(m_sliders.size());
            slider("adjust.saturation", "Saturation", -100, 100, &m_a.saturation);
            m_paged.push_back(m_sliders.size());
            slider("adjust.lightness", "Lightness", -100, 100, &m_a.lightness);
            m_graph = Gtk::make_managed<Gtk::DrawingArea>();
            m_graph->set_name("adjust.hue-bar");
            m_graph->set_content_height(52);
            m_graph->set_hexpand(true);
            m_graph->set_tooltip_text("Above: the hues as they are; below: as they turn out. "
                                      "For a range, drag its handles: the bars set where it is at full strength, the triangles where it fades out");
            m_graph->set_draw_func(sigc::mem_fun(*this, &AdjustEditor::draw_huebar));
            auto drag = Gtk::GestureDrag::create();
            drag->signal_drag_begin().connect([this](double x, double y) { m_begin.emit(); m_press_x = x; huebar_press(x, y); });
            drag->signal_drag_update().connect([this](double dx, double) { huebar_drag(m_press_x + dx); });
            drag->signal_drag_end().connect([this](double, double) { m_band_handle = -1; m_end.emit(); });
            m_graph->add_controller(drag);
            m_body.append(*m_graph);
            m_colorize = Gtk::make_managed<Gtk::CheckButton>("Colorize");
            m_colorize->set_name("adjust.colorize");
            m_colorize->set_tooltip_text("One hue and saturation for everything; the lightness stays");
            m_colorize->signal_toggled().connect([this] {
                if (m_painting) return;
                const bool on = m_colorize->get_active();
                m_a.colorize = on;
                if (on) { m_a.hue = (m_a.hue % 360 + 360) % 360; m_a.saturation = m_a.saturation > 0 ? m_a.saturation : 25; }
                else    { m_a.hue = m_a.hue > 180 ? m_a.hue - 360 : m_a.hue; m_a.saturation = 0; }
                emit();
                paint();
            });
            m_body.append(*m_colorize);
            break;
        }
        case AdjustKind::Levels: {
            m_graph = Gtk::make_managed<Gtk::DrawingArea>();
            m_graph->set_name("adjust.levels");
            m_graph->set_content_height(170);
            m_graph->set_hexpand(true);
            m_graph->set_tooltip_text("Drag the sliders: input black, grey (gamma) and white above; output below");
            m_graph->set_draw_func(sigc::mem_fun(*this, &AdjustEditor::draw_levels));
            auto drag = Gtk::GestureDrag::create();
            drag->signal_drag_begin().connect([this](double x, double y) { m_begin.emit(); levels_press(x, y); });
            drag->signal_drag_update().connect([this](double dx, double dy) { levels_drag(m_press_x + dx, m_press_y + dy); });
            drag->signal_drag_end().connect([this](double, double) { m_lv_handle = -1; m_end.emit(); });
            m_graph->add_controller(drag);
            m_body.append(*m_graph);
            auto grid = Gtk::make_managed<Gtk::Grid>();
            grid->set_column_spacing(6);
            grid->set_row_spacing(4);
            const char* names[5] = {"adjust.in-black", "adjust.gamma", "adjust.in-white", "adjust.out-black", "adjust.out-white"};
            for (int i = 0; i < 5; ++i) {
                m_lv[i] = i == 1 ? spin(0.10, 9.99, 0.01, 2) : spin(0, 255, 1, 0);
                m_lv[i]->set_name(names[i]);
                m_lv[i]->signal_value_changed().connect([this, i] {
                    if (m_painting) return;
                    auto& c = m_a.levels[static_cast<std::size_t>(channel())];
                    const double v = m_lv[i]->get_value();
                    switch (i) {
                        case 0: c.in_black = std::min(v / 255, c.in_white - 2.0 / 255); break;
                        case 1: c.gamma = v; break;
                        case 2: c.in_white = std::max(v / 255, c.in_black + 2.0 / 255); break;
                        case 3: c.out_black = v / 255; break;
                        case 4: c.out_white = v / 255; break;
                    }
                    emit();
                    paint();
                });
            }
            const auto lab = [&](const char* s, int col, int row) {
                auto l = Gtk::make_managed<Gtk::Label>(s);
                l->add_css_class("dim-label");
                l->set_xalign(0.0f);
                grid->attach(*l, col, row);
            };
            lab("Input", 0, 0);
            grid->attach(*m_lv[0], 1, 0);
            grid->attach(*m_lv[1], 2, 0);
            grid->attach(*m_lv[2], 3, 0);
            lab("Output", 0, 1);
            grid->attach(*m_lv[3], 1, 1);
            grid->attach(*m_lv[4], 3, 1);
            m_body.append(*grid);
            dropper_row();   // s064
            break;
        }
        case AdjustKind::Curves: {
            m_graph = Gtk::make_managed<Gtk::DrawingArea>();
            m_graph->set_name("adjust.curves");
            m_graph->set_content_height(250);
            m_graph->set_hexpand(true);
            m_graph->set_tooltip_text("Click to add a point and drag it; drag a point off the graph to remove it");
            m_graph->set_draw_func(sigc::mem_fun(*this, &AdjustEditor::draw_curves));
            auto drag = Gtk::GestureDrag::create();
            drag->signal_drag_begin().connect([this](double x, double y) { m_begin.emit(); curves_press(x, y); });
            drag->signal_drag_update().connect([this](double dx, double dy) { curves_drag(m_press_x + dx, m_press_y + dy); });
            drag->signal_drag_end().connect([this](double, double) { curves_release(); m_end.emit(); });
            m_graph->add_controller(drag);
            m_body.append(*m_graph);
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            auto li = Gtk::make_managed<Gtk::Label>("Input");
            li->add_css_class("dim-label");
            m_cin = spin(0, 255, 1, 0);
            m_cin->set_name("adjust.curve-in");
            auto lo = Gtk::make_managed<Gtk::Label>("Output");
            lo->add_css_class("dim-label");
            m_cout = spin(0, 255, 1, 0);
            m_cout->set_name("adjust.curve-out");
            const auto edit = [this] {
                if (m_painting || m_point < 0) return;
                auto& pts = curve_of(m_a, channel());
                if (m_point >= static_cast<int>(pts.size())) return;
                const auto i = static_cast<std::size_t>(m_point);
                double x = m_cin->get_value() / 255;
                const double lo = i > 0 ? pts[i - 1].x + kGap : 0, hi = i + 1 < pts.size() ? pts[i + 1].x - kGap : 1;
                pts[i] = {std::clamp(x, lo, hi), m_cout->get_value() / 255};
                emit();
                paint();
            };
            m_cin->signal_value_changed().connect(edit);
            m_cout->signal_value_changed().connect(edit);
            row->append(*li);
            row->append(*m_cin);
            row->append(*lo);
            row->append(*m_cout);
            m_body.append(*row);
            dropper_row();   // s064
            break;
        }
        case AdjustKind::None: break;
    }
}

void AdjustEditor::paint() {
    m_painting = true;
    if (m_mono && m_page) {   // s063: Monochrome -- the one output is the grey
        const bool showing_grey = m_page->get_model()->get_n_items() == 1;
        if (m_a.monochrome != showing_grey) {
            if (m_a.monochrome) {
                m_mixer_page = m_page->get_selected();
                m_page->set_model(Gtk::StringList::create({"Grey"}));
                m_page->set_selected(0);
            } else {
                m_page->set_model(Gtk::StringList::create({"Red", "Green", "Blue"}));
                m_page->set_selected(std::min<guint>(m_mixer_page, 2));
            }
        }
        m_page->set_sensitive(!m_a.monochrome);
    }
    if (m_auto) m_auto->set_sensitive(m_hist.has_value());   // s064
    if (m_huesat_ranges && m_page) {   // s064: Colorize has no ranges -- Master only
        if (m_a.colorize) m_page->set_selected(0);
        m_page->set_sensitive(!m_a.colorize);
    }
    m_painting = false;
    repage();   // s063: an undo can switch Monochrome under the sliders
    m_painting = true;
    for (const auto& b : m_sliders) {
        if (b.field == &m_a.hue) b.s->set_range(m_a.colorize ? 0 : -180, m_a.colorize ? 360 : 180);
        if (b.field == &m_a.saturation) b.s->set_range(m_a.colorize ? 0 : -100, 100);
        b.s->set_value(b.dfield ? *b.dfield : *b.field);
    }
    if (m_colorize) m_colorize->set_active(m_a.colorize);
    if (m_tint) m_tint->set_active(m_a.bw_tint);   // s062
    if (m_tint_h) m_tint_h->get_parent()->set_sensitive(m_a.bw_tint);
    if (m_tint_s) m_tint_s->get_parent()->set_sensitive(m_a.bw_tint);
    if (m_preset) m_preset->set_selected(preset_of(m_a.filter));
    if (m_filter_colour) {
        Gdk::RGBA c;
        c.set_rgba(m_a.filter[0], m_a.filter[1], m_a.filter[2], 1.0);
        m_filter_colour->set_rgba(c);
    }
    if (m_luma) m_luma->set_active(m_a.keep_luma);
    if (m_mono) m_mono->set_active(m_a.monochrome);   // s063
    totals();
    if (m_absolute) (m_a.selective_absolute ? m_absolute : m_relative)->set_active(true);   // s063
    if (m_grad_preset) {
        m_stop = std::clamp(m_stop, 0, static_cast<int>(m_a.grad.size()) - 1);
        m_grad_preset->set_selected(grad_preset_of(m_a.grad));
        m_reverse->set_active(m_a.grad_reverse);
        m_dither->set_active(m_a.grad_dither);
        if (!m_a.grad.empty()) {
            const auto& s = m_a.grad[static_cast<std::size_t>(m_stop)];
            Gdk::RGBA c;
            c.set_rgba(s.rgb[0], s.rgb[1], s.rgb[2], 1.0);
            m_stop_colour->set_rgba(c);
            m_stop_at->set_value(std::round(s.at * 100));
        }
        m_stop_del->set_sensitive(m_a.grad.size() > 2);
    }
    if (m_lv[0]) {
        const auto& c = m_a.levels[static_cast<std::size_t>(channel())];
        m_lv[0]->set_value(std::round(c.in_black * 255));
        m_lv[1]->set_value(c.gamma);
        m_lv[2]->set_value(std::round(c.in_white * 255));
        m_lv[3]->set_value(std::round(c.out_black * 255));
        m_lv[4]->set_value(std::round(c.out_white * 255));
    }
    if (m_cin) {
        const auto& pts = m_a.curves[static_cast<std::size_t>(channel())];
        const bool have = m_point >= 0 && m_point < static_cast<int>(pts.size());
        m_cin->set_sensitive(have);
        m_cout->set_sensitive(have);
        if (have) {
            m_cin->set_value(std::round(pts[static_cast<std::size_t>(m_point)].x * 255));
            m_cout->set_value(std::round(pts[static_cast<std::size_t>(m_point)].y * 255));
        }
    }
    if (m_graph) m_graph->queue_draw();
    m_painting = false;
}

void AdjustEditor::emit() {
    core::Adjust out = m_a;
    if (m_stop_off && m_a.grad.size() > 2 && m_stop >= 0 && m_stop < static_cast<int>(out.grad.size()))   // s063: likewise a gradient stop
        out.grad.erase(out.grad.begin() + m_stop);
    if (m_point_off && m_point >= 0) {   // a point dragged off the graph: gone (for now)
        auto& pts = out.curves[static_cast<std::size_t>(channel())];
        if (m_point < static_cast<int>(pts.size())) pts.erase(pts.begin() + m_point);
    }
    if (m_graph) m_graph->queue_draw();
    totals();
    m_changed.emit(A::normalised(out));
}

// ── s064: the eyedroppers ──────────────────────────────────────────────────

void AdjustEditor::dropper_row() {
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    auto l = Gtk::make_managed<Gtk::Label>("Pick");
    l->set_xalign(0.0f);
    l->set_size_request(76, -1);
    l->add_css_class("dim-label");
    row->append(*l);
    const char* ids[3] = {"adjust.dropper-black", "adjust.dropper-grey", "adjust.dropper-white"};
    const char* tips[3] = {"Black point -- click the canvas where it should be black (each channel's own)",
                           "Grey point -- click something that should be neutral grey",
                           "White point -- click the canvas where it should be white"};
    for (int i = 0; i < 3; ++i) {
        auto b = Gtk::make_managed<Gtk::ToggleButton>();
        b->set_name(ids[i]);
        b->set_tooltip_text(tips[i]);
        b->add_css_class("flat");
        auto glyph = Gtk::make_managed<Gtk::DrawingArea>();
        glyph->set_content_width(16);
        glyph->set_content_height(16);
        const double shade = i == 0 ? 0.0 : i == 1 ? 0.5 : 1.0;
        glyph->set_draw_func([glyph, shade](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
            const auto fg = fg_of(*glyph);
            // A dropper: the bulb up right, the tip down left, filled with its point's shade.
            cr->set_line_width(1.2);
            cr->move_to(3, h - 3);
            cr->line_to(w - 7.5, 7.5);
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.9);
            cr->stroke();
            cr->arc(w - 5.5, 5.5, 4, 0, 2 * M_PI);
            cr->set_source_rgb(shade, shade, shade);
            cr->fill_preserve();
            cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.9);
            cr->stroke();
        });
        b->set_child(*glyph);
        const auto which = static_cast<A::Dropper>(i + 1);
        b->signal_toggled().connect([this, b, which] {
            if (m_painting) return;
            if (b->get_active()) {
                m_dropper = which;
                m_painting = true;
                for (auto* o : m_drop_btn) if (o && o != b) o->set_active(false);
                m_painting = false;
            } else if (m_dropper == which) {
                m_dropper = A::Dropper::None;
            } else return;
            m_dropper_sig.emit(m_dropper);
        });
        m_drop_btn[static_cast<std::size_t>(i)] = b;
        row->append(*b);
    }
    m_body.append(*row);
}

void AdjustEditor::dropped(const std::array<double, 3>& rgb) {
    if (m_dropper == A::Dropper::None) return;
    m_a = A::dropped(m_a, m_dropper, rgb);
    m_point = -1;
    emit();
    paint();
}

void AdjustEditor::drop_off() {
    m_dropper = A::Dropper::None;
    m_painting = true;
    for (auto* b : m_drop_btn) if (b) b->set_active(false);
    m_painting = false;
}

// ── s064: Hue/Saturation's range bar ───────────────────────────────────────

namespace {
constexpr double kHueMargin = 8, kHueTop = 2, kHueBand = 18, kHueBandH = 14, kHueLow = 34, kHueSpecH = 14;
double wrap360(double v) { return std::fmod(std::fmod(v, 360.0) + 360.0, 360.0); }
}  // namespace

int AdjustEditor::hue_page() const {
    return (m_huesat_ranges && m_page && !m_a.colorize) ? static_cast<int>(std::min<guint>(m_page->get_selected(), 6)) : 0;
}

void AdjustEditor::draw_huebar(const Cairo::RefPtr<Cairo::Context>& cr, int w, int) {
    const auto fg = fg_of(*m_graph);
    const int page = hue_page();
    const auto* band = page > 0 ? &m_a.ranges[static_cast<std::size_t>(page - 1)].band : nullptr;
    // The middle of the bar: the range's middle (Master: red at the ends, cyan in the middle).
    double centre = 180;
    if (band) centre = m_band_handle >= 0 ? m_band_centre : wrap360((*band)[0] + wrap360((*band)[3] - (*band)[0]) / 2);
    const double x0 = kHueMargin, W = std::max(1.0, w - 2 * kHueMargin);
    const auto hue_at = [&](double x) { return wrap360(centre - 180 + (x - x0) / W * 360); };
    for (int x = static_cast<int>(x0); x < static_cast<int>(x0 + W); ++x) {
        const double h = hue_at(x + 0.5);
        double r, g, b;
        A::from_hsl(h / 360, 1, 0.5, r, g, b);
        cr->set_source_rgb(r, g, b);
        cr->rectangle(x, kHueTop, 1.05, kHueSpecH);
        cr->fill();
        float R = static_cast<float>(r), G = static_cast<float>(g), B = static_cast<float>(b);
        A::apply_rgb(m_a, R, G, B);
        cr->set_source_rgb(R, G, B);
        cr->rectangle(x, kHueLow, 1.05, kHueSpecH);
        cr->fill();
    }
    cr->set_line_width(1);
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.3);
    cr->rectangle(x0 - 0.5, kHueTop - 0.5, W + 1, kHueSpecH + 1);
    cr->rectangle(x0 - 0.5, kHueLow - 0.5, W + 1, kHueSpecH + 1);
    cr->stroke();
    if (!band) return;
    // The range: full strength between the bars, fading out to the triangles.
    const auto x_of = [&](double hue) { return x0 + wrap360(hue - (centre - 180)) / 360 * W; };
    const double p0 = x_of((*band)[0]), p1 = x_of((*band)[1]), p2 = x_of((*band)[2]), p3 = x_of((*band)[3]);
    const double top = kHueBand, bot = kHueBand + kHueBandH;
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.25);
    cr->move_to(p0, bot); cr->line_to(p1, top + 3); cr->line_to(p2, top + 3); cr->line_to(p3, bot);
    cr->close_path();
    cr->fill();
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.55);
    cr->rectangle(p1, top + 3, p2 - p1, bot - top - 3);
    cr->fill();
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.95);
    for (const double px : {p1, p2}) { cr->rectangle(std::round(px) - 1.5, top, 3, bot - top); cr->fill(); }
    for (const double px : {p0, p3}) { tri_up(cr, px, top + 3, 4.5); cr->fill(); }
}

void AdjustEditor::huebar_press(double x, double) {
    m_band_handle = -1;
    const int page = hue_page();
    if (page == 0 || !m_graph) return;
    const auto& band = m_a.ranges[static_cast<std::size_t>(page - 1)].band;
    m_band_centre = wrap360(band[0] + wrap360(band[3] - band[0]) / 2);
    const double x0 = kHueMargin, W = std::max(1.0, m_graph->get_width() - 2 * kHueMargin);
    double best = 9;
    for (int k = 0; k < 4; ++k) {
        const double px = x0 + wrap360(band[static_cast<std::size_t>(k)] - (m_band_centre - 180)) / 360 * W;
        const double d = std::abs(px - x);
        if (d < best || (d == best && (k == 1 || k == 2))) { best = d; m_band_handle = k; }
    }
}

void AdjustEditor::huebar_drag(double x) {
    const int page = hue_page();
    if (page == 0 || m_band_handle < 0 || !m_graph) return;
    auto& band = m_a.ranges[static_cast<std::size_t>(page - 1)].band;
    const double x0 = kHueMargin, W = std::max(1.0, m_graph->get_width() - 2 * kHueMargin);
    // In the bar's own frame (0..360 left to right) the four are in order: keep them so.
    double p[4];
    for (int k = 0; k < 4; ++k) p[k] = wrap360(band[static_cast<std::size_t>(k)] - (m_band_centre - 180));
    const int k = m_band_handle;
    const double lo = k > 0 ? p[k - 1] : 0, hi = k < 3 ? p[k + 1] : 359;
    p[k] = std::clamp(std::round((x - x0) / W * 360), lo, hi);
    const int v = static_cast<int>(wrap360(p[k] + m_band_centre - 180)) % 360;
    if (v == band[static_cast<std::size_t>(k)]) return;
    band[static_cast<std::size_t>(k)] = v;
    emit();
    paint();
}

// ── s063: Gradient Map ─────────────────────────────────────────────────────

double AdjustEditor::grad_x(double at) const {
    const double w = m_graph ? m_graph->get_width() : 200;
    return kGradMargin + at * std::max(1.0, w - 2 * kGradMargin);
}
double AdjustEditor::grad_t(double x) const {
    const double w = m_graph ? m_graph->get_width() : 200;
    return std::clamp((x - kGradMargin) / std::max(1.0, w - 2 * kGradMargin), 0.0, 1.0);
}

void AdjustEditor::draw_gradient(const Cairo::RefPtr<Cairo::Context>& cr, int w, int) {
    const auto fg = fg_of(*m_graph);
    const double x0 = kGradMargin, x1 = w - kGradMargin;
    // The bar, a column at a time.
    for (int x = static_cast<int>(x0); x < static_cast<int>(std::ceil(x1)); ++x) {
        const auto c = A::gradient_at(m_a.grad, (x + 0.5 - x0) / std::max(1.0, x1 - x0));
        cr->set_source_rgb(c[0], c[1], c[2]);
        cr->rectangle(x, kBarTop, 1.05, kBarBottom - kBarTop);
        cr->fill();
    }
    cr->set_line_width(1);
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.35);
    cr->rectangle(std::round(x0) - 0.5, kBarTop - 0.5, std::round(x1) - std::round(x0) + 1, kBarBottom - kBarTop + 1);
    cr->stroke();
    const auto n = m_a.grad.size();
    // The midpoints: a small diamond between each two stops, off the picked one's sides.
    for (std::size_t i = 1; i < n; ++i) {
        const auto& p = m_a.grad[i - 1];
        const auto& q = m_a.grad[i];
        const double mx = grad_x(p.at + (q.at - p.at) * q.mid);
        const bool near_pick = static_cast<int>(i) == m_stop || static_cast<int>(i) - 1 == m_stop;
        cr->move_to(mx, kMidY - 3.5);
        cr->line_to(mx + 3.5, kMidY);
        cr->line_to(mx, kMidY + 3.5);
        cr->line_to(mx - 3.5, kMidY);
        cr->close_path();
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), near_pick ? 0.8 : 0.3);
        if (m_grad_drag == 2 && m_mid == static_cast<int>(i)) cr->fill();
        else cr->stroke();
    }
    // The stops: a little house in each colour, under the bar; the picked one bold.
    for (std::size_t i = 0; i < n; ++i) {
        if (m_stop_off && static_cast<int>(i) == m_stop) continue;   // about to go
        const auto& s = m_a.grad[i];
        const double sx = grad_x(s.at);
        cr->move_to(sx, kStopTop);
        cr->line_to(sx + 6, kStopTop + 5);
        cr->line_to(sx + 6, kStripH - 2);
        cr->line_to(sx - 6, kStripH - 2);
        cr->line_to(sx - 6, kStopTop + 5);
        cr->close_path();
        cr->set_source_rgb(s.rgb[0], s.rgb[1], s.rgb[2]);
        cr->fill_preserve();
        const bool picked = static_cast<int>(i) == m_stop;
        cr->set_line_width(picked ? 2.0 : 1.0);
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), picked ? 1.0 : 0.6);
        cr->stroke();
    }
}

void AdjustEditor::gradient_press(double x, double y) {
    m_grad_drag = 0;
    m_mid = -1;
    m_stop_off = false;
    if (y < kBarBottom) return;   // the bar itself: nothing to pick
    // A stop first (the picked one wins a tie), then a midpoint, else a new stop.
    int best = -1;
    double bd = 8;
    for (std::size_t i = 0; i < m_a.grad.size(); ++i) {
        const double d = std::abs(grad_x(m_a.grad[i].at) - x) - (static_cast<int>(i) == m_stop ? 0.5 : 0);
        if (y >= kStopTop - 2 && d < bd) { bd = d; best = static_cast<int>(i); }
    }
    if (best < 0 && y < kStopTop + 2)
        for (std::size_t i = 1; i < m_a.grad.size(); ++i) {
            const auto& p = m_a.grad[i - 1];
            const auto& q = m_a.grad[i];
            if (std::abs(grad_x(p.at + (q.at - p.at) * q.mid) - x) <= 5) { m_mid = static_cast<int>(i); m_grad_drag = 2; paint(); return; }
        }
    if (best < 0) {
        if (m_a.grad.size() >= core::kGradStops) return;
        const double t = grad_t(x);
        core::GradStop s{t, 0.5, A::gradient_at(m_a.grad, t)};
        std::size_t at = 0;
        while (at < m_a.grad.size() && m_a.grad[at].at <= t) ++at;
        m_a.grad.insert(m_a.grad.begin() + static_cast<std::ptrdiff_t>(at), s);
        best = static_cast<int>(at);
        m_stop = best;
        emit();
    }
    m_stop = best;
    m_grad_drag = 1;
    paint();
}

void AdjustEditor::gradient_drag(double x, double y) {
    if (m_grad_drag == 1 && m_stop >= 0 && m_stop < static_cast<int>(m_a.grad.size())) {
        const auto i = static_cast<std::size_t>(m_stop);
        const double lo = i > 0 ? m_a.grad[i - 1].at : 0, hi = i + 1 < m_a.grad.size() ? m_a.grad[i + 1].at : 1;
        m_a.grad[i].at = std::clamp(grad_t(x), lo, hi);
        const double h = m_graph ? m_graph->get_height() : kStripH;
        m_stop_off = m_a.grad.size() > 2 && (y > h + 24 || y < -24);
        emit();
        paint();
    } else if (m_grad_drag == 2 && m_mid > 0 && m_mid < static_cast<int>(m_a.grad.size())) {
        const auto& p = m_a.grad[static_cast<std::size_t>(m_mid) - 1];
        auto& q = m_a.grad[static_cast<std::size_t>(m_mid)];
        if (q.at - p.at > 1e-6) q.mid = std::clamp((grad_t(x) - p.at) / (q.at - p.at), 0.05, 0.95);
        emit();
        paint();
    }
}

void AdjustEditor::gradient_release() {
    if (m_stop_off && m_a.grad.size() > 2 && m_stop >= 0 && m_stop < static_cast<int>(m_a.grad.size())) {
        m_a.grad.erase(m_a.grad.begin() + m_stop);
        m_stop = std::max(0, m_stop - 1);
        m_stop_off = false;
        emit();
    }
    m_stop_off = false;
    m_grad_drag = 0;
    m_mid = -1;
    paint();
}

void AdjustEditor::totals() {
    if (!m_total || !m_page) return;
    const auto& m = m_a.monochrome ? m_a.mono : m_a.mixer[std::min<guint>(m_page->get_selected(), 2)];   // a stale page: the grey has one
    const int t = m[0] + m[1] + m[2];
    m_total->set_text((t > 100 ? "\u26A0 Total " : "Total ") + std::to_string(t) + " %");
}

void AdjustEditor::reset() {
    m_a = A::fresh(m_a.kind);
    m_point = -1;
    emit();
    paint();
}

// ── Threshold (s062) ────────────────────────────────────────────────────────
// The luminosity histogram over a black | white split at the level, and a
// handle under it.
void AdjustEditor::draw_threshold(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const auto fg = fg_of(*m_graph);
    const double x0 = kPad, x1 = w - kPad, top = kPad, bottom = h - 14.0;
    const double lx = x0 + (m_a.threshold / 255.0) * (x1 - x0);
    cr->set_source_rgba(0, 0, 0, 0.18);
    cr->rectangle(x0, top, lx - x0, bottom - top);
    cr->fill();
    cr->set_source_rgba(1, 1, 1, 0.18);
    cr->rectangle(lx, top, x1 - lx, bottom - top);
    cr->fill();
    if (m_hist) {
        const auto& bins = (*m_hist)[0];
        std::vector<std::uint32_t> sorted(bins.begin(), bins.end());
        std::sort(sorted.begin(), sorted.end());
        const double peak = std::max<double>(1, static_cast<double>(sorted[250]));   // a few spikes may clip
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.55);
        const double bw = (x1 - x0) / 256;
        for (int i = 0; i < 256; ++i) {
            const double v = std::min(1.0, bins[static_cast<std::size_t>(i)] / peak) * (bottom - top);
            cr->rectangle(x0 + i * bw, bottom - v, bw + 0.3, v);
        }
        cr->fill();
    }
    cr->set_line_width(1);
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.9);
    cr->move_to(std::round(lx) + 0.5, top);
    cr->line_to(std::round(lx) + 0.5, bottom);
    cr->stroke();
    tri_up(cr, lx, bottom + 2, 5);
    cr->set_source_rgb(0.5, 0.5, 0.5);
    cr->fill_preserve();
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.9);
    cr->stroke();
}

void AdjustEditor::threshold_at(double x) {
    const double x0 = kPad, x1 = m_graph->get_width() - kPad;
    const int v = std::clamp(static_cast<int>(std::lround((x - x0) / std::max(1.0, x1 - x0) * 255)), 1, 255);
    if (v == m_a.threshold) return;
    m_a.threshold = v;
    emit();
    paint();
}

// ── Levels ──────────────────────────────────────────────────────────────────
// Rows: the histogram (top .. h - 52), the input sliders under it, the output
// bar (h - 28 .. h - 18) and its sliders.
void AdjustEditor::draw_levels(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const auto fg = fg_of(*m_graph);
    const double x0 = kPad, x1 = w - kPad, top = kPad, bottom = h - 52.0;
    const int ch = channel();
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.06);
    cr->rectangle(x0, top, x1 - x0, bottom - top);
    cr->fill();
    if (m_hist) {
        const auto& bins = (*m_hist)[static_cast<std::size_t>(ch)];
        std::vector<std::uint32_t> sorted(bins.begin(), bins.end());
        std::sort(sorted.begin(), sorted.end());
        const double peak = std::max<double>(1, static_cast<double>(sorted[250]));   // a few spikes may clip
        const double rgb[4][3] = {{fg.get_red(), fg.get_green(), fg.get_blue()}, {0.85, 0.2, 0.2}, {0.2, 0.7, 0.25}, {0.25, 0.4, 0.9}};
        cr->set_source_rgba(rgb[ch][0], rgb[ch][1], rgb[ch][2], 0.55);
        const double bw = (x1 - x0) / 256;
        for (int i = 0; i < 256; ++i) {
            const double v = std::min(1.0, bins[static_cast<std::size_t>(i)] / peak) * (bottom - top);
            cr->rectangle(x0 + i * bw, bottom - v, bw + 0.3, v);
        }
        cr->fill();
    }
    const auto& c = m_a.levels[static_cast<std::size_t>(ch)];
    const auto X = [&](double v) { return x0 + v * (x1 - x0); };
    const double grey = c.in_black + (c.in_white - c.in_black) * std::pow(0.5, c.gamma);
    cr->set_line_width(1);
    const auto handle = [&](double x, double y, double shade) {
        tri_up(cr, x, y, 5);
        cr->set_source_rgb(shade, shade, shade);
        cr->fill_preserve();
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.9);
        cr->stroke();
    };
    handle(X(c.in_black), bottom + 2, 0);
    handle(X(grey), bottom + 2, 0.5);
    handle(X(c.in_white), bottom + 2, 1);
    auto grad = Cairo::LinearGradient::create(x0, 0, x1, 0);
    grad->add_color_stop_rgb(0, 0, 0, 0);
    grad->add_color_stop_rgb(1, 1, 1, 1);
    cr->set_source(grad);
    cr->rectangle(x0, h - 30.0, x1 - x0, 10);
    cr->fill();
    handle(X(c.out_black), h - 18.0, 0);
    handle(X(c.out_white), h - 18.0, 1);
}

void AdjustEditor::levels_press(double x, double y) {
    m_press_x = x;
    m_press_y = y;
    const int w = m_graph->get_width(), h = m_graph->get_height();
    const double x0 = kPad, x1 = w - kPad;
    const auto& c = m_a.levels[static_cast<std::size_t>(channel())];
    const auto X = [&](double v) { return x0 + v * (x1 - x0); };
    m_lv_handle = -1;
    double best = 1e9;
    const auto consider = [&](int i, double hx) {
        const double d = std::abs(hx - x);
        if (d < best) { best = d; m_lv_handle = i; }
    };
    if (y < h - 34.0) {
        consider(0, X(c.in_black));
        consider(2, X(c.in_white));
        consider(1, X(c.in_black + (c.in_white - c.in_black) * std::pow(0.5, c.gamma)));
    } else {
        consider(3, X(c.out_black));
        consider(4, X(c.out_white));
    }
    if (best > 14) m_lv_handle = -1;
}

void AdjustEditor::levels_drag(double x, double) {
    if (m_lv_handle < 0) return;
    const int w = m_graph->get_width();
    const double v = std::clamp((x - kPad) / (w - 2 * kPad), 0.0, 1.0);
    auto& c = m_a.levels[static_cast<std::size_t>(channel())];
    switch (m_lv_handle) {
        case 0: c.in_black = std::min(v, c.in_white - 2.0 / 255); break;
        case 2: c.in_white = std::max(v, c.in_black + 2.0 / 255); break;
        case 1: {
            const double t = std::clamp((v - c.in_black) / std::max(1e-6, c.in_white - c.in_black), 0.001, 0.999);
            c.gamma = std::clamp(std::log(t) / std::log(0.5), 0.1, 9.99);
            break;
        }
        case 3: c.out_black = v; break;
        case 4: c.out_white = v; break;
    }
    emit();
    paint();
}

// ── Curves ──────────────────────────────────────────────────────────────────
double AdjustEditor::graph_side(int w, int h) const { return std::max(10.0, std::min(w, h) - 2 * kPad); }

void AdjustEditor::draw_curves(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    const auto fg = fg_of(*m_graph);
    const double s = graph_side(w, h), left = (w - s) / 2, top = kPad;
    const int ch = channel();
    const auto X = [&](double v) { return left + v * s; };
    const auto Y = [&](double v) { return top + (1 - v) * s; };
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.06);
    cr->rectangle(left, top, s, s);
    cr->fill();
    if (m_hist) {
        const auto& bins = (*m_hist)[static_cast<std::size_t>(ch)];
        std::vector<std::uint32_t> sorted(bins.begin(), bins.end());
        std::sort(sorted.begin(), sorted.end());
        const double peak = std::max<double>(1, static_cast<double>(sorted[250]));
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.12);
        for (int i = 0; i < 256; ++i) {
            const double v = std::min(1.0, bins[static_cast<std::size_t>(i)] / peak) * s;
            cr->rectangle(left + i * s / 256, top + s - v, s / 256 + 0.3, v);
        }
        cr->fill();
    }
    cr->set_line_width(1);
    cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.18);
    for (int i = 1; i < 4; ++i) {
        cr->move_to(X(i / 4.0), top); cr->line_to(X(i / 4.0), top + s);
        cr->move_to(left, Y(i / 4.0)); cr->line_to(left + s, Y(i / 4.0));
    }
    cr->move_to(X(0), Y(0));
    cr->line_to(X(1), Y(1));
    cr->stroke();
    // The other channels' curves, faint, when on RGB.
    const double rgb[4][3] = {{fg.get_red(), fg.get_green(), fg.get_blue()}, {0.85, 0.2, 0.2}, {0.2, 0.7, 0.25}, {0.25, 0.4, 0.9}};
    core::Adjust shown = m_a;
    if (m_point_off && m_point >= 0) {
        auto& pts = shown.curves[static_cast<std::size_t>(ch)];
        if (m_point < static_cast<int>(pts.size())) pts.erase(pts.begin() + m_point);
    }
    {
        const auto t = A::curve_table(shown.curves[static_cast<std::size_t>(ch)], 256);
        cr->set_source_rgba(rgb[ch][0], rgb[ch][1], rgb[ch][2], 0.95);
        cr->set_line_width(2);
        for (std::size_t i = 0; i < t.size(); ++i) {
            const double x = X(static_cast<double>(i) / 255), y = Y(t[i]);
            if (i == 0) cr->move_to(x, y); else cr->line_to(x, y);
        }
        cr->stroke();
    }
    if (ch == 0)
        for (int c = 1; c < 4; ++c) {
            const auto& pts = shown.curves[static_cast<std::size_t>(c)];
            if (pts.size() < 2 || A::identity([&] { core::Adjust a = A::fresh(AdjustKind::Curves); a.curves[0] = pts; return a; }())) continue;
            const auto t = A::curve_table(pts, 128);
            cr->set_source_rgba(rgb[c][0], rgb[c][1], rgb[c][2], 0.45);
            cr->set_line_width(1);
            for (std::size_t i = 0; i < t.size(); ++i) {
                const double x = X(static_cast<double>(i) / 127), y = Y(t[i]);
                if (i == 0) cr->move_to(x, y); else cr->line_to(x, y);
            }
            cr->stroke();
        }
    const auto& pts = shown.curves[static_cast<std::size_t>(ch)].size() >= 2 ? shown.curves[static_cast<std::size_t>(ch)]
                                                                             : std::vector<core::CurvePt>{{0, 0}, {1, 1}};
    for (std::size_t i = 0; i < pts.size(); ++i) {
        cr->rectangle(X(pts[i].x) - 4, Y(pts[i].y) - 4, 8, 8);
        const bool sel = !m_point_off && static_cast<int>(i) == m_point;
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 1);
        if (sel) cr->fill();
        else { cr->set_line_width(1.5); cr->stroke(); }
    }
}

void AdjustEditor::curves_press(double x, double y) {
    m_press_x = x;
    m_press_y = y;
    m_point_off = false;
    const int w = m_graph->get_width(), h = m_graph->get_height();
    const double s = graph_side(w, h), left = (w - s) / 2, top = kPad;
    auto& pts = curve_of(m_a, channel());
    m_point = -1;
    double best = 11;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const double d = std::hypot(left + pts[i].x * s - x, top + (1 - pts[i].y) * s - y);
        if (d < best) { best = d; m_point = static_cast<int>(i); }
    }
    if (m_point < 0) {
        const double gx = (x - left) / s, gy = 1 - (y - top) / s;
        if (gx <= 0 || gx >= 1 || gy < -0.02 || gy > 1.02 || pts.size() >= 16) { paint(); return; }
        auto at = std::find_if(pts.begin(), pts.end(), [&](const core::CurvePt& p) { return p.x > gx; });
        if (at != pts.end() && at->x - gx < kGap) { paint(); return; }
        if (at != pts.begin() && gx - std::prev(at)->x < kGap) { paint(); return; }
        at = pts.insert(at, {gx, std::clamp(gy, 0.0, 1.0)});
        m_point = static_cast<int>(at - pts.begin());
        emit();
    }
    paint();
}

void AdjustEditor::curves_drag(double x, double y) {
    if (m_point < 0) return;
    const int w = m_graph->get_width(), h = m_graph->get_height();
    const double s = graph_side(w, h), left = (w - s) / 2, top = kPad;
    auto& pts = curve_of(m_a, channel());
    if (m_point >= static_cast<int>(pts.size())) return;
    const auto i = static_cast<std::size_t>(m_point);
    const bool end = i == 0 || i + 1 == pts.size();
    // An inner point dragged well off the graph: removed (put back if it returns).
    const double out = std::max({left - x, x - (left + s), top - y, y - (top + s)});
    m_point_off = !end && pts.size() > 2 && out > 24;
    if (!m_point_off) {
        const double gx = (x - left) / s, gy = 1 - (y - top) / s;
        const double lo = i > 0 ? pts[i - 1].x + kGap : 0, hi = i + 1 < pts.size() ? pts[i + 1].x - kGap : 1;
        pts[i] = {std::clamp(gx, lo, hi), std::clamp(gy, 0.0, 1.0)};
    }
    emit();
    paint();
}

void AdjustEditor::curves_release() {
    if (m_point_off && m_point >= 0) {
        auto& pts = curve_of(m_a, channel());
        if (m_point < static_cast<int>(pts.size())) pts.erase(pts.begin() + m_point);
        m_point = -1;
    }
    m_point_off = false;
    paint();
}

}  // namespace klimt
