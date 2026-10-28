#include "StylesEditor.hpp"

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
        slider(*b, "Opacity", 0, 100, "%", &m_s.drop.opacity);
        slider(*b, "Angle", -180, 180, "°", &m_s.drop.angle);
        slider(*b, "Distance", 0, 200, "px", &m_s.drop.distance);
        slider(*b, "Spread", 0, 100, "%", &m_s.drop.spread);
        slider(*b, "Size", 0, 100, "px", &m_s.drop.size);
    }
    {   // Outer Glow
        auto* b = section("Outer Glow", &m_s.outer.on);
        colour(*b, &m_s.outer.colour);
        slider(*b, "Opacity", 0, 100, "%", &m_s.outer.opacity);
        slider(*b, "Spread", 0, 100, "%", &m_s.outer.spread);
        slider(*b, "Size", 0, 100, "px", &m_s.outer.size);
    }
    {   // Inner Shadow
        auto* b = section("Inner Shadow", &m_s.inner.on);
        colour(*b, &m_s.inner.colour);
        slider(*b, "Opacity", 0, 100, "%", &m_s.inner.opacity);
        slider(*b, "Angle", -180, 180, "°", &m_s.inner.angle);
        slider(*b, "Distance", 0, 200, "px", &m_s.inner.distance);
        slider(*b, "Choke", 0, 100, "%", &m_s.inner.spread);
        slider(*b, "Size", 0, 100, "px", &m_s.inner.size);
    }
    {   // Color Overlay
        auto* b = section("Color Overlay", &m_s.overlay.on);
        colour(*b, &m_s.overlay.colour);
        slider(*b, "Opacity", 0, 100, "%", &m_s.overlay.opacity);
    }
    {   // Stroke
        auto* b = section("Stroke", &m_s.stroke.on);
        colour(*b, &m_s.stroke.colour);
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
        slider(*b, "Opacity", 0, 100, "%", &m_s.bevel.highlight_opacity);
        colour(*b, &m_s.bevel.shadow, "Shadow");
        slider(*b, "Opacity", 0, 100, "%", &m_s.bevel.shadow_opacity);
    }
    paint();
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
