#include "ToolRail.hpp"

#include <gtkmm/colordialog.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/window.h>
#include "core/Tools.hpp"

#include <gtkmm/separator.h>

namespace klimt {

ToolRail::ToolRail() : widgets::Box("rail", Gtk::Orientation::VERTICAL, 2) {
    add_css_class("klimt-rail");
    set_margin_top(6);
    set_margin_bottom(8);
    set_margin_start(6);
    set_margin_end(6);
    Gtk::ToggleButton* first = nullptr;
    int group = -1;
    for (const auto& t : core::tools()) {
        if (group >= 0 && t.group != group) {
            auto sep = Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::HORIZONTAL);
            sep->set_margin_top(4);
            sep->set_margin_bottom(4);
            sep->set_margin_start(6);
            sep->set_margin_end(6);
            append(*sep);
        }
        group = t.group;
        auto b = Gtk::make_managed<Gtk::ToggleButton>();
        b->set_name("rail." + t.id);
        b->set_icon_name(t.icon);
        b->add_css_class("flat");
        b->set_focus_on_click(false);
        b->set_tooltip_text(t.name + "  (" + std::string(1, static_cast<char>(t.key - 'a' + 'A')) + ")");
        b->set_size_request(38, 36);
        if (first) b->set_group(*first); else first = b;
        {   // s029: right-click -- the tool's settings, without choosing it
            auto rc = Gtk::GestureClick::create();
            rc->set_button(GDK_BUTTON_SECONDARY);
            rc->signal_pressed().connect([this, b, id = t.id](int, double, double) { m_settings_signal.emit(id, b); });
            b->add_controller(rc);
        }
        b->signal_toggled().connect([this, b, id = t.id] {
            if (!b->get_active()) return;
            m_tool = id;
            if (!m_setting) m_signal.emit(id);
        });
        m_buttons[t.id] = b;
        append(*b);
    }
    auto spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_vexpand(true);
    append(*spacer);

    // The colour pair: foreground over background (defaults black / white).
    m_colours.set_content_width(36);
    m_colours.set_content_height(36);
    m_colours.set_tooltip_text("Foreground / background colour  (X swaps, D resets)");
    m_colours.set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int, int) {
        const auto sq = [&](double x, double y, double s, RGB c) {
            cr->rectangle(x + 0.5, y + 0.5, s, s);
            cr->set_source_rgb(c.r, c.g, c.b);
            cr->fill_preserve();
            cr->set_source_rgba(0.5, 0.5, 0.5, 0.9);
            cr->set_line_width(1.0);
            cr->stroke();
        };
        sq(13, 13, 20, m_bg);   // background
        sq(2, 2, 21, m_fg);     // foreground, on top
    });
    append(m_colours);
    // s008 m3: click the foreground square (top-left) or the background one
    // to choose it.
    auto click = Gtk::GestureClick::create();
    click->signal_released().connect([this](int, double x, double y) { pick(x < 23 && y < 23); });
    m_colours.add_controller(click);

    if (first) {
        m_setting = true;
        first->set_active(true);
        m_setting = false;
    }
}

void ToolRail::pick(bool foreground) {
    auto* win = dynamic_cast<Gtk::Window*>(get_root());
    if (!win) return;
    auto dlg = Gtk::ColorDialog::create();
    dlg->set_title(foreground ? "Foreground Colour" : "Background Colour");
    dlg->set_with_alpha(false);
    const RGB c = foreground ? m_fg : m_bg;
    Gdk::RGBA initial;
    initial.set_rgba(c.r, c.g, c.b, 1.0);
    dlg->choose_rgba(*win, initial, [this, dlg, foreground](Glib::RefPtr<Gio::AsyncResult>& res) {
        Gdk::RGBA got;
        try { got = dlg->choose_rgba_finish(res); } catch (const Glib::Error&) { return; }
        const RGB v{static_cast<float>(got.get_red()), static_cast<float>(got.get_green()), static_cast<float>(got.get_blue())};
        if (foreground) set_foreground(v); else set_background(v);
    });
}

void ToolRail::set_tool(const std::string& id) {
    const auto it = m_buttons.find(id);
    if (it == m_buttons.end()) return;
    m_setting = true;
    it->second->set_active(true);
    m_setting = false;
}

}  // namespace klimt
