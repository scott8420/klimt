#include "ContextStrip.hpp"

#include <glibmm/markup.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
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
    return core::clamped(p);
}
ContextStrip::BrushOpts opts_of(const core::BrushPreset& p) {
    ContextStrip::BrushOpts o;
    o.size = p.size; o.hardness = p.hardness; o.opacity = p.opacity; o.flow = p.flow; o.smoothing = p.smoothing;
    o.tip = p.tip; o.angle = p.angle; o.roundness = p.roundness; o.strength = p.strength;
    o.pressure_size = p.pressure_size; o.pressure_opacity = p.pressure_opacity; o.tilt = p.tilt;
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

    // s008 m4: the Brush's and the Eraser's options.
    if (t.id == "brush" || t.id == "eraser" || t.id == "clone" || t.id == "heal" || t.id == "smudge") {
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
        append(*presets_button(t.id));   // s021
        // s017: the TIP -- a button whose popover holds the shape, angle,
        // roundness and pen tilt (Photoshop's brush picker, cut down): the
        // strip itself stays as long as it was.
        {
            static const char* kTips[] = {"Round", "Square", "Chalk"};
            auto mb = Gtk::make_managed<Gtk::MenuButton>();
            mb->set_name("strip.brush.tip");
            const auto label_of = [](const BrushOpts& b) {
                std::string s = kTips[std::clamp(b.tip, 0, 2)];
                if (b.roundness < 100) s += " " + std::to_string(b.roundness) + "%";
                if (b.tilt) s += " · tilt";
                return s;
            };
            mb->set_label(label_of(o));
            mb->set_tooltip_text("The brush tip: its shape, angle, roundness, and the pen's tilt");
            auto pop = Gtk::make_managed<Gtk::Popover>();
            auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
            box->set_margin(10);
            auto shapes = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
            shapes->add_css_class("linked");
            Gtk::ToggleButton* first = nullptr;
            const auto changed = [this, id = t.id, mb, label_of] {
                mb->set_label(label_of(brush(id)));
                m_brush_signal.emit();
            };
            for (int i = 0; i < 3; ++i) {
                auto b = Gtk::make_managed<Gtk::ToggleButton>(kTips[i]);
                if (first) b->set_group(*first); else first = b;
                b->set_active(o.tip == i);
                b->signal_toggled().connect([this, b, id = t.id, i, changed] {
                    if (!b->get_active()) return;
                    auto x = brush(id); x.tip = i; m_brush[id] = x; changed();
                });
                shapes->append(*b);
            }
            box->append(*shapes);
            const auto field = [this, box, id = t.id, changed](const char* label, int lo, int hi, int value, const char* unit, const char* tip,
                                                               int BrushOpts::*f) {
                auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
                number_field(*row, label, lo, hi, value, unit, tip, [this, id, f] { return brush(id).*f; },
                             [this, id, f, changed](int v) {
                                 auto x = brush(id);
                                 if (x.*f == v) return;
                                 x.*f = v; m_brush[id] = x; changed();
                             });
                box->append(*row);
            };
            field("Angle", -180, 180, o.angle, "°", "The tip's angle, counter-clockwise", &BrushOpts::angle);
            field("Roundness", 5, 100, o.roundness, "%", "100: round; low: a flat, calligraphic tip", &BrushOpts::roundness);
            auto flat = Gtk::make_managed<Gtk::Button>("Flat 45°");
            flat->set_tooltip_text("A calligraphy tip: roundness 25%, angle 45°");
            flat->add_css_class("flat");
            box->append(*flat);
            auto tilt = Gtk::make_managed<Gtk::CheckButton>("Pen tilt shapes the tip");
            tilt->set_active(o.tilt);
            tilt->set_tooltip_text("Lean the pen: the tip turns to face the lean and flattens the further it leans (a pencil's side)");
            tilt->signal_toggled().connect([this, tilt, id = t.id, changed] {
                auto x = brush(id); x.tilt = tilt->get_active(); m_brush[id] = x; changed();
            });
            box->append(*tilt);
            flat->signal_clicked().connect([this, id = t.id, changed, tool = t.id] {
                auto x = brush(id); x.roundness = 25; x.angle = 45; m_brush[id] = x; changed();
                // the fields show the new values when the strip is rebuilt
                if (const auto* tt = core::find_tool(tool)) Glib::signal_idle().connect_once([this, tt] { show_tool(*tt); });
            });
            pop->set_child(*box);
            mb->set_popover(*pop);
            append(*mb);
        }
        spin("Size", 1, 2500, o.size, "px", "The brush's diameter  ([ and ] change it)", &BrushOpts::size);
        spin("Hardness", 0, 100, o.hardness, "%", "100: a hard edge; 0: soft all the way in", &BrushOpts::hardness);
        if (t.id == "smudge")   // s017
            spin("Strength", 0, 100, o.strength, "%", "How much colour the finger carries, and how far it drags it", &BrushOpts::strength);
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

    // s015: the Clone Stamp -- does the source walk with you; what it samples.
    // s015 m4: the Heal tool shares them (Spot has no source point: no Aligned).
    if (t.id == "clone" || t.id == "heal") {
        append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));
        auto al = Gtk::make_managed<Gtk::CheckButton>("Aligned");
        al->set_active(m_clone.aligned);
        al->set_tooltip_text("On: the source keeps its distance from stroke to stroke. Off: every stroke starts again from the source point");
        al->signal_toggled().connect([this, al] { m_clone.aligned = al->get_active(); m_clone_signal.emit(); });
        if (t.id == "clone" || variant("heal") == 1) append(*al);
        auto all = Gtk::make_managed<Gtk::CheckButton>("All layers");
        all->set_active(m_clone.all_layers);
        all->set_tooltip_text("Take from what you see (every layer) -- so you can retouch onto an empty layer. Off: the selected layer only");
        all->signal_toggled().connect([this, all] { m_clone.all_layers = all->get_active(); m_clone_signal.emit(); });
        append(*all);
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
