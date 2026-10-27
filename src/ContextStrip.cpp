#include "ContextStrip.hpp"

#include <glibmm/markup.h>
#include <gtkmm/image.h>
#include <gtkmm/label.h>
#include <gtkmm/separator.h>
#include <gtkmm/togglebutton.h>

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

void ContextStrip::show_tool(const core::ToolSpec& t) {
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
                if (b->get_active()) m_variant[id] = static_cast<int>(i);
            });
            group->append(*b);
        }
        append(*group);
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
        subject->set_tooltip_text("Needs pixels (docs/pixels.md)");
        append(*subject);
        auto refine = Gtk::make_managed<Gtk::Button>("Refine Edge");
        refine->set_sensitive(false);
        refine->set_tooltip_text("Needs pixels (docs/pixels.md)");
        append(*refine);
    }
}

}  // namespace klimt
