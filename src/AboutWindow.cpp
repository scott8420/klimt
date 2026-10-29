#include "AboutWindow.hpp"
#include "widgets/Widgets.hpp"
#include "Registry.hpp"

#include <gtkmm/box.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/enums.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/stylecontext.h>
#include <gtkmmconfig.h>   // GTKMM_*_VERSION
#include <gdkmm/display.h>
#include <giomm/resource.h>
#include <glibmm/markup.h>
#include <adwaita.h>

#include <fstream>
#include <string>

namespace klimt {

namespace {

// The window's look (s027, Scott): black behind white words ALWAYS -- light
// or dark desktop -- so the painting has contrast and is easily seen. Scoped
// to this window by its class; libadwaita's look everywhere else.
void install_css() {
    static bool done = false;
    if (done) return;
    done = true;
    auto css = Gtk::CssProvider::create();
    css->load_from_data(
        "window.klimt-about, window.klimt-about headerbar {"
        "  background: #000; background-image: none; color: #fff; box-shadow: none; }\n"
        "window.klimt-about headerbar { border-bottom: 1px solid #222; }\n"
        "window.klimt-about headerbar windowcontrols button { color: #fff; }\n"
        "window.klimt-about label { color: #fff; }\n"
        "window.klimt-about label.dim-label { color: alpha(#fff, 0.72); opacity: 1; }\n"
        "window.klimt-about label.klimt-about-head {"
        "  color: alpha(#fff, 0.5); font-size: 0.8em; font-weight: bold; }\n"
        "window.klimt-about label > link { color: #9cc4ff; }\n"
        "window.klimt-about scrolledwindow, window.klimt-about viewport,"
        " window.klimt-about stack { background: none; }\n"
        // the tabs: white words, the open one underlined
        "window.klimt-about stackswitcher button {"
        "  background: none; box-shadow: none; border: none; border-radius: 0;"
        "  color: alpha(#fff, 0.55); border-bottom: 2px solid transparent;"
        "  padding: 4px 6px; min-height: 0; min-width: 0; }\n"
        "window.klimt-about stackswitcher button:hover { color: #fff; }\n"
        "window.klimt-about stackswitcher button:checked {"
        "  color: #fff; border-bottom-color: #fff; }\n"
        "window.klimt-about button.klimt-about-close {"
        "  background: none; color: #fff; border: 1px solid alpha(#fff, 0.6); }\n"
        "window.klimt-about button.klimt-about-close:hover { background: alpha(#fff, 0.14); }\n");
    if (auto display = Gdk::Display::get_default())
        Gtk::StyleContext::add_provider_for_display(display, css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

widgets::Label* text(std::string_view name, const Glib::ustring& markup, bool dim = false) {
    auto* l = Gtk::make_managed<widgets::Label>(widgets::unregistered, name);
    l->set_markup(markup);
    l->set_halign(Gtk::Align::START);
    l->set_xalign(0.0f);
    l->set_wrap(true);
    l->set_max_width_chars(36);   // keeps the column at its 340 px; words wrap
    if (dim) l->add_css_class("dim-label");
    return l;
}

widgets::Label* head(std::string_view name, const char* words) {
    auto* l = text(name, Glib::Markup::escape_text(words));
    l->add_css_class("klimt-about-head");
    l->set_margin_top(14);
    return l;
}

widgets::ScrolledWindow* scroller(std::string_view name, Gtk::Widget& child) {
    auto* sw = Gtk::make_managed<widgets::ScrolledWindow>(widgets::unregistered, name);
    sw->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    sw->set_vexpand(true);
    sw->set_child(child);
    return sw;
}

widgets::Box* column(std::string_view name) {
    auto* b = Gtk::make_managed<widgets::Box>(widgets::unregistered, name, Gtk::Orientation::VERTICAL);
    b->set_spacing(4);
    return b;
}

std::string os_name() {
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            auto v = line.substr(12);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            return v;
        }
    }
    return "unknown";
}

}  // namespace

AboutWindow::AboutWindow(Gtk::Window& parent) {
    set_name("shell.about");
    registry::add("shell.about", this);

    set_title("About Klimt");
    set_modal(true);
    set_resizable(false);
    // s027: the painting's natural size is its 1120 px; the window is sized
    // to show it at 560 tall beside a 340 px column of words.
    set_default_size(262 + 340, 560);
    set_transient_for(parent);
    set_hide_on_close(true);   // singleton: X hides, never destructs mid-session

    install_css();
    add_css_class("klimt-about");

    // Raw Gtk::HeaderBar, matching Shell's headerbar (Cairn doesn't wrap it --
    // widen on demand, and nothing addresses it).
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(true);
    set_titlebar(*hb);

    // s027: two columns -- the painting full height on the left, edge to
    // edge; on the right, four tabs over the words, Close under them.
    auto* row = Gtk::make_managed<widgets::Box>(
        widgets::unregistered, "shell.about.row", Gtk::Orientation::HORIZONTAL);

    // The app icon's painting, whole: Klimt's The Dancer (c. 1916-18, public
    // domain), from the compiled-in bundle (resources/images/, made by
    // scripts/make_app_icon.py). 1120 px tall shown at 560, so a HiDPI screen
    // gets every pixel. The width follows the painting's shape (524 x 1120).
    auto* art = Gtk::make_managed<widgets::Picture>(widgets::unregistered, "shell.about.logo");
    art->set_resource("/io/github/scott8420/Klimt/images/klimt-about-panel.jpg");
    art->set_can_shrink(true);
    art->set_content_fit(Gtk::ContentFit::COVER);
    art->set_size_request(262, 560);
    art->set_tooltip_text("Gustav Klimt, The Dancer (c. 1916-18)");
    row->append(*art);

    auto* page = Gtk::make_managed<widgets::Box>(
        widgets::unregistered, "shell.about.page", Gtk::Orientation::VERTICAL);
    page->set_spacing(12);
    page->set_margin_start(28);
    page->set_margin_end(28);
    page->set_margin_top(16);
    page->set_margin_bottom(24);
    page->set_size_request(340 - 56, -1);
    page->set_hexpand(true);

    auto* stack = Gtk::make_managed<widgets::Stack>(widgets::unregistered, "shell.about.tabs");
    stack->set_vexpand(true);
    stack->set_transition_type(Gtk::StackTransitionType::CROSSFADE);
    auto* switcher = Gtk::make_managed<widgets::StackSwitcher>(widgets::unregistered, "shell.about.switcher");
    switcher->set_stack(*stack);
    switcher->set_halign(Gtk::Align::START);
    switcher->set_hexpand(false);
    page->append(*switcher);
    page->append(*stack);

    // ── About ──────────────────────────────────────────────────────────────
    {
        auto* c = column("shell.about.about");
        c->set_valign(Gtk::Align::CENTER);
        c->append(*text("shell.about.name", "<span size='xx-large' weight='bold'>Klimt</span>"));
        c->append(*text("shell.about.tagline", "A raster image editor for GNOME.", true));
        auto* build = text("shell.about.note", "Build " KLIMT_BUILD, true);   // CMakeLists.txt KLIMT_BUILD
        build->set_margin_top(12);
        c->append(*build);
        auto* credit = text("shell.about.credit", "<i>The Dancer</i>, Gustav Klimt, c. 1916-18", true);
        credit->add_css_class("caption");
        credit->set_margin_top(24);
        c->append(*credit);
        stack->add(*c, "about", "About");
    }

    // ── Credits ────────────────────────────────────────────────────────────
    {
        auto* c = column("shell.about.credits");
        c->append(*head("shell.about.credits.by.h", "WRITTEN BY"));
        c->append(*text("shell.about.credits.by", "Scott (scott8420)\nwith Claude, by Anthropic"));
        c->append(*head("shell.about.credits.art.h", "ART"));
        c->append(*text("shell.about.credits.art",
                        "<i>The Dancer</i> (Die Tänzerin), Gustav Klimt, c. 1916-18. Public domain."));
        c->append(*head("shell.about.credits.libs.h", "BUILT WITH"));
        struct Lib { const char* name; const char* licence; };
        static constexpr Lib libs[] = {
            {"GTK 4 and gtkmm", "LGPL-2.1-or-later"},
            {"libadwaita", "LGPL-2.1-or-later"},
            {"spdlog", "MIT"},
            {"nlohmann/json", "MIT"},
            {"libpng", "PNG Reference Library License v2"},
            {"libjpeg-turbo", "IJG and BSD-3-Clause"},
            {"libtiff", "libtiff License"},
            {"libzip", "BSD-3-Clause"},
        };
        for (const auto& l : libs)
            c->append(*text("shell.about.credits.lib",
                            Glib::Markup::escape_text(l.name) + "  <span alpha='60%'>" +
                                Glib::Markup::escape_text(l.licence) + "</span>"));
        stack->add(*scroller("shell.about.credits.scroll", *c), "credits", "Credits");
    }

    // ── License -- the project's own LICENSE, from the bundle ──────────────
    {
        auto* c = column("shell.about.license");
        c->append(*text("shell.about.license.link",
                        "<a href='https://github.com/scott8420/klimt'>github.com/scott8420/klimt</a>"));
        std::string body = "LICENSE missing from the bundle.";
        try {
            auto bytes = Gio::Resource::lookup_data_global("/io/github/scott8420/Klimt/text/LICENSE");
            gsize n = 0;
            auto* p = static_cast<const char*>(bytes->get_data(n));
            body.assign(p, n);
            // the file is hard-wrapped at 80 columns; let the label wrap it
            // instead -- one line per paragraph
            std::string flowed;
            for (std::size_t i = 0; i < body.size(); ++i) {
                const char ch = body[i];
                if (ch == '\n' && i + 1 < body.size() && body[i + 1] != '\n' &&
                    i > 0 && body[i - 1] != '\n')
                    flowed += ' ';
                else
                    flowed += ch;
            }
            body = flowed;
        } catch (const Glib::Error&) {}
        auto* lic = text("shell.about.license.text", Glib::Markup::escape_text(body), true);
        lic->add_css_class("caption");
        lic->set_selectable(true);
        lic->set_margin_top(10);
        c->append(*lic);
        stack->add(*scroller("shell.about.license.scroll", *c), "license", "License");
    }

    // ── System -- filled when the window maps (fill_system) ────────────────
    {
        auto* c = column("shell.about.system");
        c->append(*head("shell.about.system.h", "FOR A BUG REPORT"));
        m_system = text("shell.about.system.text", "", false);
        m_system->set_selectable(true);
        m_system->add_css_class("monospace");
        m_system->add_css_class("caption");
        c->append(*m_system);
        stack->add(*scroller("shell.about.system.scroll", *c), "system", "System");
    }
    signal_map().connect([this] { fill_system(); });

    auto* close_btn = Gtk::make_managed<widgets::Button>(
        widgets::unregistered, "shell.about.close", "Close");
    close_btn->add_css_class("klimt-about-close");
    close_btn->set_halign(Gtk::Align::START);
    close_btn->signal_clicked().connect([this]() { close(); });
    page->append(*close_btn);

    row->append(*page);
    set_child(*row);
}

void AboutWindow::fill_system() {
    if (!m_system) return;
    std::string backend = "unknown";
    if (auto d = get_display()) {
        const std::string type = G_OBJECT_TYPE_NAME(d->gobj());   // GdkWaylandDisplay, GdkX11Display ...
        if (type.find("Wayland") != std::string::npos) backend = "Wayland";
        else if (type.find("X11") != std::string::npos) backend = "X11";
        else backend = type;
    }
    const auto v3 = [](unsigned a, unsigned b, unsigned c) {
        return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c);
    };
    std::string s;
    s += "Klimt       " KLIMT_BUILD "\n";
    s += "GTK         " + v3(gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version()) + "\n";
    s += "gtkmm       " + v3(GTKMM_MAJOR_VERSION, GTKMM_MINOR_VERSION, GTKMM_MICRO_VERSION) + "\n";
    s += "libadwaita  " + v3(adw_get_major_version(), adw_get_minor_version(), adw_get_micro_version()) + "\n";
    s += "Display     " + backend + ", scale " + std::to_string(get_scale_factor()) + "\n";
    s += "OS          " + os_name() + "\n";
    s += "Compiler    " __VERSION__;
    m_system->set_text(s);
}

AboutWindow::~AboutWindow() {
    registry::remove(this);
}

}  // namespace klimt
