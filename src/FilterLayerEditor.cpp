#include "FilterLayerEditor.hpp"

#include <glibmm/markup.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>

#include <gtkmm/expander.h>
#include <gtkmm/stringlist.h>

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
    Gtk::Box& into = m_into ? *m_into : static_cast<Gtk::Box&>(m_body);
    into.append(*top);
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
    into.append(*s);
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

// s041 m2: a label and a dropdown on a line; a choice is ONE step.
void FilterLayerEditor::choice(const char* name, const char* label, std::vector<Glib::ustring> items, std::function<int()> get,
                               std::function<void(int)> put) {
    auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->set_xalign(0.0f);
    l->set_hexpand(true);
    line->append(*l);
    auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
    dd->set_name(name);
    line->append(*dd);
    m_body.append(*line);
    m_choices.push_back({dd, std::move(get)});
    dd->property_selected().signal_changed().connect([this, dd, put = std::move(put)] {
        if (m_syncing) return;
        put(static_cast<int>(dd->get_selected()));
        emit();
    });
}

void FilterLayerEditor::rebuild() {
    while (auto* c = m_body.get_first_child()) m_body.remove(*c);
    m_rows.clear();
    m_remove = nullptr;
    m_shape = nullptr;
    m_motion_only.clear();
    m_into = nullptr;
    m_gauss = m_mono = nullptr;
    m_choices.clear();   // s041 m2
    m_shear = nullptr;   // s044 m2
    m_depth = nullptr; m_invert = nullptr; m_depth_only.clear();   // s039
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
        case FilterKind::SmartSharpen: smart_rows(); break;   // s034
        case FilterKind::DustScratches:
            row("filter_layer.radius", "Radius", 1, 100, 0, true, "px", &m_f.radius);
            row("filter_layer.threshold", "Threshold", 0, 255, 0, false, "levels", &m_threshold);
            break;
        case FilterKind::ReduceNoise: reduce_rows(); break;
        case FilterKind::Maximum:
        case FilterKind::Minimum: extreme_rows(); break;   // m3
        case FilterKind::SurfaceBlur:   // m4
            row("filter_layer.radius", "Radius", 1, 100, 0, true, "px", &m_f.radius);
            row("filter_layer.threshold", "Threshold", 2, 255, 0, false, "levels", &m_threshold);
            break;
        case FilterKind::RadialBlur: {   // s040
            row("filter_layer.amount", "Amount", 1, 100, 0, false, "", &m_f.amount);
            choice("filter_layer.method", "Method", {"Spin", "Zoom"}, [this] { return m_f.zoom ? 1 : 0; }, [this](int v) { m_f.zoom = v == 1; });   // s041 m2
            choice("filter_layer.quality", "Quality", {"Draft", "Good", "Best"}, [this] { return m_f.quality; }, [this](int v) { m_f.quality = v; });
            row("filter_layer.cx", "Centre X", 0, 100, 1, false, "%", &m_f.cx);
            row("filter_layer.cy", "Centre Y", 0, 100, 1, false, "%", &m_f.cy);
            break;
        }
        case FilterKind::Clouds:   // s042 m3
        case FilterKind::DifferenceClouds:
        case FilterKind::Diffuse:   // s042 m2 (Clouds join here for the Randomize button)
            if (m_f.kind != FilterKind::Diffuse) {
                auto rb = Gtk::make_managed<Gtk::Button>("Randomize");
                rb->set_name("filter_layer.randomize");
                rb->set_halign(Gtk::Align::START);
                rb->signal_clicked().connect([this] { m_f.seed = m_f.seed * 1664525u + 1013904223u; if (!m_f.seed) m_f.seed = 1; emit(); });
                m_body.append(*rb);
                break;
            }
            choice("filter_layer.diffuse_mode", "Mode", {"Normal", "Darken Only", "Lighten Only", "Anisotropic"}, [this] { return m_f.axis; },
                   [this](int v) { m_f.axis = v; });
            break;
        case FilterKind::Shear: {   // s044 m2
            m_shear = Gtk::make_managed<widgets::ShearCurve>("filter_layer.shear_curve");
            m_shear->set(m_f);
            m_shear->signal_begin().connect([this] { m_begin.emit(); });
            m_shear->signal_end().connect([this] { m_end.emit(); });
            m_shear->signal_changed().connect([this] { if (!m_syncing) { m_f.shear = m_shear->value().shear; emit(); } });
            m_body.append(*m_shear);
            choice("filter_layer.undefined", "Undefined Areas", {"Repeat Edge Pixels", "Wrap Around"}, [this] { return m_f.wrap ? 1 : 0; },
                   [this](int v) { m_f.wrap = v == 1; });
            break;
        }
        case FilterKind::Lighting:   // s045 m1
            choice("filter_layer.light", "Light", {"Point", "Spot", "Infinite"}, [this] { return m_f.axis; }, [this](int v) { m_f.axis = v; });
            row("filter_layer.intensity", "Intensity", 0, 200, 0, false, "%", &m_f.amount);
            row("filter_layer.angle", "Direction", -180, 180, 0, false, "°", &m_angle);
            row("filter_layer.hotspot", "Hotspot", 5, 90, 0, false, "°", &m_f.hotspot);
            row("filter_layer.reach", "Reach", 5, 200, 0, false, "%", &m_f.radius);
            row("filter_layer.ambient", "Ambience", 0, 100, 0, false, "%", &m_f.ambient);
            row("filter_layer.gloss", "Gloss", 0, 100, 0, false, "%", &m_f.gloss);
            choice("filter_layer.texture", "Texture", {"None", "The Layer's Lightness"}, [this] { return m_f.texture ? 1 : 0; },
                   [this](int v) { m_f.texture = v == 1; });
            row("filter_layer.height", "Height", 0, 100, 0, false, "", &m_f.height);
            row("filter_layer.cx", "Centre X", 0, 100, 1, false, "%", &m_f.cx);
            row("filter_layer.cy", "Centre Y", 0, 100, 1, false, "%", &m_f.cy);
            break;
        case FilterKind::LensFlare:   // s044 m1
            row("filter_layer.brightness", "Brightness", 10, 300, 0, false, "%", &m_f.amount);
            choice("filter_layer.lens", "Lens Type", {"50-300mm Zoom", "35mm Prime", "105mm Prime", "Movie Prime"}, [this] { return m_f.axis; },
                   [this](int v) { m_f.axis = v; });
            row("filter_layer.cx", "Centre X", 0, 100, 1, false, "%", &m_f.cx);
            row("filter_layer.cy", "Centre Y", 0, 100, 1, false, "%", &m_f.cy);
            break;
        case FilterKind::Fibers: {   // s043 m3
            row("filter_layer.variance", "Variance", 1, 64, 0, false, "", &m_f.amount);
            row("filter_layer.strength", "Strength", 1, 64, 0, false, "", &m_f.ridges);
            auto rb = Gtk::make_managed<Gtk::Button>("Randomize");
            rb->set_name("filter_layer.randomize");
            rb->set_halign(Gtk::Align::START);
            rb->signal_clicked().connect([this] { m_f.seed = m_f.seed * 1664525u + 1013904223u; if (!m_f.seed) m_f.seed = 1; emit(); });
            m_body.append(*rb);
            break;
        }
        case FilterKind::Mosaic:   // s043 m1: Pixelate (Fragment: nothing to set)
            row("filter_layer.cell", "Cell Size", 2, 200, 0, true, "px", &m_f.radius);
            break;
        case FilterKind::ColourHalftone:   // s043 m2 (Facet: nothing to set)
            row("filter_layer.max_radius", "Max. Radius", 4, 127, 0, true, "px", &m_f.radius);
            row("filter_layer.screen1", "Channel 1", -360, 360, 0, false, "°", &m_f.screen[0]);
            row("filter_layer.screen2", "Channel 2", -360, 360, 0, false, "°", &m_f.screen[1]);
            row("filter_layer.screen3", "Channel 3", -360, 360, 0, false, "°", &m_f.screen[2]);
            break;
        case FilterKind::Mezzotint: {
            choice("filter_layer.mezzotint", "Type", {"Fine Dots", "Medium Dots", "Grainy Dots", "Coarse Dots", "Short Lines", "Medium Lines",
                                                       "Long Lines", "Short Strokes", "Medium Strokes", "Long Strokes"},
                   [this] { return m_f.axis; }, [this](int v) { m_f.axis = v; });
            auto rb = Gtk::make_managed<Gtk::Button>("Randomize");
            rb->set_name("filter_layer.randomize");
            rb->set_halign(Gtk::Align::START);
            rb->signal_clicked().connect([this] { m_f.seed = m_f.seed * 1664525u + 1013904223u; if (!m_f.seed) m_f.seed = 1; emit(); });
            m_body.append(*rb);
            break;
        }
        case FilterKind::Crystallize:
        case FilterKind::Pointillize: {
            row("filter_layer.cell", "Cell Size", 3, 300, 0, true, "px", &m_f.radius);
            auto rb = Gtk::make_managed<Gtk::Button>("Randomize");
            rb->set_name("filter_layer.randomize");
            rb->set_halign(Gtk::Align::START);
            rb->signal_clicked().connect([this] { m_f.seed = m_f.seed * 1664525u + 1013904223u; if (!m_f.seed) m_f.seed = 1; emit(); });
            m_body.append(*rb);
            break;
        }
        case FilterKind::Wind:
            choice("filter_layer.wind_method", "Method", {"Wind", "Blast", "Stagger"}, [this] { return m_f.axis; }, [this](int v) { m_f.axis = v; });
            choice("filter_layer.wind_from", "Direction", {"From the Right", "From the Left"}, [this] { return m_f.from_left ? 1 : 0; },
                   [this](int v) { m_f.from_left = v == 1; });
            break;
        case FilterKind::Emboss:   // s042 m1 (Find Edges, Solarize: nothing to set)
            row("filter_layer.angle", "Angle", -180, 180, 0, false, "°", &m_angle);
            row("filter_layer.height", "Height", 1, 10, 0, false, "px", &m_f.radius);
            row("filter_layer.amount", "Amount", 1, 500, 0, false, "%", &m_f.amount);
            break;
        case FilterKind::PolarCoordinates:   // s041 m3
            choice("filter_layer.polar", "Mode", {"Rectangular to Polar", "Polar to Rectangular"}, [this] { return m_f.axis; },
                   [this](int v) { m_f.axis = v; });
            break;
        case FilterKind::Ripple:
            row("filter_layer.amount", "Amount", -999, 999, 0, false, "%", &m_f.amount);
            choice("filter_layer.size", "Size", {"Small", "Medium", "Large"}, [this] { return m_f.quality; }, [this](int v) { m_f.quality = v; });
            break;
        case FilterKind::Wave:
            row("filter_layer.generators", "Generators", 1, 999, 0, true, "", &m_f.generators);   // s042 m4
            row("filter_layer.wl_min", "Wavelength Min.", 2, 999, 0, true, "px", &m_f.wl_min);
            row("filter_layer.wavelength", "Wavelength Max.", 2, 999, 0, true, "px", &m_f.radius);
            row("filter_layer.amp_min", "Amplitude Min.", 1, 999, 0, true, "px", &m_f.amp_min);
            row("filter_layer.amplitude", "Amplitude Max.", 1, 999, 0, true, "px", &m_f.amount);
            row("filter_layer.scale_h", "Horizontal", 0, 100, 0, false, "%", &m_f.scale_h);
            row("filter_layer.scale_v", "Vertical", 0, 100, 0, false, "%", &m_f.scale_v);
            choice("filter_layer.wave_type", "Type", {"Sine", "Triangle", "Square"}, [this] { return m_f.axis; }, [this](int v) { m_f.axis = v; });
            choice("filter_layer.undefined", "Undefined Areas", {"Repeat Edge Pixels", "Wrap Around"}, [this] { return m_f.wrap ? 1 : 0; },
                   [this](int v) { m_f.wrap = v == 1; });
            {
                auto rb = Gtk::make_managed<Gtk::Button>("Randomize");
                rb->set_name("filter_layer.randomize");
                rb->set_halign(Gtk::Align::START);
                rb->signal_clicked().connect([this] { m_f.seed = m_f.seed * 1664525u + 1013904223u; if (!m_f.seed) m_f.seed = 1; emit(); });
                m_body.append(*rb);
            }
            break;
        case FilterKind::ZigZag:
            row("filter_layer.amount", "Amount", -100, 100, 0, false, "", &m_f.amount);
            row("filter_layer.ridges", "Ridges", 1, 20, 0, false, "", &m_f.ridges);
            choice("filter_layer.zigzag_style", "Style", {"Around Center", "Out From Center", "Pond Ripples"}, [this] { return m_f.axis; },
                   [this](int v) { m_f.axis = v; });
            break;
        case FilterKind::Twirl:   // s040 m4
            row("filter_layer.angle", "Angle", -999, 999, 0, false, "°", &m_angle);
            break;
        case FilterKind::Pinch:
            row("filter_layer.amount", "Amount", -100, 100, 0, false, "%", &m_f.amount);
            break;
        case FilterKind::Spherize:
            row("filter_layer.amount", "Amount", -100, 100, 0, false, "%", &m_f.amount);
            choice("filter_layer.axis", "Mode", {"Normal", "Horizontal only", "Vertical only"}, [this] { return m_f.axis; },
                   [this](int v) { m_f.axis = v; });   // s041 m2
            break;
        case FilterKind::LensBlur: {   // s037: the iris, then the specular highlights
            // s039: the depth map -- the transparency of what is below (a
            // filter layer has no pixels of its own to mask).
            auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            auto l = Gtk::make_managed<Gtk::Label>("Depth Map");
            l->set_xalign(0.0f);
            l->set_hexpand(true);
            line->append(*l);
            m_depth = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"None", "Transparency"}));
            m_depth->set_name("filter_layer.depth");
            m_depth->set_tooltip_text("Transparency: what is below it, its alpha as the depth (opaque 255)");
            line->append(*m_depth);
            m_body.append(*line);
            m_depth->property_selected().signal_changed().connect([this] {
                if (m_syncing) return;
                m_f.depth = m_depth->get_selected() == 1 ? core::DepthSource::Transparency : core::DepthSource::None;
                sync();
                emit();
            });
            row("filter_layer.focal", "Blur Focal Distance", 0, 255, 0, false, "", &m_f.focal);
            m_depth_only = {m_rows.back().s, m_rows.back().n->get_parent()};
            m_invert = Gtk::make_managed<Gtk::CheckButton>("Invert");
            m_invert->set_name("filter_layer.invert");
            m_invert->signal_toggled().connect([this] { if (!m_syncing) { m_f.invert = m_invert->get_active(); emit(); } });
            m_body.append(*m_invert);
            m_depth_only.push_back(m_invert);
            row("filter_layer.radius", "Radius", 1, 100, 0, true, "px", &m_f.radius);
            row("filter_layer.blades", "Blades", 3, 8, 0, false, "", &m_f.blades);
            row("filter_layer.curvature", "Blade Curvature", 0, 100, 0, false, "%", &m_f.curvature);
            row("filter_layer.rotation", "Rotation", 0, 360, 0, false, "°", &m_angle);
            row("filter_layer.bright", "Highlight Brightness", 0, 100, 0, false, "%", &m_f.bright);
            row("filter_layer.threshold", "Highlight Threshold", 0, 255, 0, false, "levels", &m_threshold);
            // s039 m2: its Noise
            row("filter_layer.amount", "Noise", 0, 100, 0, false, "%", &m_f.amount);
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
    if (m_shear && m_shear->value().shear != m_f.shear) m_shear->set(m_f);   // s044 m2: an undo, another layer's
    for (const auto& ch : m_choices) ch.dd->set_selected(static_cast<guint>(ch.get()));   // s041 m2
    if (m_mono) m_mono->set_active(m_f.mono);
    if (m_shape) m_shape->set_selected(m_f.round ? 1u : 0u);   // m3
    if (m_depth) {   // s039
        m_depth->set_selected(m_f.depth == core::DepthSource::Transparency ? 1u : 0u);
        for (auto* w : m_depth_only) w->set_sensitive(m_f.depth != core::DepthSource::None);
        m_invert->set_active(m_f.invert);
    }
    if (m_remove) {   // s034
        m_remove->set_selected(m_f.remove == core::Remove::Lens ? 1u : m_f.remove == core::Remove::Motion ? 2u : 0u);
        for (auto* w : m_motion_only) w->set_visible(m_f.remove == core::Remove::Motion);
    }
    m_syncing = false;
}

// s034: Smart Sharpen -- Amount, Radius, Reduce Noise, Remove (Motion: its
// Angle), then Shadows and Highlights folded away (Photoshop's layout).
void FilterLayerEditor::smart_rows() {
    row("filter_layer.amount", "Amount", 1, 500, 0, false, "%", &m_f.amount);
    row("filter_layer.radius", "Radius", 0.1, 64, 1, true, "px", &m_f.radius);
    row("filter_layer.noise", "Reduce Noise", 0, 100, 0, false, "%", &m_f.noise);
    auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>("Remove");
    l->set_xalign(0.0f);
    l->set_hexpand(true);
    line->append(*l);
    m_remove = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Gaussian Blur", "Lens Blur", "Motion Blur"}));
    m_remove->set_name("filter_layer.remove");
    m_remove->set_tooltip_text("The blur to take away: Gaussian (as Unsharp Mask), Lens (a soft focus -- finer edges, fewer halos), "
                               "Motion (a streak at the angle)");
    line->append(*m_remove);
    line->set_margin_bottom(4);
    m_body.append(*line);
    row("filter_layer.angle", "Angle", -90, 90, 0, false, "°", &m_angle);
    m_motion_only = {m_rows.back().s, m_rows.back().n->get_parent()};
    m_remove->property_selected().signal_changed().connect([this] {
        if (m_syncing) return;
        const auto i = m_remove->get_selected();
        m_f.remove = i == 1 ? core::Remove::Lens : i == 2 ? core::Remove::Motion : core::Remove::Gaussian;
        for (auto* w : m_motion_only) w->set_visible(m_f.remove == core::Remove::Motion);
        emit();
    });
    for (const bool dark : {true, false}) {
        auto* ex = Gtk::make_managed<Gtk::Expander>(dark ? "Shadows" : "Highlights");
        ex->set_name(dark ? "filter_layer.shadows" : "filter_layer.highlights");
        ex->set_tooltip_text(dark ? "Less sharpening in the dark areas" : "Less sharpening in the light areas");
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        box->set_margin_start(12);
        box->set_margin_top(4);
        ex->set_child(*box);
        m_body.append(*ex);
        m_into = box;
        row(dark ? "filter_layer.shadow_fade" : "filter_layer.light_fade", "Fade Amount", 0, 100, 0, false, "%", dark ? &m_f.shadow_fade : &m_f.light_fade);
        row(dark ? "filter_layer.shadow_tonal" : "filter_layer.light_tonal", "Tonal Width", 0, 100, 0, false, "%", dark ? &m_f.shadow_tonal : &m_f.light_tonal);
        row(dark ? "filter_layer.shadow_radius" : "filter_layer.light_radius", "Radius", 1, 100, 0, true, "px", dark ? &m_f.shadow_radius : &m_f.light_radius);
        m_into = nullptr;
    }
}

// s034: Reduce Noise -- Strength, Preserve Details, Reduce Colour Noise, Sharpen Details.
void FilterLayerEditor::reduce_rows() {
    row("filter_layer.strength", "Strength", 0, 10, 0, false, "", &m_f.amount);
    row("filter_layer.preserve", "Preserve Details", 0, 100, 0, false, "%", &m_f.preserve);
    row("filter_layer.colour", "Reduce Colour Noise", 0, 100, 0, false, "%", &m_f.colour);
    row("filter_layer.sharpen", "Sharpen Details", 0, 100, 0, false, "%", &m_f.sharpen);
}

// s034 m3: Maximum / Minimum -- Radius, Preserve Squareness / Roundness.
void FilterLayerEditor::extreme_rows() {
    row("filter_layer.radius", "Radius", 1, 500, 0, true, "px", &m_f.radius);
    auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>("Preserve");
    l->set_xalign(0.0f);
    l->set_hexpand(true);
    line->append(*l);
    m_shape = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Squareness", "Roundness"}));
    m_shape->set_name("filter_layer.preserve_shape");
    m_shape->set_tooltip_text("Squareness: corners stay square (fast). Roundness: grows and shrinks round, as a brush would");
    line->append(*m_shape);
    m_body.append(*line);
    m_shape->property_selected().signal_changed().connect([this] {
        if (m_syncing) return;
        m_f.round = m_shape->get_selected() == 1;
        emit();
    });
}

}  // namespace klimt
