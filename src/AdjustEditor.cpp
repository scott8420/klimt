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
    s->signal_value_changed().connect([this, s, field] {
        if (m_painting) return;
        *field = static_cast<int>(std::lround(s->get_value()));
        emit();
    });
    begin_end_on(*s);
    row->append(*s);
    m_body.append(*row);
    m_sliders.push_back({s, field});
    return s;
}

void AdjustEditor::rebuild() {
    while (auto* c = m_body.get_first_child()) m_body.remove(*c);
    m_sliders.clear();
    m_colorize = nullptr;
    m_channel = nullptr;
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
        case AdjustKind::HueSat: {
            slider("adjust.hue", "Hue", -180, 180, &m_a.hue);
            slider("adjust.saturation", "Saturation", -100, 100, &m_a.saturation);
            slider("adjust.lightness", "Lightness", -100, 100, &m_a.lightness);
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
            break;
        }
        case AdjustKind::None: break;
    }
}

void AdjustEditor::paint() {
    m_painting = true;
    for (const auto& b : m_sliders) {
        if (b.field == &m_a.hue) b.s->set_range(m_a.colorize ? 0 : -180, m_a.colorize ? 360 : 180);
        if (b.field == &m_a.saturation) b.s->set_range(m_a.colorize ? 0 : -100, 100);
        b.s->set_value(*b.field);
    }
    if (m_colorize) m_colorize->set_active(m_a.colorize);
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
    if (m_point_off && m_point >= 0) {   // a point dragged off the graph: gone (for now)
        auto& pts = out.curves[static_cast<std::size_t>(channel())];
        if (m_point < static_cast<int>(pts.size())) pts.erase(pts.begin() + m_point);
    }
    if (m_graph) m_graph->queue_draw();
    m_changed.emit(A::normalised(out));
}

void AdjustEditor::reset() {
    m_a = A::fresh(m_a.kind);
    m_point = -1;
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
