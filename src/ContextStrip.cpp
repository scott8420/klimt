#include "ContextStrip.hpp"
#include "core/Patterns.hpp"   // s033: the texture's patterns

#include <glibmm/markup.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/stack.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerscroll.h>
#include <gtkmm/gesturedrag.h>
#include <gdkmm/cursor.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <cairomm/context.h>
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
    // s029: the settings panel's tabs, compact (five fit ~340 px).
    static bool styled = false;
    if (!styled) {
        styled = true;
        auto css = Gtk::CssProvider::create();
        css->load_from_data("button.klimt-tab { padding: 3px 6px; min-width: 0; }\n"
                            "box.klimt-defaults button { padding: 3px 8px; min-width: 0; }\n");
        Gtk::StyleContext::add_provider_for_display(Gdk::Display::get_default(), css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
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

// ── s021: brush presets ─────────────────────────────────────────────────────
namespace {
core::BrushPreset preset_of(const ContextStrip::BrushOpts& o) {
    core::BrushPreset p;
    p.size = o.size; p.hardness = o.hardness; p.opacity = o.opacity; p.flow = o.flow; p.smoothing = o.smoothing;
    p.tip = o.tip; p.angle = o.angle; p.roundness = o.roundness; p.strength = o.strength;
    p.pressure_size = o.pressure_size; p.pressure_opacity = o.pressure_opacity; p.tilt = o.tilt;
    p.size_jitter = o.size_jitter; p.min_size = o.min_size; p.angle_jitter = o.angle_jitter;   // s028
    p.roundness_jitter = o.roundness_jitter; p.min_roundness = o.min_roundness;
    p.scatter = o.scatter; p.count = o.count; p.count_jitter = o.count_jitter; p.both_axes = o.both_axes;
    p.fg_bg = o.fg_bg; p.hue = o.hue; p.sat = o.sat; p.bri = o.bri; p.per_tip = o.per_tip;
    p.tex_on = o.tex_on; p.tex_pattern = o.tex_pattern; p.tex_scale = o.tex_scale; p.tex_depth = o.tex_depth;   // s033
    p.tex_subtract = o.tex_subtract; p.tex_invert = o.tex_invert;
    return core::clamped(p);
}
ContextStrip::BrushOpts opts_of(const core::BrushPreset& p) {
    ContextStrip::BrushOpts o;
    o.size = p.size; o.hardness = p.hardness; o.opacity = p.opacity; o.flow = p.flow; o.smoothing = p.smoothing;
    o.tip = p.tip; o.angle = p.angle; o.roundness = p.roundness; o.strength = p.strength;
    o.pressure_size = p.pressure_size; o.pressure_opacity = p.pressure_opacity; o.tilt = p.tilt;
    o.size_jitter = p.size_jitter; o.min_size = p.min_size; o.angle_jitter = p.angle_jitter;   // s028
    o.roundness_jitter = p.roundness_jitter; o.min_roundness = p.min_roundness;
    o.scatter = p.scatter; o.count = p.count; o.count_jitter = p.count_jitter; o.both_axes = p.both_axes;
    o.fg_bg = p.fg_bg; o.hue = p.hue; o.sat = p.sat; o.bri = p.bri; o.per_tip = p.per_tip;
    o.tex_on = p.tex_on; o.tex_pattern = p.tex_pattern; o.tex_scale = p.tex_scale; o.tex_depth = p.tex_depth;   // s033
    o.tex_subtract = p.tex_subtract; o.tex_invert = p.tex_invert;
    return o;
}
// The preset's tip, drawn: its shape, hardness, angle, roundness, in the
// theme's foreground.
void draw_tip(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h, const core::BrushPreset& p, const Gdk::RGBA& fg) {
    const double r = std::min(w, h) * 0.42;
    cr->save();
    cr->translate(w / 2.0, h / 2.0);
    cr->rotate(-p.angle * M_PI / 180.0);
    cr->scale(1.0, std::max(0.05, p.roundness / 100.0));
    const double hard = p.hardness / 100.0;
    if (p.tip == 1) {   // square
        cr->set_source_rgba(fg.get_red(), fg.get_green(), fg.get_blue(), 0.95);
        cr->rectangle(-r * 0.85, -r * 0.85, r * 1.7, r * 1.7);
        cr->fill();
    } else {
        auto g = Cairo::RadialGradient::create(0, 0, 0, 0, 0, r);
        g->add_color_stop_rgba(0, fg.get_red(), fg.get_green(), fg.get_blue(), 0.95);
        g->add_color_stop_rgba(std::clamp(hard * 0.95, 0.0, 0.95), fg.get_red(), fg.get_green(), fg.get_blue(), hard > 0.01 ? 0.95 : 0.6);
        g->add_color_stop_rgba(1, fg.get_red(), fg.get_green(), fg.get_blue(), 0);
        cr->set_source(g);
        cr->arc(0, 0, r, 0, 2 * M_PI);
        cr->fill();
        if (p.tip == 2) {   // chalk: grain punched out
            cr->set_operator(Cairo::Context::Operator::CLEAR);
            unsigned seed = 7;
            for (int k = 0; k < 40; ++k) {
                seed = seed * 1103515245u + 12345u;
                const double a = (seed % 628) / 100.0;
                seed = seed * 1103515245u + 12345u;
                const double d = (seed % 100) / 100.0 * r;
                cr->arc(std::cos(a) * d, std::sin(a) * d, r * 0.07, 0, 2 * M_PI);
                cr->fill();
            }
        }
    }
    cr->restore();
}
}  // namespace

void ContextStrip::set_presets_file(const std::string& file) {
    m_presets_file = file;
    m_presets = core::load_presets(file);
}

void ContextStrip::store_presets() {
    if (!m_presets_file.empty()) core::save_presets(m_presets_file, m_presets);
}

void ContextStrip::apply_preset(const std::string& tool, std::size_t i) {
    if (i >= m_presets.size()) return;
    m_brush[tool] = opts_of(m_presets[i]);
    if (m_shown == tool)
        if (const auto* t = core::find_tool(tool)) Glib::signal_idle().connect_once([this, t] { show_tool(*t); });
    m_brush_signal.emit();
}

std::size_t ContextStrip::save_preset(const std::string& tool, const std::string& name) {
    auto p = preset_of(brush(tool));
    p.name = core::unique_preset_name(m_presets, name);
    m_presets.push_back(p);
    store_presets();
    return m_presets.size() - 1;
}

void ContextStrip::delete_preset(std::size_t i) {
    if (i >= m_presets.size()) return;
    m_presets.erase(m_presets.begin() + static_cast<std::ptrdiff_t>(i));
    store_presets();
}

Gtk::Widget* ContextStrip::presets_button(const std::string& tool) {
    auto mb = Gtk::make_managed<Gtk::MenuButton>();
    mb->set_name("strip.brush.presets");
    const auto cur = preset_of(brush(tool));
    std::string label = "Brushes";
    for (const auto& p : m_presets) if (p.same_brush(cur)) { label = p.name; break; }
    mb->set_label(label);
    mb->set_tooltip_text("Brush presets: pick one, or save the brush as it is now");
    auto pop = Gtk::make_managed<Gtk::Popover>();
    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(8);
    auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroll->set_max_content_height(380);
    scroll->set_propagate_natural_height(true);
    auto list = Gtk::make_managed<Gtk::ListBox>();
    list->set_name("strip.brush.presets.list");
    list->add_css_class("navigation-sidebar");
    list->set_selection_mode(Gtk::SelectionMode::NONE);
    list->set_activate_on_single_click(true);
    const Gdk::RGBA fg = mb->get_color();
    for (std::size_t i = 0; i < m_presets.size(); ++i) {
        const auto p = m_presets[i];
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        row->set_margin_top(2);
        row->set_margin_bottom(2);
        auto glyph = Gtk::make_managed<Gtk::DrawingArea>();
        glyph->set_content_width(30);
        glyph->set_content_height(30);
        glyph->set_draw_func([p, fg](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) { draw_tip(cr, w, h, p, fg); });
        row->append(*glyph);
        auto name = Gtk::make_managed<Gtk::Label>(p.name);
        name->set_xalign(0.f);
        name->set_hexpand(true);
        if (p.same_brush(cur)) name->set_markup("<b>" + Glib::Markup::escape_text(p.name) + "</b>");
        row->append(*name);
        auto size = Gtk::make_managed<Gtk::Label>(std::to_string(p.size) + " px");
        size->add_css_class("dim-label");
        size->add_css_class("numeric");
        row->append(*size);
        auto del = Gtk::make_managed<Gtk::Button>();
        del->set_icon_name("window-close-symbolic");
        del->add_css_class("flat");
        del->add_css_class("circular");
        del->set_tooltip_text("Delete “" + p.name + "”");
        del->signal_clicked().connect([this, pop, name = p.name, tool] {
            for (std::size_t k = 0; k < m_presets.size(); ++k)
                if (m_presets[k].name == name) { delete_preset(k); break; }
            pop->popdown();
            if (const auto* t = core::find_tool(tool)) Glib::signal_idle().connect_once([this, t] { show_tool(*t); });
        });
        row->append(*del);
        list->append(*row);
    }
    list->signal_row_activated().connect([this, pop, tool](Gtk::ListBoxRow* r) {
        if (!r) return;
        pop->popdown();
        apply_preset(tool, static_cast<std::size_t>(r->get_index()));
    });
    scroll->set_child(*list);
    box->append(*scroll);
    if (m_presets.empty()) {
        auto none = Gtk::make_managed<Gtk::Label>("No presets -- save one below");
        none->add_css_class("dim-label");
        box->append(*none);
    }
    // Save the brush as it is now.
    auto save_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_name("strip.brush.presets.name");
    entry->set_placeholder_text("Name");
    entry->set_hexpand(true);
    auto save = Gtk::make_managed<Gtk::Button>("Save Brush");
    save->set_name("strip.brush.presets.save");
    save->set_tooltip_text("Save the brush as it is now as a preset");
    const auto do_save = [this, pop, entry, tool] {
        save_preset(tool, entry->get_text());
        pop->popdown();
        if (const auto* t = core::find_tool(tool)) Glib::signal_idle().connect_once([this, t] { show_tool(*t); });
    };
    save->signal_clicked().connect(do_save);
    entry->signal_activate().connect(do_save);
    save_row->append(*entry);
    save_row->append(*save);
    box->append(*save_row);
    pop->set_child(*box);
    mb->set_popover(*pop);
    return mb;
}

void ContextStrip::set_variant(const std::string& tool_id, int v) {
    m_variant[tool_id] = v;
    if (m_shown == tool_id)
        if (const auto* t = core::find_tool(tool_id)) show_tool(*t);
    m_variant_signal.emit(tool_id, v);
}

void ContextStrip::show_variant(const std::string& tool_id, int v) {
    if (m_shown != tool_id || v < 0 || v >= static_cast<int>(m_variant_buttons.size())) return;
    m_quiet = true;
    m_variant_buttons[static_cast<std::size_t>(v)]->set_active(true);
    m_quiet = false;
}

void ContextStrip::show_tool(const core::ToolSpec& t) {
    m_shown = t.id;
    m_variant_buttons.clear();
    while (auto* c = get_first_child()) remove(*c);

    auto name = Gtk::make_managed<Gtk::Label>();
    name->set_markup("<b>" + Glib::Markup::escape_text(t.name) + "</b>");
    name->set_tooltip_text(t.note);   // s029: the note, off the strip
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
            m_variant_buttons.push_back(b);
            if (t.id == "pen") {   // s035 (Scott): the keys, where they're found
                static const char* tips[] = {
                    "Draw: click a corner, drag a smooth point, click the first point to close  (P;  Shift+P: the next mode)",
                    "Select: pick and drag points and handles; Delete removes  (A, or hold Ctrl while drawing;  Shift+P: the next mode)",
                    "Convert: click a point -- corner <-> smooth; drag a handle alone  (hold Alt while drawing;  Shift+P: the next mode)"};
                if (i < 3) b->set_tooltip_text(tips[i]);
            }
            b->signal_toggled().connect([this, b, id = t.id, i] {
                if (!b->get_active() || m_quiet) return;
                m_variant[id] = static_cast<int>(i);
                m_variant_signal.emit(id, static_cast<int>(i));
                // A variant with its own options (the Wand's) rebuilds the
                // strip -- at idle, not inside the button's own signal.
                if (id == "select" || id == "heal")   // s015 m4: Spot / Healing differ
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

    // s029: THE SHORT STRIP. Only what changes mid-drawing stays here (the
    // presets, Size, Opacity; the Wand's Tolerance, the Gradient's Opacity);
    // everything else is in the tool's SETTINGS -- one dropdown, its options
    // on tabs (settings_panel; also right-click the tool in the rail, or the
    // canvas). The tool's note is the name's tooltip. Nothing scrolls.
    const auto with_settings = [this, &t](const std::string& label, const std::string& tip) {
        auto mb = Gtk::make_managed<Gtk::MenuButton>();
        mb->set_name("strip.settings");
        mb->set_label(label);
        mb->set_tooltip_text(tip + "  (or right-click the tool)");
        auto pop = Gtk::make_managed<Gtk::Popover>();
        pop->set_child(*settings_panel(t.id, pop, [mb, this, id = t.id] { if (is_brush_tool(id)) mb->set_label(tip_label(brush(id))); }));
        mb->set_popover(*pop);
        append(*mb);
    };
    if (is_brush_tool(t.id)) {
        const auto o = brush(t.id);
        append(*presets_button(t.id));   // s021
        with_settings(tip_label(o), "The brush: hardness, flow, smoothing, pressure, the tip, its dynamics");
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
        spin("Size", 1, 2500, o.size, "px", "The brush's diameter  ([ and ] change it)", &BrushOpts::size);
        spin("Opacity", 1, 100, o.opacity, "%", "The stroke's strength -- crossing itself doesn't build past it", &BrushOpts::opacity);
    }
    if (t.id == "gradient") {   // s012
        with_settings("Settings", "The gradient's options: reverse, to transparent");
        number_field(*this, "Opacity", 1, 100, m_gradient.opacity, "%", "The gradient's strength over what's there",
                     [this] { return m_gradient.opacity; }, [this](int v) { m_gradient.opacity = v; });
    }
    if (t.id == "crop")   // s010
        with_settings("Settings", "What happens to the pixels outside the box");
    if (t.id == "select" && variant("select") == 3) {   // s008 m3: the Magic Wand
        with_settings("Settings", "The Magic Wand: contiguous, all layers");
        number_field(*this, "Tolerance", 0, 255, m_wand.tolerance, "", "How unlike the clicked pixel a pixel may be (0..255) and still be taken",
                     [this] { return m_wand.tolerance; }, [this](int v) { m_wand.tolerance = v; });
    }

    if (t.id == "pen") {   // s035 m5: the picked point -- corner or smooth
        auto pt = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        pt->add_css_class("linked");
        auto corner = Gtk::make_managed<Gtk::Button>("Corner");
        corner->set_name("strip.pen.corner");
        corner->set_tooltip_text("The picked point becomes a corner (no handles)");
        corner->signal_clicked().connect([this] { m_pen_point.emit(false); });
        auto smooth = Gtk::make_managed<Gtk::Button>("Smooth");
        smooth->set_name("strip.pen.smooth");
        smooth->set_tooltip_text("The picked point becomes smooth (handles in line)");
        smooth->signal_clicked().connect([this] { m_pen_point.emit(true); });
        pt->append(*corner);
        pt->append(*smooth);
        append(*pt);
    }
    if (t.id == "refine") {   // s038: the Refine brush
        with_settings("Settings", "The Refine brush: size, edge, soft, the overlay -- and Save as Default");   // s039 m3
        number_field(*this, "Size", 1, 2500, brush("refine").size, "px", "The stroke's width  ([ and ] change it)",
                     [this] { return brush("refine").size; },
                     [this](int v) {
                         auto o = brush("refine");
                         if (o.size == v) return;
                         o.size = v;
                         m_brush["refine"] = o;
                         m_brush_signal.emit();
                     });
        number_field(*this, "Edge", 2, 64, m_refine.band, "px",
                     "How far your rough selection may cut INTO the subject (outside it, as loose as you like)",
                     [this] { return m_refine.band; }, [this](int v) { m_refine.band = v; });
        number_field(*this, "Soft", 0, 64, m_refine.soft, "px",
                     "Hair, fur, soft focus: within this many pixels of the edge, how much is subject is read from the colour (0: a clean edge)",
                     [this] { return m_refine.soft; }, [this](int v) { m_refine.soft = v; });
        // s039 m3: the overlay -- the unselected under red while you paint.
        auto ov = Gtk::make_managed<Gtk::ToggleButton>("Overlay");
        ov->set_name("strip.refine_overlay");
        ov->set_tooltip_text("Show what is NOT selected under a red veil (Quick Mask's) while the Refine brush is in hand -- "
                             "you see the cut, not just the ants");
        ov->set_active(m_refine.overlay);
        ov->signal_toggled().connect([this, ov] {
            if (m_refine.overlay == ov->get_active()) return;
            m_refine.overlay = ov->get_active();
            m_refine_overlay.emit();
        });
        append(*ov);
    }
    if (t.id == "select" || t.id == "refine") {   // s038: the rough selection snapped to the subject
        auto snap = Gtk::make_managed<Gtk::Button>("Refine to Edges");
        snap->set_name("strip.refine_edges");
        snap->set_action_name("win.refine-edges");
        snap->set_tooltip_text("Snap the selection -- a rough Lasso round the subject is enough -- to the subject's real edge  (Ctrl+Alt+R). "
                               "Then the Refine brush (W): paint what belongs, Alt-paint what doesn't");
        append(*snap);
    }
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
    }
}


// ── s029: the tool's settings, on tabs ──────────────────────────────────────

bool ContextStrip::is_brush_tool(const std::string& t) {
    return t == "brush" || t == "eraser" || t == "clone" || t == "heal" || t == "smudge";
}

bool ContextStrip::has_settings(const std::string& t) {
    return is_brush_tool(t) || t == "gradient" || t == "crop" || t == "select" || t == "refine";   // s039 m3: Refine
}

std::string ContextStrip::tip_label(const BrushOpts& b) {
    static const char* kTips[] = {"Round", "Square", "Chalk"};
    std::string s = kTips[std::clamp(b.tip, 0, 2)];
    if (b.roundness < 100) s += " " + std::to_string(b.roundness) + "%";
    if (b.tilt) s += " · tilt";
    if (b.dynamics()) s += " · jitter";   // s028
    if (b.tex_on) s += " · texture";       // s033
    return s;
}

void ContextStrip::refresh_strip(const std::string& tool) {
    if (m_shown != tool) return;
    if (const auto* t = core::find_tool(tool)) Glib::signal_idle().connect_once([this, t] { if (m_shown == t->id) show_tool(*t); });
}

Gtk::Widget* ContextStrip::settings_panel(const std::string& id, Gtk::Popover* pop, std::function<void()> told) {
    const auto* spec = core::find_tool(id);
    auto outer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    outer->set_name("settings." + id);
    outer->set_margin(10);
    outer->set_size_request(300, -1);
    auto head = Gtk::make_managed<Gtk::Label>();
    head->set_markup("<b>" + Glib::Markup::escape_text(spec ? spec->name : id) + "</b>");
    head->set_xalign(0.f);
    outer->append(*head);
    if (spec && !spec->note.empty()) {
        auto n = Gtk::make_managed<Gtk::Label>(spec->note);
        n->add_css_class("dim-label");
        n->add_css_class("caption");
        n->set_wrap(true);
        n->set_max_width_chars(40);
        n->set_xalign(0.f);
        outer->append(*n);
    }
    auto stack = Gtk::make_managed<Gtk::Stack>();
    stack->set_name("settings." + id + ".pages");
    stack->set_transition_type(Gtk::StackTransitionType::NONE);
    stack->set_vhomogeneous(false);
    // The tabs: a linked row of small toggles. (Gtk::StackSwitcher, under
    // libadwaita, made five tabs 666 px wide -- the panel didn't fit beside
    // the pointer, and GTK closed it.)
    auto switcher = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    switcher->set_name("settings." + id + ".tabs");
    switcher->add_css_class("linked");
    switcher->set_homogeneous(true);
    outer->append(*switcher);
    outer->append(*stack);
    int pages = 0;
    auto first_tab = std::make_shared<Gtk::ToggleButton*>(nullptr);
    const auto page = [stack, switcher, first_tab, &pages](const char* name, const char* title) {
        auto b = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        b->set_margin_top(4);
        stack->add(*b, name, title);
        auto t = Gtk::make_managed<Gtk::ToggleButton>(title);
        t->add_css_class("klimt-tab");
        if (*first_tab) t->set_group(**first_tab); else { *first_tab = t; t->set_active(true); }
        t->signal_toggled().connect([t, stack, n = std::string(name)] { if (t->get_active()) stack->set_visible_child(n); });
        switcher->append(*t);
        ++pages;
        return b;
    };
    // Changes reach the brush, the strip's label, the outline at once.
    const auto changed = [this, told] { if (told) told(); m_brush_signal.emit(); };
    // Values set from outside the fields (Flat 45°, No Dynamics, the
    // defaults): close, and show the strip again.
    const auto close_and_refresh = [this, pop, id] { if (pop) pop->popdown(); refresh_strip(id); };
    const auto field = [](Gtk::Box* into, const char* label, int lo, int hi, int value, const char* unit, const char* tip,
                          std::function<int()> get, std::function<void(int)> put) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        number_field(*row, label, lo, hi, value, unit, tip, std::move(get), std::move(put));
        if (auto* bx = dynamic_cast<Gtk::Box*>(row->get_first_child())) {   // labels left, numbers right
            if (auto* lab = dynamic_cast<Gtk::Label*>(bx->get_first_child())) { lab->set_hexpand(true); lab->set_xalign(0.f); }
            bx->set_hexpand(true);
        }
        into->append(*row);
    };
    const auto check = [](Gtk::Box* into, const char* label, const char* tip, bool on, std::function<void(bool)> put) {
        auto c = Gtk::make_managed<Gtk::CheckButton>(label);
        c->set_active(on);
        c->set_tooltip_text(tip);
        c->signal_toggled().connect([c, put] { put(c->get_active()); });
        into->append(*c);
    };
    const auto note = [](Gtk::Box* into, const char* text) {
        auto l = Gtk::make_managed<Gtk::Label>(text);
        l->add_css_class("dim-label");
        l->add_css_class("caption");
        l->set_wrap(true);
        l->set_max_width_chars(40);
        l->set_xalign(0.f);
        into->append(*l);
    };

    if (is_brush_tool(id)) {
        const auto o = brush(id);
        const auto bf = [this, id, changed, field](Gtk::Box* into, const char* label, int lo, int hi, const char* unit, const char* tip,
                                                   int BrushOpts::*f) {
            field(into, label, lo, hi, brush(id).*f, unit, tip, [this, id, f] { return brush(id).*f; },
                  [this, id, f, changed](int v) {
                      auto x = brush(id);
                      if (x.*f == v) return;
                      x.*f = v; m_brush[id] = x; changed();
                  });
        };
        const auto bb = [this, id, changed, check](Gtk::Box* into, const char* label, const char* tip, bool BrushOpts::*f) {
            check(into, label, tip, brush(id).*f, [this, id, f, changed](bool v) { auto x = brush(id); x.*f = v; m_brush[id] = x; changed(); });
        };
        // Brush: how it lays paint down.
        auto br = page("brush", "Brush");
        bf(br, "Hardness", 0, 100, "%", "100: a hard edge; 0: soft all the way in", &BrushOpts::hardness);
        if (id == "smudge")   // s017
            bf(br, "Strength", 0, 100, "%", "How much colour the finger carries, and how far it drags it", &BrushOpts::strength);
        bf(br, "Flow", 1, 100, "%", "How much each dab lays down: below 100, a stroke builds up where it overlaps (up to the opacity)",
           &BrushOpts::flow);
        bf(br, "Smoothing", 0, 100, "%", "The stabiliser: the brush trails the pen on a string -- wobbles vanish, curves stay smooth",
           &BrushOpts::smoothing);
        bb(br, "Pressure sets the size", "A pen's pressure sets the size (light: thin)", &BrushOpts::pressure_size);
        bb(br, "Pressure sets the opacity", "A pen's pressure sets the strength (light: faint)", &BrushOpts::pressure_opacity);
        note(br, "Size and Opacity are on the strip.");
        // Tip: its shape (s017).
        auto tp = page("tip", "Tip");
        static const char* kTips[] = {"Round", "Square", "Chalk"};
        auto shapes = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
        shapes->add_css_class("linked");
        shapes->set_homogeneous(true);
        Gtk::ToggleButton* first = nullptr;
        for (int i = 0; i < 3; ++i) {
            auto b = Gtk::make_managed<Gtk::ToggleButton>(kTips[i]);
            if (first) b->set_group(*first); else first = b;
            b->set_active(o.tip == i);
            b->signal_toggled().connect([this, b, id, i, changed] {
                if (!b->get_active()) return;
                auto x = brush(id); x.tip = i; m_brush[id] = x; changed();
            });
            shapes->append(*b);
        }
        tp->append(*shapes);
        bf(tp, "Angle", -180, 180, "°", "The tip's angle, counter-clockwise", &BrushOpts::angle);
        bf(tp, "Roundness", 5, 100, "%", "100: round; low: a flat, calligraphic tip", &BrushOpts::roundness);
        auto flat = Gtk::make_managed<Gtk::Button>("Flat 45°");
        flat->set_tooltip_text("A calligraphy tip: roundness 25%, angle 45°");
        flat->add_css_class("flat");
        flat->signal_clicked().connect([this, id, changed, close_and_refresh] {
            auto x = brush(id); x.roundness = 25; x.angle = 45; m_brush[id] = x; changed(); close_and_refresh();
        });
        tp->append(*flat);
        bb(tp, "Pen tilt shapes the tip", "Lean the pen: the tip turns to face the lean and flattens the further it leans (a pencil's side)",
           &BrushOpts::tilt);
        // s028: Shape, Scatter, Colour.
        auto shape = page("shape", "Shape");
        bf(shape, "Size jitter", 0, 100, "%", "Each dab smaller by up to this much -- never larger than the brush", &BrushOpts::size_jitter);
        bf(shape, "Minimum size", 0, 100, "%", "Size jitter never takes a dab below this much of the brush", &BrushOpts::min_size);
        bf(shape, "Angle jitter", 0, 100, "%", "Each dab turned by up to this much of 180° either way", &BrushOpts::angle_jitter);
        bf(shape, "Roundness jitter", 0, 100, "%", "Each dab flattened by up to this much", &BrushOpts::roundness_jitter);
        bf(shape, "Minimum roundness", 0, 100, "%", "Roundness jitter never flattens a dab below this", &BrushOpts::min_roundness);
        note(shape, "Pen pressure still sets the size first; the jitter works on what it gives.");
        auto scat = page("scatter", "Scatter");
        bf(scat, "Scatter", 0, 1000, "%", "How far each dab lands off the path, as a share of its diameter", &BrushOpts::scatter);
        bb(scat, "Both axes", "On: dabs scatter along the path too. Off: across it only", &BrushOpts::both_axes);
        bf(scat, "Count", 1, 16, "", "Dabs laid at each step along the path", &BrushOpts::count);
        bf(scat, "Count jitter", 0, 100, "%", "Fewer dabs at a step, by up to this much", &BrushOpts::count_jitter);
        // s033: Texture -- a pattern pressed into the stroke, fixed to the picture like paper grain.
        auto tex = page("texture", "Texture");
        bb(tex, "Texture", "A pattern pressed into the stroke, fixed to the picture -- strokes over each other share the grain",
           &BrushOpts::tex_on);
        {
            auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
            auto l = Gtk::make_managed<Gtk::Label>("Pattern");
            l->add_css_class("dim-label");
            l->set_hexpand(true);
            l->set_xalign(0.f);
            row->append(*l);
            std::vector<Glib::ustring> names;
            std::vector<std::string> keys;
            for (const auto& p : core::pattern::builtin()) { names.emplace_back(p.name); keys.emplace_back(p.key); }
            for (const auto& p : core::pattern::library()) { names.emplace_back(p.name); keys.push_back("u:" + p.name); }
            auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(names));
            dd->set_name("settings.texture.pattern");
            dd->set_tooltip_text("The built-in patterns, then yours (Edit > Define Pattern)");
            for (std::size_t k = 0; k < keys.size(); ++k) if (keys[k] == o.tex_pattern) dd->set_selected(static_cast<guint>(k));
            dd->property_selected().signal_changed().connect([this, dd, keys, id, changed] {
                const auto k = dd->get_selected();
                if (k >= keys.size()) return;
                auto x = brush(id); x.tex_pattern = keys[k]; m_brush[id] = x; changed();
            });
            row->append(*dd);
            tex->append(*row);
        }
        bf(tex, "Scale", 10, 1000, "%", "The pattern's size", &BrushOpts::tex_scale);
        bf(tex, "Depth", 0, 100, "%", "How deep the grain goes: 0 none, 100 the dark parts take no paint", &BrushOpts::tex_depth);
        bb(tex, "Subtract", "On: the grain's darkness taken away (crisper grain). Off: multiplied (softer)", &BrushOpts::tex_subtract);
        bb(tex, "Invert", "The pattern's light and dark swapped", &BrushOpts::tex_invert);
        note(tex, "Strokes over each other share the grain -- it's on the picture, not on the brush.");
        if (id == "brush") {
            auto colr = page("colour", "Colour");
            bf(colr, "Fore / back", 0, 100, "%", "Each dab's colour moves toward the background colour by up to this", &BrushOpts::fg_bg);
            bf(colr, "Hue jitter", 0, 100, "%", "Each dab's hue turned by up to this much of 180° either way", &BrushOpts::hue);
            bf(colr, "Saturation jitter", 0, 100, "%", "Each dab's saturation moved by up to this, either way", &BrushOpts::sat);
            bf(colr, "Brightness jitter", 0, 100, "%", "Each dab's brightness moved by up to this, either way", &BrushOpts::bri);
            bb(colr, "Per dab", "On: every dab draws its own colour. Off: one colour for the whole stroke", &BrushOpts::per_tip);
            note(colr, "From the colour pair. On a layer mask or in Quick Mask the Brush paints without it.");
        }
        if (id == "clone" || id == "heal") {   // s015: where it takes from
            auto src = page("source", "Source");
            if (id == "clone" || variant("heal") == 1)
                check(src, "Aligned", "On: the source keeps its distance from stroke to stroke. Off: every stroke starts again from the source point",
                      m_clone.aligned, [this](bool v) { m_clone.aligned = v; m_clone_signal.emit(); });
            check(src, "All layers", "Take from what you see (every layer) -- so you can retouch onto an empty layer. Off: the selected layer only",
                  m_clone.all_layers, [this](bool v) { m_clone.all_layers = v; m_clone_signal.emit(); });
            note(src, "Alt-click (or Ctrl-click) on the canvas sets where it takes from.");
        }
        auto nodyn = Gtk::make_managed<Gtk::Button>("No Dynamics");
        nodyn->set_name("settings.no_dynamics");
        nodyn->add_css_class("flat");
        nodyn->set_tooltip_text("Turn every jitter, the scatter and the count off -- the tip stays as it is");
        nodyn->set_sensitive(o.dynamics());
        nodyn->signal_clicked().connect([this, id, changed, close_and_refresh] {
            auto x = brush(id);
            const BrushOpts plain;
            x.size_jitter = plain.size_jitter; x.min_size = plain.min_size; x.angle_jitter = plain.angle_jitter;
            x.roundness_jitter = plain.roundness_jitter; x.min_roundness = plain.min_roundness;
            x.scatter = plain.scatter; x.count = plain.count; x.count_jitter = plain.count_jitter; x.both_axes = plain.both_axes;
            x.fg_bg = plain.fg_bg; x.hue = plain.hue; x.sat = plain.sat; x.bri = plain.bri; x.per_tip = plain.per_tip;
            m_brush[id] = x;
            changed();
            close_and_refresh();
        });
        outer->append(*nodyn);
    }
    if (id == "gradient") {   // s012
        auto g = page("gradient", "Gradient");
        field(g, "Opacity", 1, 100, m_gradient.opacity, "%", "The gradient's strength over what's there",
              [this] { return m_gradient.opacity; }, [this](int v) { m_gradient.opacity = v; });
        check(g, "Reverse", "Background colour first, foreground last", m_gradient.reverse, [this](bool v) { m_gradient.reverse = v; });
        check(g, "To transparent", "From the foreground colour to nothing (not to the background colour)", m_gradient.to_transparent,
              [this](bool v) { m_gradient.to_transparent = v; });
    }
    if (id == "crop") {   // s010
        auto c = page("crop", "Crop");
        check(c, "Delete cropped pixels", "On: what's outside the box is gone. Off: it's kept beyond the canvas -- Move brings it back, "
                                          "Image > Reveal All shows it", m_crop_delete, [this](bool v) { m_crop_delete = v; });
    }
    if (id == "select") {   // s008 m3: the Magic Wand's (the select tool's only settings so far)
        auto w = page("wand", "Magic Wand");
        field(w, "Tolerance", 0, 255, m_wand.tolerance, "", "How unlike the clicked pixel a pixel may be (0..255) and still be taken",
              [this] { return m_wand.tolerance; }, [this](int v) { m_wand.tolerance = v; });
        check(w, "Contiguous", "Only pixels joined to the one clicked; off: every like pixel", m_wand.contiguous,
              [this](bool v) { m_wand.contiguous = v; });
        check(w, "All layers", "Look at what you see (every layer); off: the selected layer only", m_wand.all_layers,
              [this](bool v) { m_wand.all_layers = v; });
    }
    if (id == "refine") {   // s039 m3: the Refine brush -- so it can be saved as a default
        auto r = page("refine", "Refine Brush");
        field(r, "Size", 1, 2500, brush("refine").size, "px", "The stroke's width  ([ and ] change it)",
              [this] { return brush("refine").size; },
              [this](int v) { auto o = brush("refine"); o.size = v; m_brush["refine"] = o; m_brush_signal.emit(); });
        field(r, "Edge", 2, 64, m_refine.band, "px", "How far your rough selection may cut INTO the subject",
              [this] { return m_refine.band; }, [this](int v) { m_refine.band = v; });
        field(r, "Soft", 0, 64, m_refine.soft, "px", "Hair, fur, soft focus: the soft edge's width (0: a clean edge)",
              [this] { return m_refine.soft; }, [this](int v) { m_refine.soft = v; });
        check(r, "Overlay", "What is NOT selected under a red veil while the Refine brush is in hand", m_refine.overlay,
              [this](bool v) { if (m_refine.overlay != v) { m_refine.overlay = v; m_refine_overlay.emit(); } });
    }
    if (pages == 0) {
        auto none = Gtk::make_managed<Gtk::Label>("No settings for this tool.");
        none->add_css_class("dim-label");
        none->set_xalign(0.f);
        outer->append(*none);
    }
    // s029: the defaults -- back to them, keep these as them, or the factory's.
    if (pages > 0) {
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        row->set_name("settings." + id + ".defaults");
        row->add_css_class("klimt-defaults");
        row->set_homogeneous(true);
        auto reset = Gtk::make_managed<Gtk::Button>("Reset");
        reset->set_tooltip_text(has_saved_default(id) ? "Back to your saved defaults for this tool" : "Back to the factory settings (no defaults saved yet)");
        auto save = Gtk::make_managed<Gtk::Button>("Save as Default");
        save->set_tooltip_text("Start this tool like this from now on (and Reset comes back here)");
        auto factory = Gtk::make_managed<Gtk::Button>("Factory");
        factory->add_css_class("flat");
        factory->set_tooltip_text("The built-in settings; forgets your saved defaults for this tool");
        reset->signal_clicked().connect([this, pop, id] { if (pop) pop->popdown(); reset_tool(id); });
        save->signal_clicked().connect([this, id, save] {
            save_tool_default(id);
            save->set_label("Saved");
            Glib::signal_timeout().connect_once([save] { save->set_label("Save as Default"); }, 1200);
        });
        factory->signal_clicked().connect([this, pop, id] { if (pop) pop->popdown(); factory_tool(id); });
        row->append(*reset);
        row->append(*save);
        row->append(*factory);
        outer->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL));
        outer->append(*row);
    }
    switcher->set_visible(pages > 1);
    stack->set_visible(pages > 0);
    // Values the strip also shows (the Gradient's opacity, the Wand's
    // tolerance) follow when the panel closes.
    if (pop) pop->signal_closed().connect([this, id] { refresh_strip(id); });
    (void)close_and_refresh;
    return outer;
}


// ── s029: each tool's saved defaults ────────────────────────────────────────

namespace {
const char* group_of(const std::string& t) {
    if (t == "gradient") return "gradient";
    if (t == "crop") return "crop";
    if (t == "select") return "wand";
    if (t == "clone" || t == "heal") return "source";
    if (t == "refine") return "refine";   // s039 m3
    return nullptr;
}
int get_or(const std::map<std::string, int>& m, const char* k, int v) { const auto it = m.find(k); return it == m.end() ? v : it->second; }
}  // namespace

void ContextStrip::set_defaults_file(const std::string& file) {
    m_defaults_file = file;
    m_defaults = core::load_tool_defaults(file);
    for (const auto& [tool, p] : m_defaults.brushes) if (is_brush_tool(tool)) m_brush[tool] = opts_of(p);
    for (const char* t : {"gradient", "crop", "select", "clone", "refine"}) apply_defaults(t, false);   // s039 m3: Refine
    if (!m_shown.empty()) refresh_strip(m_shown);
    m_brush_signal.emit();
}

bool ContextStrip::has_saved_default(const std::string& t) const {
    if (m_defaults.brushes.count(t)) return true;
    const char* g = group_of(t);
    return g && m_defaults.others.count(g) && !is_brush_tool(t);
}

void ContextStrip::apply_defaults(const std::string& t, bool factory) {
    if (is_brush_tool(t)) {
        const auto it = m_defaults.brushes.find(t);
        m_brush[t] = !factory && it != m_defaults.brushes.end() ? opts_of(it->second) : BrushOpts{};
    }
    const char* g = group_of(t);
    if (!g) return;
    static const std::map<std::string, int> none;
    const auto it = m_defaults.others.find(g);
    const auto& m = !factory && it != m_defaults.others.end() ? it->second : none;
    if (t == "gradient") {
        const GradientOpts f;
        m_gradient.opacity = std::clamp(get_or(m, "opacity", f.opacity), 1, 100);
        m_gradient.reverse = get_or(m, "reverse", f.reverse) != 0;
        m_gradient.to_transparent = get_or(m, "to_transparent", f.to_transparent) != 0;
    } else if (t == "crop") {
        m_crop_delete = get_or(m, "delete", 0) != 0;
    } else if (t == "refine") {   // s039 m3
        const RefineOpts f;
        auto o = brush("refine");
        o.size = std::clamp(get_or(m, "size", BrushOpts{}.size), 1, 2500);
        m_brush["refine"] = o;
        m_refine.band = std::clamp(get_or(m, "edge", f.band), 2, 64);
        m_refine.soft = std::clamp(get_or(m, "soft", f.soft), 0, 64);
        const bool ov = get_or(m, "overlay", f.overlay) != 0;
        if (ov != m_refine.overlay) { m_refine.overlay = ov; m_refine_overlay.emit(); }
    } else if (t == "select") {
        const Wand f;
        m_wand.tolerance = std::clamp(get_or(m, "tolerance", f.tolerance), 0, 255);
        m_wand.contiguous = get_or(m, "contiguous", f.contiguous) != 0;
        m_wand.all_layers = get_or(m, "all_layers", f.all_layers) != 0;
    } else {   // clone / heal: the source options they share
        const CloneOpts f;
        m_clone.aligned = get_or(m, "aligned", f.aligned) != 0;
        m_clone.all_layers = get_or(m, "all_layers", f.all_layers) != 0;
        m_clone_signal.emit();
    }
}

void ContextStrip::reset_tool(const std::string& t) {
    apply_defaults(t, false);
    m_brush_signal.emit();
    refresh_strip(t);
}

void ContextStrip::factory_tool(const std::string& t) {
    m_defaults.brushes.erase(t);
    if (const char* g = group_of(t); g && (!is_brush_tool(t) || !m_defaults.brushes.count(t == "clone" ? "heal" : "clone")))
        m_defaults.others.erase(g);
    if (!m_defaults_file.empty()) core::save_tool_defaults(m_defaults_file, m_defaults);
    apply_defaults(t, true);
    m_brush_signal.emit();
    refresh_strip(t);
}

void ContextStrip::save_tool_default(const std::string& t) {
    if (is_brush_tool(t)) {
        auto p = preset_of(brush(t));
        p.name = t;
        m_defaults.brushes[t] = p;
    }
    if (t == "gradient")
        m_defaults.others["gradient"] = {{"opacity", m_gradient.opacity}, {"reverse", m_gradient.reverse}, {"to_transparent", m_gradient.to_transparent}};
    else if (t == "crop")
        m_defaults.others["crop"] = {{"delete", m_crop_delete}};
    else if (t == "select")
        m_defaults.others["wand"] = {{"tolerance", m_wand.tolerance}, {"contiguous", m_wand.contiguous}, {"all_layers", m_wand.all_layers}};
    else if (t == "refine")   // s039 m3
        m_defaults.others["refine"] = {{"size", brush("refine").size}, {"edge", m_refine.band}, {"soft", m_refine.soft}, {"overlay", m_refine.overlay}};
    else if (t == "clone" || t == "heal")
        m_defaults.others["source"] = {{"aligned", m_clone.aligned}, {"all_layers", m_clone.all_layers}};
    if (!m_defaults_file.empty()) core::save_tool_defaults(m_defaults_file, m_defaults);
}

}  // namespace klimt
