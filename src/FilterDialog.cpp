#include "FilterDialog.hpp"
#include "Registry.hpp"

#include <gtkmm/eventcontrollerkey.h>
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
    m_live.signal_toggled().connect([this] { m_changed.emit(); });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        const core::Filter f = F::normalised(m_f);
        m_ok.emit(f);   // first: the Shell keeps the preview up until the result lands
        close();
    });
    signal_hide().connect([this] { m_closed.emit(); });
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Escape) { close(); return true; }
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
        case FilterKind::AddNoise: {
            row("filter.amount", "Amount", 0.1, 400, 1, true, "%", &m_f.amount);
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
            m_body.append(*dist);
            m_mono = Gtk::make_managed<Gtk::CheckButton>("Monochromatic");
            m_mono->set_name("filter.mono");
            m_mono->set_tooltip_text("The same noise in every channel -- grey grain, no colour speckles");
            m_body.append(*m_mono);
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
            break;
        }
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
    if (m_remove) {   // s034
        m_remove->set_selected(m_f.remove == core::Remove::Lens ? 1u : m_f.remove == core::Remove::Motion ? 2u : 0u);
        for (auto* w : m_motion_only) w->set_visible(m_f.remove == core::Remove::Motion);
    }
    m_syncing = false;
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
