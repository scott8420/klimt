#include "FilterDialog.hpp"
#include "Registry.hpp"

#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/expander.h>
#include <gtkmm/stringlist.h>
#include <glibmm/markup.h>

#include <algorithm>
#include <cmath>

namespace klimt {

namespace F = core::filter;
using core::FilterKind;

FilterDialog::FilterDialog(Gtk::Window& parent)
    : m_body("shell.filter.body", Gtk::Orientation::VERTICAL, 8),
      m_busy("shell.filter.busy"),
      m_ok_button("shell.filter.ok", "OK"), m_cancel_button("shell.filter.cancel", "Cancel") {
    set_name("shell.filter");
    registry::add("shell.filter", this);
    set_modal(true);
    set_resizable(false);
    set_transient_for(parent);
    set_hide_on_close(true);
    set_default_size(360, -1);
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(false);
    m_ok_button.add_css_class("suggested-action");
    hb->pack_start(m_cancel_button);
    hb->pack_end(m_ok_button);
    set_titlebar(*hb);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    box->set_margin(16);
    box->set_size_request(340, -1);
    box->append(m_body);
    auto* foot = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    m_preview.set_name("shell.filter.preview");
    m_preview.set_active(true);
    m_preview.set_tooltip_text("Show it on the canvas while you choose (what the view shows)");
    m_preview.set_hexpand(true);
    foot->append(m_preview);
    m_busy.add_css_class("dim-label");
    m_busy.set_text("Previewing…");
    m_busy.set_visible(false);
    foot->append(m_busy);
    // s018: Live -- a live filter on the layer instead of new pixels.
    m_live.set_name("shell.filter.live");
    m_live.set_tooltip_text("Keep the layer's pixels: the filter goes under the layer (Properties), "
                            "where you can hide it, change it or reorder it later");
    box->append(m_live);
    box->append(*foot);
    set_child(*box);

    m_preview.signal_toggled().connect([this] { m_changed.emit(); });
    m_live.signal_toggled().connect([this] { if (m_depth) sync(); m_changed.emit(); });   // s041 m4: the channel note follows
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        const core::Filter f = F::normalised(m_f);
        m_ok.emit(f);   // first: the Shell keeps the preview up until the result lands
        close();
    });
    signal_hide().connect([this] { focal_pick_off(); m_closed.emit(); });
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Escape) { if (focal_picking()) focal_pick_off(); else close(); return true; }   // s039 m2
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { m_ok_button.activate(); return true; }
        return false;
    }, false);
    add_controller(keys);
}

FilterDialog::~FilterDialog() { registry::remove(this); }

void FilterDialog::set_busy(bool b) { m_busy.set_visible(b); }

void FilterDialog::ask(const core::Filter& start, const std::string& layer) {
    const bool kind_changed = start.kind != m_f.kind;
    m_f = F::normalised(start);
    m_threshold = m_f.threshold;
    m_angle = m_f.angle;
    set_title(std::string(F::label(m_f.kind)) + (m_editing ? " (live)" : "") + " -- “" + layer + "”");
    if (kind_changed || m_rows.empty()) rebuild();
    else if (m_depth) { m_syncing = true; m_depth->set_model(depth_model()); m_syncing = false; }   // s041 m4: this document's channels
    sync();
    m_busy.set_visible(false);
    present();
}

void FilterDialog::set_live(bool live, bool editing) {
    m_editing = editing;
    m_live.set_active(live);
    m_live.set_visible(!editing);
}

void FilterDialog::row(const char* name, const char* label, double lo, double hi, int digits, bool log, const char* unit, double* field) {
    // Photoshop's layout: the label and the value on a line, the slider under them.
    auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->set_xalign(0.0f);
    l->set_hexpand(true);
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
    u->set_size_request(48, -1);
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
    into.append(*s);
    const std::size_t i = m_rows.size();
    m_rows.push_back({s, n, lo, hi, log, field});
    s->signal_value_changed().connect([this, i] {
        if (m_syncing) return;
        const Row& w = m_rows[i];
        const double t = w.s->get_value() / 1000;
        double v = w.log ? w.lo * std::pow(w.hi / w.lo, t) : w.lo + (w.hi - w.lo) * t;
        const double q = w.n->get_digits() ? 10 : 1;
        *w.field = std::round(v * q) / q;
        sync(static_cast<int>(i));   // not the slider itself: it's being dragged
        m_changed.emit();
    });
    n->signal_value_changed().connect([this, i] {
        if (m_syncing) return;
        *m_rows[i].field = m_rows[i].n->get_value();
        sync();
        m_changed.emit();
    });
}

void FilterDialog::rebuild() {
    while (auto* c = m_body.get_first_child()) m_body.remove(*c);
    m_rows.clear();
    m_remove = nullptr;
    m_shape = nullptr;
    m_motion_only.clear();
    m_into = nullptr;
    m_gauss = m_uniform = m_mono = nullptr;
    if (focal_picking()) { m_focal_pick->set_active(false); }
    m_depth = nullptr; m_invert = nullptr; m_depth_note = nullptr; m_focal_pick = nullptr;   // s039
    m_depth_only.clear();
    m_method = m_quality = nullptr;   // s040
    m_choices.clear();                // s041 m3
    m_centre_box = nullptr;
    auto head = Gtk::make_managed<Gtk::Label>();
    head->set_markup("<b>" + Glib::Markup::escape_text(std::string(F::label(m_f.kind))) + "</b>");
    head->set_xalign(0.0f);
    m_body.append(*head);
    switch (m_f.kind) {
        case FilterKind::GaussianBlur:
        case FilterKind::HighPass:
            row("filter.radius", "Radius", 0.1, 250, 1, true, "pixels", &m_f.radius);
            break;
        case FilterKind::UnsharpMask:
            row("filter.amount", "Amount", 1, 500, 0, false, "%", &m_f.amount);
            row("filter.radius", "Radius", 0.1, 250, 1, true, "pixels", &m_f.radius);
            row("filter.threshold", "Threshold", 0, 255, 0, false, "levels", &m_threshold);
            break;
        case FilterKind::BoxBlur:   // m3
            row("filter.radius", "Radius", 1, 250, 0, true, "pixels", &m_f.radius);
            break;
        case FilterKind::Median:
            row("filter.radius", "Radius", 1, 100, 0, true, "pixels", &m_f.radius);
            break;
        case FilterKind::MotionBlur:
            row("filter.angle", "Angle", -90, 90, 0, false, "°", &m_angle);
            row("filter.distance", "Distance", 1, 500, 0, true, "pixels", &m_f.radius);
            break;
        case FilterKind::AddNoise:
            noise_rows(0.1, 400, 1, true);
            break;
        case FilterKind::SmartSharpen: smart_rows(); break;   // s034
        case FilterKind::DustScratches:
            row("filter.radius", "Radius", 1, 100, 0, true, "pixels", &m_f.radius);
            row("filter.threshold", "Threshold", 0, 255, 0, false, "levels", &m_threshold);
            break;
        case FilterKind::ReduceNoise: reduce_rows(); break;
        case FilterKind::Maximum:
        case FilterKind::Minimum: extreme_rows(); break;   // m3
        case FilterKind::SurfaceBlur:   // m4
            row("filter.radius", "Radius", 1, 100, 0, true, "pixels", &m_f.radius);
            row("filter.threshold", "Threshold", 2, 255, 0, false, "levels", &m_threshold);
            break;
        case FilterKind::LensBlur: lens_rows(); break;   // s037, s039
        case FilterKind::RadialBlur: radial_rows(); break;   // s040
        case FilterKind::FindEdges:   // s042 m1: Stylize
        case FilterKind::Solarize: {
            auto l = Gtk::make_managed<Gtk::Label>(m_f.kind == FilterKind::FindEdges
                                                       ? "No settings: the edges drawn dark on white, in their colours."
                                                       : "No settings: the tones past half turned over -- a print flashed with light.");
            l->set_wrap(true);
            l->set_xalign(0.0f);
            l->add_css_class("dim-label");
            m_body.append(*l);
            break;
        }
        case FilterKind::Clouds:   // s042 m3: Render
        case FilterKind::DifferenceClouds: {
            auto l = Gtk::make_managed<Gtk::Label>(m_f.kind == FilterKind::Clouds
                                                       ? "From the foreground colour to the background colour."
                                                       : "The clouds' difference with what is there -- run it again for marble.");
            l->set_wrap(true);
            l->set_xalign(0.0f);
            l->add_css_class("dim-label");
            m_body.append(*l);
            randomize_button();
            break;
        }
        case FilterKind::Diffuse:   // s042 m2
            choice("filter.diffuse_mode", "Mode", {"Normal", "Darken Only", "Lighten Only", "Anisotropic"}, [this] { return m_f.axis; },
                   [this](int v) { m_f.axis = v; }, "Normal: each pixel takes a neighbour's colour; Darken / Lighten Only: only a darker / lighter one; "
                   "Anisotropic: the most alike -- grain smoothed along the edges");
            break;
        case FilterKind::Wind:
            choice("filter.wind_method", "Method", {"Wind", "Blast", "Stagger"}, [this] { return m_f.axis; }, [this](int v) { m_f.axis = v; });
            choice("filter.wind_from", "Direction", {"From the Right", "From the Left"}, [this] { return m_f.from_left ? 1 : 0; },
                   [this](int v) { m_f.from_left = v == 1; });
            break;
        case FilterKind::Emboss:
            row("filter.angle", "Angle", -180, 180, 0, false, "°", &m_angle);
            row("filter.height", "Height", 1, 10, 0, false, "pixels", &m_f.radius);
            row("filter.amount", "Amount", 1, 500, 0, false, "%", &m_f.amount);
            break;
        case FilterKind::PolarCoordinates:   // s041 m3
            choice("filter.polar", "Mode", {"Rectangular to Polar", "Polar to Rectangular"}, [this] { return m_f.axis; },
                   [this](int v) { m_f.axis = v; }, "Rectangular to Polar: the top edge gathers into the middle, rows become rings");
            break;
        case FilterKind::Ripple:
            row("filter.amount", "Amount", -999, 999, 0, false, "%", &m_f.amount);
            choice("filter.size", "Size", {"Small", "Medium", "Large"}, [this] { return m_f.quality; }, [this](int v) { m_f.quality = v; });
            break;
        case FilterKind::Wave:
            row("filter.generators", "Number of Generators", 1, 999, 0, true, "", &m_f.generators);   // s042 m4
            row("filter.wl_min", "Wavelength Min.", 2, 999, 0, true, "pixels", &m_f.wl_min);
            row("filter.wavelength", "Wavelength Max.", 2, 999, 0, true, "pixels", &m_f.radius);
            row("filter.amp_min", "Amplitude Min.", 1, 999, 0, true, "pixels", &m_f.amp_min);
            row("filter.amplitude", "Amplitude Max.", 1, 999, 0, true, "pixels", &m_f.amount);
            row("filter.scale_h", "Horizontal", 0, 100, 0, false, "%", &m_f.scale_h);
            row("filter.scale_v", "Vertical", 0, 100, 0, false, "%", &m_f.scale_v);
            choice("filter.wave_type", "Type", {"Sine", "Triangle", "Square"}, [this] { return m_f.axis; }, [this](int v) { m_f.axis = v; });
            choice("filter.undefined", "Undefined Areas", {"Repeat Edge Pixels", "Wrap Around"}, [this] { return m_f.wrap ? 1 : 0; },
                   [this](int v) { m_f.wrap = v == 1; }, "What fills in where the picture moved away from its edge");
            randomize_button();   // s042 m4: other wavelengths, amplitudes and phases
            break;
        case FilterKind::ZigZag:
            row("filter.amount", "Amount", -100, 100, 0, false, "", &m_f.amount);
            row("filter.ridges", "Ridges", 1, 20, 0, false, "", &m_f.ridges);
            choice("filter.zigzag_style", "Style", {"Around Center", "Out From Center", "Pond Ripples"}, [this] { return m_f.axis; },
                   [this](int v) { m_f.axis = v; });
            break;
        case FilterKind::Twirl:      // s040 m4: Distort -- in the ellipse fitted to the selection (or the picture)
            row("filter.angle", "Angle", -999, 999, 0, false, "°", &m_angle);
            break;
        case FilterKind::Pinch:
            row("filter.amount", "Amount", -100, 100, 0, false, "%", &m_f.amount);
            break;
        case FilterKind::Spherize: {
            row("filter.amount", "Amount", -100, 100, 0, false, "%", &m_f.amount);
            auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            auto l = Gtk::make_managed<Gtk::Label>("Mode");
            l->set_xalign(0.0f);
            l->set_hexpand(true);
            line->append(*l);
            m_method = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Normal", "Horizontal only", "Vertical only"}));
            m_method->set_name("filter.axis");
            line->append(*m_method);
            m_body.append(*line);
            m_method->property_selected().signal_changed().connect([this] {
                if (m_syncing) return;
                m_f.axis = static_cast<int>(m_method->get_selected());
                m_changed.emit();
            });
            break;
        }
        case FilterKind::None: break;
    }
}

void FilterDialog::sync(int keep_slider) {
    m_f.threshold = static_cast<int>(std::lround(m_threshold));
    m_f.angle = static_cast<int>(std::lround(m_angle));
    const std::uint32_t seed = m_f.seed;
    m_f = F::normalised(m_f);
    m_f.seed = seed;
    m_threshold = m_f.threshold;
    m_angle = m_f.angle;
    m_syncing = true;
    for (std::size_t i = 0; i < m_rows.size(); ++i) {
        const Row& w = m_rows[i];
        const double v = std::clamp(*w.field, w.lo, w.hi);
        const double t = w.log ? std::log(v / w.lo) / std::log(w.hi / w.lo) : (v - w.lo) / (w.hi - w.lo);
        if (static_cast<int>(i) != keep_slider) w.s->set_value(t * 1000);
        w.n->set_value(v);
    }
    if (m_gauss) { m_gauss->set_active(m_f.gaussian); m_uniform->set_active(!m_f.gaussian); }
    if (m_mono) m_mono->set_active(m_f.mono);
    if (m_shape) m_shape->set_selected(m_f.round ? 1u : 0u);   // m3
    for (const auto& ch : m_choices) ch.dd->set_selected(static_cast<guint>(ch.get()));   // s041 m3
    if (m_method) m_method->set_selected(m_f.kind == FilterKind::Spherize ? static_cast<guint>(m_f.axis) : m_f.zoom ? 1u : 0u);   // s040
    if (m_centre_box) {   // s040 m2: the picture's shape, 240 px on its long side
        const double k = 240.0 / std::max(m_iw, m_ih);
        m_centre_box->set_content_width(std::max(40, static_cast<int>(m_iw * k)));
        m_centre_box->set_content_height(std::max(40, static_cast<int>(m_ih * k)));
        m_centre_box->queue_draw();
    }
    if (m_quality) m_quality->set_selected(static_cast<guint>(m_f.quality));
    if (m_depth) {   // s039
        guint at = m_f.depth == core::DepthSource::Transparency ? 1u : m_f.depth == core::DepthSource::LayerMask ? 2u : 0u;
        bool gone = false;   // s041 m4: a channel since deleted
        if (m_f.depth == core::DepthSource::Channel) {
            gone = true;
            for (std::size_t k = 0; k < m_channels.size(); ++k)
                if (m_channels[k].first == m_f.depth_channel) { at = static_cast<guint>(3 + k); gone = false; }
        }
        m_depth->set_selected(at);
        for (auto* w : m_depth_only) w->set_sensitive(m_f.depth != core::DepthSource::None);
        if (m_invert) m_invert->set_active(m_f.invert);
        if (m_depth_note) {
            std::string note;
            if (m_f.depth == core::DepthSource::LayerMask && !m_has_mask) note = "The layer has no mask: the blur is even, as None.";
            else if (m_f.depth == core::DepthSource::Channel && gone) note = "That channel is gone: the blur is even, as None.";
            else if (m_f.depth == core::DepthSource::Channel && live())
                note = "A live Lens Blur sees only its layer, not the channels: the blur is even. Untick Live, or use Layer Mask.";
            m_depth_note->set_text(note);
            m_depth_note->set_visible(!note.empty());
        }
    }
    if (m_remove) {   // s034
        m_remove->set_selected(m_f.remove == core::Remove::Lens ? 1u : m_f.remove == core::Remove::Motion ? 2u : 0u);
        for (auto* w : m_motion_only) w->set_visible(m_f.remove == core::Remove::Motion);
    }
    m_syncing = false;
}

// Add Noise's Amount, Distribution and Monochromatic -- s039: Lens Blur's Noise too.
void FilterDialog::noise_rows(double lo, double hi, int digits, bool log) {
    row("filter.amount", "Amount", lo, hi, digits, log, "%", &m_f.amount);
    auto dist = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    auto l = Gtk::make_managed<Gtk::Label>("Distribution");
    l->set_xalign(0.0f);
    l->set_size_request(76, -1);
    l->add_css_class("dim-label");
    dist->append(*l);
    m_uniform = Gtk::make_managed<Gtk::CheckButton>("Uniform");
    m_uniform->set_name("filter.uniform");
    m_gauss = Gtk::make_managed<Gtk::CheckButton>("Gaussian");
    m_gauss->set_name("filter.gaussian");
    m_gauss->set_group(*m_uniform);
    dist->append(*m_uniform);
    dist->append(*m_gauss);
    Gtk::Box& into = m_into ? *m_into : static_cast<Gtk::Box&>(m_body);
    into.append(*dist);
    m_mono = Gtk::make_managed<Gtk::CheckButton>("Monochromatic");
    m_mono->set_name("filter.mono");
    m_mono->set_tooltip_text("The same noise in every channel -- grey grain, no colour speckles");
    into.append(*m_mono);
    m_gauss->signal_toggled().connect([this] {
        if (m_syncing) return;
        m_f.gaussian = m_gauss->get_active();
        m_changed.emit();
    });
    m_mono->signal_toggled().connect([this] {
        if (m_syncing) return;
        m_f.mono = m_mono->get_active();
        m_changed.emit();
    });
}

// s039 m2: Set Focal Point.
bool FilterDialog::focal_picking() const { return m_focal_pick && m_focal_pick->get_active(); }
void FilterDialog::focal_pick_off() {
    if (focal_picking()) m_focal_pick->set_active(false);   // its handler re-modals and tells the Shell
}
void FilterDialog::set_focal_at(double depth) {
    depth = std::clamp(depth, 0.0, 1.0);
    m_f.focal = std::round((m_f.invert ? 1 - depth : depth) * 255);
    focal_pick_off();
    sync();
    m_changed.emit();
}

// s040: Radial Blur -- Amount, Method, Quality, the Blur Centre.
void FilterDialog::radial_rows() {
    row("filter.amount", "Amount", 1, 100, 0, false, "", &m_f.amount);
    const auto pick = [this](const char* label, const char* name, std::vector<Glib::ustring> items, const char* tip) {
        auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto l = Gtk::make_managed<Gtk::Label>(label);
        l->set_xalign(0.0f);
        l->set_hexpand(true);
        line->append(*l);
        auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
        dd->set_name(name);
        dd->set_tooltip_text(tip);
        line->append(*dd);
        line->set_margin_bottom(4);
        m_body.append(*line);
        return dd;
    };
    m_method = pick("Method", "filter.method", {"Spin", "Zoom"},
                    "Spin: arcs round the centre (a wheel turning); Zoom: streaks along the rays from it (a zoom burst)");
    m_quality = pick("Quality", "filter.quality", {"Draft", "Good", "Best"},
                     "Draft: fast and grainy; Good: smooth; Best: smoothest on long streaks");
    m_method->property_selected().signal_changed().connect([this] {
        if (m_syncing) return;
        m_f.zoom = m_method->get_selected() == 1;
        if (m_centre_box) m_centre_box->queue_draw();
        m_changed.emit();
    });
    m_quality->property_selected().signal_changed().connect([this] {
        if (m_syncing) return;
        m_f.quality = static_cast<int>(m_quality->get_selected());
        m_changed.emit();
    });
    // s040 m2: THE BLUR CENTRE -- the picture's shape; rings (Spin) or rays
    // (Zoom) round the centre; click or drag to move it.
    auto lab = Gtk::make_managed<Gtk::Label>();
    lab->set_markup("<small><b>Blur Centre</b></small>");
    lab->set_xalign(0.0f);
    lab->add_css_class("dim-label");
    lab->set_margin_top(6);
    m_body.append(*lab);
    m_centre_box = Gtk::make_managed<Gtk::DrawingArea>();
    m_centre_box->set_name("filter.centre_box");
    m_centre_box->set_tooltip_text("Click or drag: the blur's centre on the picture");
    m_centre_box->set_halign(Gtk::Align::CENTER);
    m_centre_box->set_cursor("crosshair");
    m_centre_box->set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        cr->set_source_rgb(0.12, 0.12, 0.13);
        cr->rectangle(0, 0, w, h);
        cr->fill();
        const double x = m_f.cx / 100 * w, y = m_f.cy / 100 * h;
        cr->set_source_rgba(0.85, 0.85, 0.88, 0.55);
        cr->set_line_width(1);
        const double far = std::hypot(std::max(x, w - x), std::max(y, h - y));
        if (m_f.zoom) {
            for (int k = 0; k < 24; ++k) {
                const double a = k * 3.14159265358979 / 12;
                cr->move_to(x + 8 * std::cos(a), y + 8 * std::sin(a));
                cr->line_to(x + far * std::cos(a), y + far * std::sin(a));
            }
        } else {
            for (double r = 10; r < far; r += 10) { cr->begin_new_sub_path(); cr->arc(x, y, r, 0, 2 * 3.14159265358979); }
        }
        cr->stroke();
        cr->set_source_rgb(1, 1, 1);
        cr->set_line_width(1.5);
        cr->begin_new_sub_path();
        cr->arc(x, y, 5, 0, 2 * 3.14159265358979);
        cr->stroke();
        cr->set_source_rgba(1, 1, 1, 0.35);
        cr->rectangle(0.5, 0.5, w - 1, h - 1);
        cr->stroke();
    });
    auto drag = Gtk::GestureDrag::create();
    drag->signal_drag_begin().connect([this](double x, double y) { centre_from(x, y); });
    drag->signal_drag_update().connect([this, drag](double dx, double dy) {
        double sx = 0, sy = 0;
        drag->get_start_point(sx, sy);
        centre_from(sx + dx, sy + dy);
    });
    m_centre_box->add_controller(drag);
    m_body.append(*m_centre_box);
    m_focal_pick = Gtk::make_managed<Gtk::ToggleButton>("Pick on Picture");
    m_focal_pick->set_name("filter.centre_pick");
    m_focal_pick->set_tooltip_text("Then click the picture: the blur's centre goes there (Esc: never mind)");
    m_focal_pick->set_halign(Gtk::Align::START);
    m_body.append(*m_focal_pick);
    m_focal_pick->signal_toggled().connect([this] {
        const bool on = m_focal_pick->get_active();
        set_modal(!on);
        m_focal_signal.emit(on);
    });
    row("filter.cx", "Centre X", 0, 100, 1, false, "%", &m_f.cx);
    row("filter.cy", "Centre Y", 0, 100, 1, false, "%", &m_f.cy);
}

void FilterDialog::choice(const char* name, const char* label, std::vector<Glib::ustring> items, std::function<int()> get,
                          std::function<void(int)> put, const char* tip) {
    auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->set_xalign(0.0f);
    l->set_hexpand(true);
    line->append(*l);
    auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
    dd->set_name(name);
    if (tip) dd->set_tooltip_text(tip);
    line->append(*dd);
    line->set_margin_bottom(4);
    m_body.append(*line);
    m_choices.push_back({dd, std::move(get)});
    dd->property_selected().signal_changed().connect([this, dd, put = std::move(put)] {
        if (m_syncing) return;
        put(static_cast<int>(dd->get_selected()));
        m_changed.emit();
    });
}

// s042 m3: Randomize -- a new pattern (the seed).
void FilterDialog::randomize_button() {
    auto b = Gtk::make_managed<Gtk::Button>("Randomize");
    b->set_name("filter.randomize");
    b->set_halign(Gtk::Align::START);
    b->set_tooltip_text("Another pattern");
    b->signal_clicked().connect([this] {
        m_f.seed = m_f.seed * 1664525u + 1013904223u;
        if (m_f.seed == 0) m_f.seed = 1;
        m_changed.emit();
    });
    m_body.append(*b);
}

void FilterDialog::centre_from(double x, double y) {
    if (!m_centre_box) return;
    const double w = std::max(1, m_centre_box->get_width()), h = std::max(1, m_centre_box->get_height());
    m_f.cx = std::clamp(x / w * 100, 0.0, 100.0);
    m_f.cy = std::clamp(y / h * 100, 0.0, 100.0);
    sync();
    m_changed.emit();
}

// s041 m4: None, Transparency, Layer Mask -- then each saved channel by name.
Glib::RefPtr<Gtk::StringList> FilterDialog::depth_model() const {
    std::vector<Glib::ustring> items{"None", "Transparency", "Layer Mask"};
    for (const auto& [id, name] : m_channels) items.push_back("Channel: " + name);
    return Gtk::StringList::create(items);
}

void FilterDialog::set_centre_at(double cx, double cy) {
    m_f.cx = std::clamp(cx, 0.0, 100.0);
    m_f.cy = std::clamp(cy, 0.0, 100.0);
    focal_pick_off();
    sync();
    m_changed.emit();
}

// s037 / s039: Lens Blur -- Photoshop's order: the Depth Map, the Iris, the
// Specular Highlights.
void FilterDialog::lens_rows() {
    const auto section = [this](const char* text) {
        auto l = Gtk::make_managed<Gtk::Label>();
        l->set_markup(std::string("<small><b>") + text + "</b></small>");
        l->set_xalign(0.0f);
        l->set_margin_top(6);
        l->add_css_class("dim-label");
        m_body.append(*l);
    };
    section("Depth Map");
    auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>("Source");
    l->set_xalign(0.0f);
    l->set_hexpand(true);
    line->append(*l);
    m_depth = Gtk::make_managed<Gtk::DropDown>(depth_model());   // s041 m4: and the saved channels
    m_depth->set_name("filter.depth");
    m_depth->set_tooltip_text("What says how far each pixel is: None -- the same blur everywhere; Transparency -- the layer's "
                              "alpha; Layer Mask -- its mask (white 255, black 0). Shift-click the mask to switch it off "
                              "and it still serves here");
    line->append(*m_depth);
    line->set_margin_bottom(4);
    m_body.append(*line);
    row("filter.focal", "Blur Focal Distance", 0, 255, 0, false, "", &m_f.focal);
    m_depth_only = {m_rows.back().s, m_rows.back().n->get_parent()};
    m_rows.back().s->set_tooltip_text("The depth that stays sharp; the further a pixel's depth from it, the more it blurs");
    m_invert = Gtk::make_managed<Gtk::CheckButton>("Invert");
    m_invert->set_name("filter.invert");
    m_invert->set_tooltip_text("Turn the depth map over: what was near is far");
    m_body.append(*m_invert);
    m_depth_only.push_back(m_invert);
    // s039 m2: Set Focal Point -- click the picture: what is there stays sharp.
    m_focal_pick = Gtk::make_managed<Gtk::ToggleButton>("Set Focal Point");
    m_focal_pick->set_name("filter.focal_pick");
    m_focal_pick->set_tooltip_text("Then click the picture: the focal distance becomes the depth there, so it stays sharp "
                                   "(Esc: never mind)");
    m_focal_pick->set_halign(Gtk::Align::START);
    m_body.append(*m_focal_pick);
    m_depth_only.push_back(m_focal_pick);
    m_focal_pick->signal_toggled().connect([this] {
        const bool on = m_focal_pick->get_active();
        set_modal(!on);   // armed: the canvas takes the next click
        m_focal_signal.emit(on);
    });
    m_depth_note = Gtk::make_managed<Gtk::Label>("The layer has no mask: the blur is even, as None.");
    m_depth_note->set_name("filter.depth_note");
    m_depth_note->set_xalign(0.0f);
    m_depth_note->set_wrap(true);
    m_depth_note->add_css_class("dim-label");
    m_depth_note->add_css_class("caption");
    m_body.append(*m_depth_note);
    m_depth->property_selected().signal_changed().connect([this] {
        if (m_syncing) return;
        const auto i = m_depth->get_selected();
        m_f.depth = i == 1 ? core::DepthSource::Transparency : i == 2 ? core::DepthSource::LayerMask
                  : i >= 3 && i - 3 < m_channels.size() ? core::DepthSource::Channel : core::DepthSource::None;
        m_f.depth_channel = m_f.depth == core::DepthSource::Channel ? m_channels[i - 3].first : std::string();   // s041 m4
        if (m_f.depth == core::DepthSource::None) focal_pick_off();
        sync();
        m_changed.emit();
    });
    m_invert->signal_toggled().connect([this] {
        if (m_syncing) return;
        m_f.invert = m_invert->get_active();
        m_changed.emit();
    });
    section("Iris");
    row("filter.radius", "Radius", 1, 100, 0, true, "pixels", &m_f.radius);
    row("filter.blades", "Blades", 3, 8, 0, false, "", &m_f.blades);
    row("filter.curvature", "Blade Curvature", 0, 100, 0, false, "%", &m_f.curvature);
    row("filter.rotation", "Rotation", 0, 360, 0, false, "°", &m_angle);
    // Folded away (the dialog fits a laptop's screen); open when in use.
    const auto fold = [this](const char* title, const char* name, bool open) {
        auto* ex = Gtk::make_managed<Gtk::Expander>(title);
        ex->set_name(name);
        ex->set_expanded(open);
        ex->set_margin_top(4);
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        box->set_margin_start(12);
        box->set_margin_top(4);
        ex->set_child(*box);
        m_body.append(*ex);
        m_into = box;
    };
    fold("Specular Highlights", "filter.highlights", m_f.bright > 0);
    row("filter.bright", "Brightness", 0, 100, 0, false, "%", &m_f.bright);
    row("filter.threshold", "Threshold", 0, 255, 0, false, "levels", &m_threshold);
    fold("Noise", "filter.noise_box", m_f.amount > 0);   // s039 m2: grain back in what is blurred
    noise_rows(0, 100, 0, false);
    m_rows[m_rows.size() - 1].s->set_tooltip_text("Blurring smooths the film grain or sensor noise away; this puts some back, "
                                                  "as much as each pixel is blurred");
    m_into = nullptr;
}

// s034: Smart Sharpen -- Amount, Radius, Reduce Noise, Remove (Motion: its
// Angle), then Shadows and Highlights folded away (Photoshop's layout).
void FilterDialog::smart_rows() {
    row("filter.amount", "Amount", 1, 500, 0, false, "%", &m_f.amount);
    row("filter.radius", "Radius", 0.1, 64, 1, true, "pixels", &m_f.radius);
    row("filter.noise", "Reduce Noise", 0, 100, 0, false, "%", &m_f.noise);
    auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>("Remove");
    l->set_xalign(0.0f);
    l->set_hexpand(true);
    line->append(*l);
    m_remove = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Gaussian Blur", "Lens Blur", "Motion Blur"}));
    m_remove->set_name("filter.remove");
    m_remove->set_tooltip_text("The blur to take away: Gaussian (as Unsharp Mask), Lens (a soft focus -- finer edges, fewer halos), "
                               "Motion (a streak at the angle)");
    line->append(*m_remove);
    line->set_margin_bottom(4);
    m_body.append(*line);
    row("filter.angle", "Angle", -90, 90, 0, false, "°", &m_angle);
    m_motion_only = {m_rows.back().s, m_rows.back().n->get_parent()};
    m_remove->property_selected().signal_changed().connect([this] {
        if (m_syncing) return;
        const auto i = m_remove->get_selected();
        m_f.remove = i == 1 ? core::Remove::Lens : i == 2 ? core::Remove::Motion : core::Remove::Gaussian;
        for (auto* w : m_motion_only) w->set_visible(m_f.remove == core::Remove::Motion);
        m_changed.emit();
    });
    for (const bool dark : {true, false}) {
        auto* ex = Gtk::make_managed<Gtk::Expander>(dark ? "Shadows" : "Highlights");
        ex->set_name(dark ? "filter.shadows" : "filter.highlights");
        ex->set_tooltip_text(dark ? "Less sharpening in the dark areas" : "Less sharpening in the light areas");
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        box->set_margin_start(12);
        box->set_margin_top(4);
        ex->set_child(*box);
        m_body.append(*ex);
        m_into = box;
        row(dark ? "filter.shadow_fade" : "filter.light_fade", "Fade Amount", 0, 100, 0, false, "%", dark ? &m_f.shadow_fade : &m_f.light_fade);
        row(dark ? "filter.shadow_tonal" : "filter.light_tonal", "Tonal Width", 0, 100, 0, false, "%", dark ? &m_f.shadow_tonal : &m_f.light_tonal);
        row(dark ? "filter.shadow_radius" : "filter.light_radius", "Radius", 1, 100, 0, true, "pixels", dark ? &m_f.shadow_radius : &m_f.light_radius);
        m_into = nullptr;
    }
}

// s034: Reduce Noise -- Strength, Preserve Details, Reduce Colour Noise, Sharpen Details.
void FilterDialog::reduce_rows() {
    row("filter.strength", "Strength", 0, 10, 0, false, "", &m_f.amount);
    row("filter.preserve", "Preserve Details", 0, 100, 0, false, "%", &m_f.preserve);
    row("filter.colour", "Reduce Colour Noise", 0, 100, 0, false, "%", &m_f.colour);
    row("filter.sharpen", "Sharpen Details", 0, 100, 0, false, "%", &m_f.sharpen);
}

// s034 m3: Maximum / Minimum -- Radius, Preserve Squareness / Roundness.
void FilterDialog::extreme_rows() {
    row("filter.radius", "Radius", 1, 500, 0, true, "pixels", &m_f.radius);
    auto line = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>("Preserve");
    l->set_xalign(0.0f);
    l->set_hexpand(true);
    line->append(*l);
    m_shape = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Squareness", "Roundness"}));
    m_shape->set_name("filter.preserve_shape");
    m_shape->set_tooltip_text("Squareness: corners stay square (fast). Roundness: grows and shrinks round, as a brush would");
    line->append(*m_shape);
    m_body.append(*line);
    m_shape->property_selected().signal_changed().connect([this] {
        if (m_syncing) return;
        m_f.round = m_shape->get_selected() == 1;
        m_changed.emit();
    });
}

}  // namespace klimt
