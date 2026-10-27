#include "ContextStrip.hpp"

#include <glibmm/markup.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/spinbutton.h>
#include <glibmm/main.h>

#include <algorithm>

namespace klimt {

ContextStrip::ContextStrip() : widgets::Box("strip", Gtk::Orientation::HORIZONTAL, 12) {
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
        const auto spin = [this, id = t.id](const char* label, int lo, int hi, int value, const char* unit, const char* tip,
                                            int BrushOpts::*field) {
            auto l = Gtk::make_managed<Gtk::Label>(label);
            l->add_css_class("dim-label");
            append(*l);
            auto sb = Gtk::make_managed<Gtk::SpinButton>();
            sb->set_range(lo, hi);
            sb->set_increments(1, 10);
            sb->set_value(value);
            sb->set_width_chars(4);
            sb->set_tooltip_text(tip);
            sb->signal_value_changed().connect([this, sb, id, field] {
                auto o = brush(id);
                o.*field = static_cast<int>(sb->get_value());
                m_brush[id] = o;
                m_brush_signal.emit();
            });
            append(*sb);
            if (*unit) {
                auto u = Gtk::make_managed<Gtk::Label>(unit);
                u->add_css_class("dim-label");
                append(*u);
            }
        };
        const auto o = brush(t.id);
        spin("Size", 1, 2500, o.size, "px", "The brush's diameter  ([ and ] change it)", &BrushOpts::size);
        spin("Hardness", 0, 100, o.hardness, "%", "100: a hard edge; 0: soft all the way in", &BrushOpts::hardness);
        spin("Opacity", 1, 100, o.opacity, "%", "The stroke's strength -- crossing itself doesn't build past it", &BrushOpts::opacity);
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
