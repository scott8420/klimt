#include "FilterLayerEditor.hpp"

#include <glibmm/markup.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/label.h>

#include <cmath>

namespace klimt {

namespace F = core::filter;
using core::FilterKind;

FilterLayerEditor::FilterLayerEditor() : Gtk::Box(Gtk::Orientation::VERTICAL, 4) {
    set_name("properties.filter_layer");
    append(m_body);
}

void FilterLayerEditor::set(const core::Filter& f) {
    m_f = f;
    m_threshold = f.threshold;
    m_angle = f.angle;
    if (f.kind != m_built) rebuild();
    sync();
}

void FilterLayerEditor::emit() {
    m_f.threshold = static_cast<int>(std::lround(m_threshold));
    m_f.angle = static_cast<int>(std::lround(m_angle));
    m_changed.emit(m_f);
}

void FilterLayerEditor::row(const char* name, const char* label, double lo, double hi, int digits, bool log, const char* unit, double* field) {
    auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->set_xalign(0.0f);
    l->set_hexpand(true);
    l->add_css_class("dim-label");
    top->append(*l);
    auto n = Gtk::make_managed<Gtk::SpinButton>();
    n->set_name(std::string(name) + ".value");
    n->set_range(lo, hi);
    const double step = digits ? 0.1 : 1;
    n->set_increments(step, step * 10);
    n->set_digits(static_cast<guint>(digits));
    n->set_width_chars(5);
    n->set_numeric(true);
    top->append(*n);
    auto u = Gtk::make_managed<Gtk::Label>(unit);
    u->set_size_request(40, -1);
    u->set_xalign(0.0f);
    u->add_css_class("dim-label");
    top->append(*u);
    m_body.append(*top);
    auto s = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
    s->set_name(std::string(name) + ".slider");
    s->set_range(0, 1000);   // a position; log or linear onto lo..hi
    s->set_increments(1, 50);
    s->set_draw_value(false);
    s->set_hexpand(true);
    s->set_margin_bottom(4);
    // A drag on the slider: one step for the whole drag.
    auto drag = Gtk::GestureDrag::create();
    drag->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    drag->signal_drag_begin().connect([this](double, double) { m_begin.emit(); });
    drag->signal_drag_end().connect([this](double, double) { m_end.emit(); });
    drag->signal_cancel().connect([this](const Gdk::EventSequence*) { m_end.emit(); });
    s->add_controller(drag);
    m_body.append(*s);
    const std::size_t i = m_rows.size();
    m_rows.push_back({s, n, lo, hi, log, field});
    s->signal_value_changed().connect([this, i] {
        if (m_syncing) return;
        const Row& w = m_rows[i];
        const double t = w.s->get_value() / 1000;
        const double v = w.log ? w.lo * std::pow(w.hi / w.lo, t) : w.lo + (w.hi - w.lo) * t;
        const double q = w.n->get_digits() ? 10 : 1;
        *w.field = std::round(v * q) / q;
        sync(static_cast<int>(i));
        emit();
    });
    n->signal_value_changed().connect([this, i] {
        if (m_syncing) return;
        *m_rows[i].field = m_rows[i].n->get_value();
        sync();
        emit();
    });
}

void FilterLayerEditor::rebuild() {
    while (auto* c = m_body.get_first_child()) m_body.remove(*c);
    m_rows.clear();
    m_gauss = m_mono = nullptr;
    m_built = m_f.kind;
    auto head = Gtk::make_managed<Gtk::Label>();
    head->set_markup("<b>" + Glib::Markup::escape_text(std::string(F::label(m_f.kind))) + "</b>");
    head->set_xalign(0.0f);
    m_body.append(*head);
    switch (m_f.kind) {
        case FilterKind::GaussianBlur:
        case FilterKind::HighPass:
            row("filter_layer.radius", "Radius", 0.1, 250, 1, true, "px", &m_f.radius);
            break;
        case FilterKind::UnsharpMask:
            row("filter_layer.amount", "Amount", 1, 500, 0, false, "%", &m_f.amount);
            row("filter_layer.radius", "Radius", 0.1, 250, 1, true, "px", &m_f.radius);
            row("filter_layer.threshold", "Threshold", 0, 255, 0, false, "levels", &m_threshold);
            break;
        case FilterKind::BoxBlur:
            row("filter_layer.radius", "Radius", 1, 250, 0, true, "px", &m_f.radius);
            break;
        case FilterKind::Median:
            row("filter_layer.radius", "Radius", 1, 100, 0, true, "px", &m_f.radius);
            break;
        case FilterKind::MotionBlur:
            row("filter_layer.angle", "Angle", -90, 90, 0, false, "°", &m_angle);
            row("filter_layer.distance", "Distance", 1, 500, 0, true, "px", &m_f.radius);
            break;
        case FilterKind::AddNoise: {
            row("filter_layer.amount", "Amount", 0.1, 400, 1, true, "%", &m_f.amount);
            m_gauss = Gtk::make_managed<Gtk::CheckButton>("Gaussian");
            m_gauss->set_tooltip_text("Off: uniform noise");
            m_gauss->signal_toggled().connect([this] { if (!m_syncing) { m_f.gaussian = m_gauss->get_active(); emit(); } });
            m_body.append(*m_gauss);
            m_mono = Gtk::make_managed<Gtk::CheckButton>("Monochromatic");
            m_mono->signal_toggled().connect([this] { if (!m_syncing) { m_f.mono = m_mono->get_active(); emit(); } });
            m_body.append(*m_mono);
            break;
        }
        default: break;
    }
    auto note = Gtk::make_managed<Gtk::Label>("Filters everything below it. To filter one layer only, clip it to that layer "
                                              "(the Layers panel's ⋮ > Clip / Release). Its mask, opacity and blend mode work as on any layer.");
    note->set_wrap(true);
    note->set_xalign(0.0f);
    note->add_css_class("dim-label");
    note->add_css_class("caption");
    m_body.append(*note);
}

void FilterLayerEditor::sync(int keep) {
    m_syncing = true;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        const Row& w = m_rows[i];
        const double v = std::clamp(*w.field, w.lo, w.hi);
        w.n->set_value(v);
        if (static_cast<int>(i) != keep) {
            const double t = w.log ? std::log(v / w.lo) / std::log(w.hi / w.lo) : (v - w.lo) / (w.hi - w.lo);
            w.s->set_value(t * 1000);
        }
    }
    if (m_gauss) m_gauss->set_active(m_f.gaussian);
    if (m_mono) m_mono->set_active(m_f.mono);
    m_syncing = false;
}

}  // namespace klimt
