#include "ContextStrip.hpp"

#include <glibmm/markup.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gesturedrag.h>
#include <gdkmm/cursor.h>
#include <glibmm/main.h>

#include <algorithm>
#include <functional>
#include <cmath>
#include <memory>
#include <string>

namespace klimt {

namespace {
// s012: a COMPACT number field (the -/+ spin buttons pushed options off a
// 1280 px window): type a number, scroll over it, or drag its label sideways
// to scrub (Photoshop's way; easy with a pen).
void number_field(Gtk::Box& into, const char* label, int lo, int hi, int value, const char* unit, const char* tip,
                  std::function<int()> get, std::function<void(int)> put) {
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    auto l = Gtk::make_managed<Gtk::Label>(label);
    l->add_css_class("dim-label");
    l->set_cursor(Gdk::Cursor::create("ew-resize"));
    l->set_tooltip_text(std::string(tip) + "  (drag this label sideways, or scroll, to change it)");
    box->append(*l);
    auto e = Gtk::make_managed<Gtk::Entry>();
    e->set_width_chars(4);
    e->set_max_width_chars(5);
    e->set_alignment(1.0f);
    e->set_text(std::to_string(value) + unit);
    e->set_tooltip_text(tip);
    box->append(*e);
    const auto set = [put, lo, hi, e, unit = std::string(unit)](int v) {
        v = std::clamp(v, lo, hi);
        e->set_text(std::to_string(v) + unit);
        put(v);
    };
    const auto parse = [e, set, get] {
        try { set(std::stoi(e->get_text())); } catch (...) { set(get()); }
    };
    e->signal_activate().connect(parse);
    auto focus = Gtk::EventControllerFocus::create();
    focus->signal_leave().connect(parse);
    e->add_controller(focus);
    const auto step_of = [lo, hi](int v) { return hi - lo > 200 ? (v < 10 ? 1 : v < 50 ? 5 : v < 200 ? 10 : 50) : 1; };
    for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(l), static_cast<Gtk::Widget*>(e)}) {
        auto scroll = Gtk::EventControllerScroll::create();
        scroll->set_flags(Gtk::EventControllerScroll::Flags::VERTICAL);
        scroll->signal_scroll().connect([get, set, step_of](double, double dy) {
            const int v = get();
            set(v + (dy < 0 ? step_of(v) : -step_of(v)));
            return true;
        }, false);
        w->add_controller(scroll);
    }
    auto drag = Gtk::GestureDrag::create();
    auto from = std::make_shared<int>(value);
    drag->signal_drag_begin().connect([get, from](double, double) { *from = get(); });
    drag->signal_drag_update().connect([from, set, lo, hi](double dx, double) {
        const double per_px = std::max(0.25, (hi - lo) / 600.0);   // the whole range in ~600 px
        set(*from + static_cast<int>(std::lround(dx * per_px)));
    });
    l->add_controller(drag);
    into.append(*box);
}
}  // namespace

ContextStrip::ContextStrip() : widgets::Box("strip", Gtk::Orientation::HORIZONTAL, 8) {
    add_css_class("klimt-strip");
    set_margin_start(12);
    set_margin_end(12);
    set_margin_top(6);
    set_margin_bottom(6);
}

int ContextStrip::variant(const std::string& tool_id) const {
    const auto it = m_variant.find(tool_id);
    return it == m_variant.end() ? 0 : it->second;
}

ContextStrip::BrushOpts ContextStrip::brush(const std::string& tool) const {
    const auto it = m_brush.find(tool);
    return it == m_brush.end() ? BrushOpts{} : it->second;
}

void ContextStrip::set_brush_size(const std::string& tool, int size) {
    auto o = brush(tool);
    o.size = std::clamp(size, 1, 2500);
    m_brush[tool] = o;
    if (m_shown == tool)
        if (const auto* t = core::find_tool(tool)) show_tool(*t);
    m_brush_signal.emit();
}

void ContextStrip::show_tool(const core::ToolSpec& t) {
    m_shown = t.id;
    while (auto* c = get_first_child()) remove(*c);

    auto name = Gtk::make_managed<Gtk::Label>();
    name->set_markup("<b>" + Glib::Markup::escape_text(t.name) + "</b>");
    append(*name);

    if (!t.variants.empty()) {
        auto group = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        group->add_css_class("linked");
        Gtk::ToggleButton* first = nullptr;
        const int chosen = variant(t.id);
        for (std::size_t i = 0; i < t.variants.size(); ++i) {
            auto b = Gtk::make_managed<Gtk::ToggleButton>(t.variants[i]);
            b->set_focus_on_click(false);
            if (first) b->set_group(*first); else first = b;
            if (static_cast<int>(i) == chosen) b->set_active(true);
            b->signal_toggled().connect([this, b, id = t.id, i] {
                if (!b->get_active()) return;
                m_variant[id] = static_cast<int>(i);
                m_variant_signal.emit(id, static_cast<int>(i));
                // A variant with its own options (the Wand's) rebuilds the
                // strip -- at idle, not inside the button's own signal.
                if (id == "select")
                    if (const auto* tool = core::find_tool(id))
                        Glib::signal_idle().connect_once([this, tool] { show_tool(*tool); });
            });
            // s008: the variants that don't act yet say so instead of faking.
            if (t.id == "select" && i >= 4) {
                b->set_sensitive(false);
                b->set_tooltip_text("Arrives with a later milestone");
            }
            group->append(*b);
        }
        append(*group);
    }

    // s008 m4: the Brush's and the Eraser's options.
    if (t.id == "brush" || t.id == "eraser") {
        // s012: COMPACT number fields (the -/+ spin buttons pushed Pressure,
        // Flow and Smoothing off a 1280 px window): type a number, scroll over
        // it, or drag its label sideways to scrub (Photoshop's way; easy with a pen).
        const auto spin = [this, id = t.id](const char* label, int lo, int hi, int value, const char* unit, const char* tip,
                                            int BrushOpts::*field) {
            number_field(*this, label, lo, hi, value, unit, tip, [this, id, field] { return brush(id).*field; },
                         [this, id, field](int v) {
                             auto o = brush(id);
                             if (o.*field == v) return;
                             o.*field = v;
                             m_brush[id] = o;
                             m_brush_signal.emit();
                         });
        };
        const auto o = brush(t.id);
        spin("Size", 1, 2500, o.size, "px", "The brush's diameter  ([ and ] change it)", &BrushOpts::size);
        spin("Hardness", 0, 100, o.hardness, "%", "100: a hard edge; 0: soft all the way in", &BrushOpts::hardness);
        spin("Opacity", 1, 100, o.opacity, "%", "The stroke's strength -- crossing itself doesn't build past it", &BrushOpts::opacity);
        // s012 m1: build-up and the stabiliser.
        spin("Flow", 1, 100, o.flow, "%", "How much each dab lays down: below 100, a stroke builds up where it overlaps (up to the opacity)",
             &BrushOpts::flow);
        spin("Smoothing", 0, 100, o.smoothing, "%", "The stabiliser: the brush trails the pen on a string -- wobbles vanish, curves stay smooth",
             &BrushOpts::smoothing);
        // s009 m3: what a pen's pressure drives (a mouse has none: always full).
        auto pl = Gtk::make_managed<Gtk::Label>("Pressure");
        pl->add_css_class("dim-label");
        append(*pl);
        auto group = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        group->add_css_class("linked");
        const auto toggle = [this, group, id = t.id](const char* label, const char* tip, bool on, bool BrushOpts::*field) {
            auto b = Gtk::make_managed<Gtk::ToggleButton>(label);
            b->set_active(on);
            b->set_tooltip_text(tip);
            b->signal_toggled().connect([this, b, id, field] {
                auto o = brush(id);
                o.*field = b->get_active();
                m_brush[id] = o;
                m_brush_signal.emit();
            });
            group->append(*b);
        };
        toggle("Size", "A pen's pressure sets the size (light: thin)", o.pressure_size, &BrushOpts::pressure_size);
        toggle("Opacity", "A pen's pressure sets the strength (light: faint)", o.pressure_opacity, &BrushOpts::pressure_opacity);
        append(*group);
    }

    // s012: the Gradient -- its strength, its direction, its far end.
    if (t.id == "gradient") {
        append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
        number_field(*this, "Opacity", 1, 100, m_gradient.opacity, "%", "The gradient's strength over what's there",
                     [this] { return m_gradient.opacity; }, [this](int v) { m_gradient.opacity = v; });
        auto rev = Gtk::make_managed<Gtk::CheckButton>("Reverse");
        rev->set_active(m_gradient.reverse);
        rev->set_tooltip_text("Background colour first, foreground last");
        rev->signal_toggled().connect([this, rev] { m_gradient.reverse = rev->get_active(); });
        append(*rev);
        auto tr = Gtk::make_managed<Gtk::CheckButton>("To transparent");
        tr->set_active(m_gradient.to_transparent);
        tr->set_tooltip_text("From the foreground colour to nothing (not to the background colour)");
        tr->signal_toggled().connect([this, tr] { m_gradient.to_transparent = tr->get_active(); });
        append(*tr);
    }

    // s010: Crop -- what happens to the pixels outside the box.
    if (t.id == "crop") {
        append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
        auto del = Gtk::make_managed<Gtk::CheckButton>("Delete cropped pixels");
        del->set_active(m_crop_delete);
        del->set_tooltip_text("On: what's outside the box is gone. Off: it's kept beyond the canvas -- "
                              "Move brings it back, Image > Reveal All shows it");
        del->signal_toggled().connect([this, del] { m_crop_delete = del->get_active(); });
        append(*del);
    }

    // s008 m3: the Magic Wand's options, when it's the select variant.
    if (t.id == "select" && variant("select") == 3) {
        append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
        auto tl = Gtk::make_managed<Gtk::Label>("Tolerance");
        tl->add_css_class("dim-label");
        append(*tl);
        auto tol = Gtk::make_managed<Gtk::SpinButton>();
        tol->set_range(0, 255);
        tol->set_increments(1, 8);
        tol->set_value(m_wand.tolerance);
        tol->set_width_chars(3);
        tol->set_tooltip_text("How unlike the clicked pixel a pixel may be (0..255) and still be taken");
        tol->signal_value_changed().connect([this, tol] { m_wand.tolerance = static_cast<int>(tol->get_value()); });
        append(*tol);
        auto contig = Gtk::make_managed<Gtk::CheckButton>("Contiguous");
        contig->set_active(m_wand.contiguous);
        contig->set_tooltip_text("Only pixels joined to the one clicked; off: every like pixel");
        contig->signal_toggled().connect([this, contig] { m_wand.contiguous = contig->get_active(); });
        append(*contig);
        auto all = Gtk::make_managed<Gtk::CheckButton>("All layers");
        all->set_active(m_wand.all_layers);
        all->set_tooltip_text("Look at what you see (every layer); off: the selected layer only");
        all->signal_toggled().connect([this, all] { m_wand.all_layers = all->get_active(); });
        append(*all);
    }

    append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
    auto note = Gtk::make_managed<Gtk::Label>(t.note);
    note->add_css_class("dim-label");
    note->set_ellipsize(Pango::EllipsizeMode::END);
    note->set_xalign(0.0f);
    note->set_hexpand(true);
    note->set_tooltip_text(t.note);   // the whole note when the strip is crowded
    append(*note);

    if (t.id == "select") {
        // Klimt's strength, one click from the selection tools (docs/ui.md 4).
        // Shown, greyed, until there are pixels for them to work on.
        auto subject = Gtk::make_managed<Gtk::Button>();
        auto inner = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("klimt-subject-symbolic");
        inner->append(*icon);
        inner->append(*Gtk::make_managed<Gtk::Label>("Select Subject"));
        subject->set_child(*inner);
        subject->set_sensitive(false);
        subject->set_tooltip_text("Arrives with libs/seg (local models)");
        append(*subject);
        auto refine = Gtk::make_managed<Gtk::Button>("Refine Edge");
        refine->set_sensitive(false);
        refine->set_tooltip_text("Arrives with libs/seg (local models)");
        append(*refine);
    }
}

}  // namespace klimt
