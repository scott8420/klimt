#include "StylesEditor.hpp"
#include "core/Document.hpp"   // s025: blend_modes()
#include "core/Patterns.hpp"   // s028 m4

#include <gtkmm/checkbutton.h>
#include <gtkmm/colordialog.h>
#include <gtkmm/colordialogbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/expander.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>

#include <cmath>

namespace klimt {

StylesEditor::StylesEditor() : Gtk::Box(Gtk::Orientation::VERTICAL, 2) {
    set_name("properties.styles");
    auto head = Gtk::make_managed<Gtk::Label>();
    head->set_markup("<b>Layer Styles</b>");
    head->set_xalign(0.0f);
    append(*head);

    {   // Drop Shadow
        auto* b = section("Drop Shadow", &m_s.drop.on);
        colour(*b, &m_s.drop.colour);
        mode(*b, &m_s.drop.mode);
        slider(*b, "Opacity", 0, 100, "%", &m_s.drop.opacity);
        slider(*b, "Angle", -180, 180, "°", &m_s.drop.angle);
        slider(*b, "Distance", 0, 200, "px", &m_s.drop.distance);
        slider(*b, "Spread", 0, 100, "%", &m_s.drop.spread);
        slider(*b, "Size", 0, 100, "px", &m_s.drop.size);
    }
    {   // Outer Glow
        auto* b = section("Outer Glow", &m_s.outer.on);
        colour(*b, &m_s.outer.colour);
        mode(*b, &m_s.outer.mode);
        slider(*b, "Opacity", 0, 100, "%", &m_s.outer.opacity);
        slider(*b, "Spread", 0, 100, "%", &m_s.outer.spread);
        slider(*b, "Size", 0, 100, "px", &m_s.outer.size);
    }
    {   // Inner Shadow
        auto* b = section("Inner Shadow", &m_s.inner.on);
        colour(*b, &m_s.inner.colour);
        mode(*b, &m_s.inner.mode);
        slider(*b, "Opacity", 0, 100, "%", &m_s.inner.opacity);
        slider(*b, "Angle", -180, 180, "°", &m_s.inner.angle);
        slider(*b, "Distance", 0, 200, "px", &m_s.inner.distance);
        slider(*b, "Choke", 0, 100, "%", &m_s.inner.spread);
        slider(*b, "Size", 0, 100, "px", &m_s.inner.size);
    }
    {   // s023: Inner Glow
        auto* b = section("Inner Glow", &m_s.inner_glow.on);
        colour(*b, &m_s.inner_glow.colour);
        mode(*b, &m_s.inner_glow.mode);
        slider(*b, "Opacity", 0, 100, "%", &m_s.inner_glow.opacity);
        choice(*b, "Source", {"Edge", "Center"}, [this] { return m_s.inner_glow.center ? 1 : 0; }, [this](int v) { m_s.inner_glow.center = v == 1; });
        slider(*b, "Choke", 0, 100, "%", &m_s.inner_glow.choke);
        slider(*b, "Size", 0, 100, "px", &m_s.inner_glow.size);
    }
    {   // s026: Satin
        auto* b = section("Satin", &m_s.satin.on);
        colour(*b, &m_s.satin.colour);
        mode(*b, &m_s.satin.mode);
        slider(*b, "Opacity", 0, 100, "%", &m_s.satin.opacity);
        slider(*b, "Angle", -180, 180, "°", &m_s.satin.angle);
        slider(*b, "Distance", 1, 100, "px", &m_s.satin.distance);
        slider(*b, "Size", 0, 100, "px", &m_s.satin.size);
        choice(*b, "Invert", {"No", "Yes"}, [this] { return m_s.satin.invert ? 1 : 0; }, [this](int v) { m_s.satin.invert = v == 1; });
    }
    {   // s025: Gradient Overlay
        auto* b = section("Gradient Overlay", &m_s.gradient.on);
        colour(*b, &m_s.gradient.from, "From");
        colour(*b, &m_s.gradient.to, "To");
        mode(*b, &m_s.gradient.mode);
        slider(*b, "Opacity", 0, 100, "%", &m_s.gradient.opacity);
        choice(*b, "Style", {"Linear", "Radial"}, [this] { return m_s.gradient.style; }, [this](int v) { m_s.gradient.style = v; });
        slider(*b, "Angle", -180, 180, "°", &m_s.gradient.angle);
        slider(*b, "Scale", 10, 150, "%", &m_s.gradient.scale);
        choice(*b, "Reverse", {"No", "Yes"}, [this] { return m_s.gradient.reverse ? 1 : 0; }, [this](int v) { m_s.gradient.reverse = v == 1; });
    }
    {   // s028 m4: Pattern Overlay
        auto* b = section("Pattern Overlay", &m_s.pattern.on);
        // s033: the built-ins and YOUR patterns (Edit > Define Pattern).
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto l = Gtk::make_managed<Gtk::Label>("Pattern");
        l->set_size_request(64, -1);
        l->set_xalign(0.0f);
        l->add_css_class("dim-label");
        row->append(*l);
        m_pattern_dd = Gtk::make_managed<Gtk::DropDown>();
        m_pattern_dd->set_name("properties.styles.pattern.which");
        m_pattern_dd->set_tooltip_text("The built-in patterns, then yours (Edit > Define Pattern makes one from the selection)");
        m_pattern_dd->property_selected().signal_changed().connect([this] {
            if (m_painting || !m_pattern_dd) return;
            const auto i = m_pattern_dd->get_selected();
            if (i >= m_pattern_items.size()) return;
            const auto& it = m_pattern_items[i];
            if (it.image) { m_s.pattern.user = it.user; m_s.pattern.image = it.image; }
            else { m_s.pattern.pattern = it.builtin; m_s.pattern.user.clear(); m_s.pattern.image = nullptr; }
            emit();
        });
        m_painters.push_back([this] {
            fill_patterns();
            for (std::size_t i = 0; i < m_pattern_items.size(); ++i) {
                const auto& it = m_pattern_items[i];
                const bool match = m_s.pattern.image ? it.image == m_s.pattern.image : (!it.image && it.builtin == m_s.pattern.pattern);
                if (match) { m_pattern_dd->set_selected(static_cast<guint>(i)); break; }
            }
        });
        row->append(*m_pattern_dd);
        b->append(*row);
        mode(*b, &m_s.pattern.mode);
        slider(*b, "Opacity", 0, 100, "%", &m_s.pattern.opacity);
        slider(*b, "Scale", 10, 1000, "%", &m_s.pattern.scale);
        slider(*b, "Offset X", -200, 200, "px", &m_s.pattern.x);
        slider(*b, "Offset Y", -200, 200, "px", &m_s.pattern.y);
    }
    {   // Color Overlay
        auto* b = section("Color Overlay", &m_s.overlay.on);
        colour(*b, &m_s.overlay.colour);
        mode(*b, &m_s.overlay.mode);
        slider(*b, "Opacity", 0, 100, "%", &m_s.overlay.opacity);
    }
    {   // Stroke
        auto* b = section("Stroke", &m_s.stroke.on);
        colour(*b, &m_s.stroke.colour);
        mode(*b, &m_s.stroke.mode);
        slider(*b, "Size", 1, 100, "px", &m_s.stroke.size);
        slider(*b, "Opacity", 0, 100, "%", &m_s.stroke.opacity);
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto l = Gtk::make_managed<Gtk::Label>("Position");
        l->set_size_request(64, -1);
        l->set_xalign(0.0f);
        l->add_css_class("dim-label");
        row->append(*l);
        auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create({"Outside", "Inside", "Center"}));
        dd->set_name("properties.styles.stroke.position");
        dd->property_selected().signal_changed().connect([this, dd] {
            if (m_painting) return;
            m_s.stroke.position = static_cast<int>(std::min<guint>(dd->get_selected(), 2));
            emit();
        });
        m_painters.push_back([this, dd] { dd->set_selected(static_cast<guint>(m_s.stroke.position)); });
        row->append(*dd);
        b->append(*row);
    }
    {   // s021: Bevel & Emboss
        auto* b = section("Bevel & Emboss", &m_s.bevel.on);
        choice(*b, "Style", {"Inner Bevel", "Emboss"}, [this] { return m_s.bevel.style; }, [this](int v) { m_s.bevel.style = v; });
        slider(*b, "Depth", 1, 500, "%", &m_s.bevel.depth);
        choice(*b, "Direction", {"Up", "Down"}, [this] { return m_s.bevel.up ? 0 : 1; }, [this](int v) { m_s.bevel.up = v == 0; });
        slider(*b, "Size", 0, 100, "px", &m_s.bevel.size);
        slider(*b, "Soften", 0, 16, "px", &m_s.bevel.soften);
        slider(*b, "Angle", -180, 180, "°", &m_s.bevel.angle);
        slider(*b, "Altitude", 0, 90, "°", &m_s.bevel.altitude);
        colour(*b, &m_s.bevel.highlight, "Highlight");
        mode(*b, &m_s.bevel.highlight_mode, "Its mode");
        slider(*b, "Opacity", 0, 100, "%", &m_s.bevel.highlight_opacity);
        colour(*b, &m_s.bevel.shadow, "Shadow");
        mode(*b, &m_s.bevel.shadow_mode, "Its mode");
        slider(*b, "Opacity", 0, 100, "%", &m_s.bevel.shadow_opacity);
    }
    paint();
}

// s033: the list's items -- rebuilt only when they change (a new / removed
// library pattern, a layer whose own pattern isn't in the library).
void StylesEditor::fill_patterns() {
    if (!m_pattern_dd) return;
    std::vector<PatternItem> items;
    for (int k = 0; k < static_cast<int>(core::pattern::builtin().size()); ++k) items.push_back({k, {}, nullptr});
    bool have_own = !m_s.pattern.image;
    for (const auto& p : core::pattern::library()) {
        items.push_back({-1, p.name, p.image});
        if (p.image == m_s.pattern.image) have_own = true;
    }
    if (!have_own) items.push_back({-1, m_s.pattern.user, m_s.pattern.image});   // from the file, not in the library
    bool same = items.size() == m_pattern_items.size();
    for (std::size_t i = 0; same && i < items.size(); ++i)
        same = items[i].builtin == m_pattern_items[i].builtin && items[i].image == m_pattern_items[i].image;
    if (same) return;
    m_pattern_items = std::move(items);
    std::vector<Glib::ustring> names;
    for (const auto& it : m_pattern_items) {
        if (!it.image) names.emplace_back(core::pattern::builtin()[static_cast<std::size_t>(it.builtin)].name);
        else {
            bool in_lib = false;
            for (const auto& p : core::pattern::library()) in_lib = in_lib || p.image == it.image;
            names.emplace_back(it.user + (in_lib ? "" : " (this file)"));
        }
    }
    const bool was = m_painting;
    m_painting = true;
    m_pattern_dd->set_model(Gtk::StringList::create(names));
    m_painting = was;
}

void StylesEditor::mode(Gtk::Box& into, pixel::Blend* field, const char* label) {
    static const std::vector<pixel::Blend> modes = [] {
        std::vector<pixel::Blend> v;
        for (const auto& b : core::blend_modes()) if (b.mode != pixel::Blend::PassThrough) v.push_back(b.mode);
        return v;
    }();
    std::vector<Glib::ustring> names;
    for (const auto m : modes) names.emplace_back(std::string(core::blend_label(m)));
    choice(into, label, names,
           [field] {
               for (std::size_t i = 0; i < modes.size(); ++i) if (modes[i] == *field) return static_cast<int>(i);
               return 0;
           },
           [field](int i) { if (i >= 0 && static_cast<std::size_t>(i) < modes.size()) *field = modes[static_cast<std::size_t>(i)]; });
}

void StylesEditor::choice(Gtk::Box& into, const char* label, std::vector<Glib::ustring> items, std::function<int()> get,
                          std::function<void(int)> put) {
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->set_size_request(64, -1);
    l->set_xalign(0.0f);
    l->add_css_class("dim-label");
    row->append(*l);
    const auto n = static_cast<guint>(items.size());
    auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(items));
    dd->property_selected().signal_changed().connect([this, dd, put, n] {
        if (m_painting) return;
        put(static_cast<int>(std::min<guint>(dd->get_selected(), n - 1)));
        emit();
    });
    m_painters.push_back([dd, get] { dd->set_selected(static_cast<guint>(get())); });
    row->append(*dd);
    into.append(*row);
}

Gtk::Box* StylesEditor::section(const char* name, bool* on) {
    auto ex = Gtk::make_managed<Gtk::Expander>();
    auto head = Gtk::make_managed<Gtk::CheckButton>(name);
    head->set_tooltip_text(std::string("Show / hide the ") + name + " (the arrow opens its settings)");
    head->signal_toggled().connect([this, head, on, ex] {
        if (m_painting) return;
        *on = head->get_active();
        if (*on) ex->set_expanded(true);   // turned on: show what it does
        emit();
    });
    m_painters.push_back([this, head, on] { head->set_active(*on); });
    ex->set_label_widget(*head);
    auto body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    body->set_margin_start(18);
    body->set_margin_bottom(6);
    ex->set_child(*body);
    append(*ex);
    return body;
}

void StylesEditor::slider(Gtk::Box& into, const char* label, int lo, int hi, const char* unit, int* field) {
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->set_size_request(64, -1);
    l->set_xalign(0.0f);
    l->add_css_class("dim-label");
    row->append(*l);
    auto s = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
    s->set_range(lo, hi);
    s->set_increments(1, 10);
    s->set_digits(0);
    s->set_round_digits(0);
    s->set_draw_value(true);
    s->set_value_pos(Gtk::PositionType::RIGHT);
    s->set_hexpand(true);
    s->set_tooltip_text(std::string(label) + " (" + unit + ")");
    s->signal_value_changed().connect([this, s, field] {
        if (m_painting) return;
        *field = static_cast<int>(std::lround(s->get_value()));
        emit();
    });
    auto drag = Gtk::GestureDrag::create();   // a drag is one step: begin .. end
    drag->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    drag->signal_drag_begin().connect([this](double, double) { m_begin.emit(); });
    drag->signal_drag_end().connect([this](double, double) { m_end.emit(); });
    drag->signal_cancel().connect([this](const Gdk::EventSequence*) { m_end.emit(); });
    s->add_controller(drag);
    m_painters.push_back([s, field] { s->set_value(*field); });
    row->append(*s);
    into.append(*row);
}

void StylesEditor::colour(Gtk::Box& into, core::Rgb* field, const char* label) {
    auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->set_size_request(64, -1);
    l->set_xalign(0.0f);
    l->add_css_class("dim-label");
    row->append(*l);
    auto b = Gtk::make_managed<Gtk::ColorDialogButton>(Gtk::ColorDialog::create());
    b->property_rgba().signal_changed().connect([this, b, field] {
        if (m_painting) return;
        const auto c = b->get_rgba();
        *field = {c.get_red(), c.get_green(), c.get_blue()};
        emit();
    });
    m_painters.push_back([b, field] {
        Gdk::RGBA c;
        c.set_rgba(field->r, field->g, field->b, 1.0);
        b->set_rgba(c);
    });
    row->append(*b);
    into.append(*row);
}

void StylesEditor::set(const core::Styles& s) {
    if (s == m_s) return;
    m_s = s;
    paint();
}

void StylesEditor::paint() {
    m_painting = true;
    for (auto& p : m_painters) p();
    m_painting = false;
}

void StylesEditor::emit() { m_changed.emit(m_s); }

}  // namespace klimt
