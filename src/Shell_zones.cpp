#include "Shell.hpp"
#include "Log.hpp"

#include <adwaita.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/overlay.h>
#include <gtkmm/paned.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gtkmm/scrolledwindow.h>

#include <algorithm>
#include <gtkmm/separator.h>
#include <gtkmm/stackswitcher.h>
#include <gtkmm/styleprovider.h>
#include <gdkmm/display.h>

// Shell_zones.cpp -- ZONES. The window's frame (docs/ui.md), the dock, and the
// menu model.

namespace klimt {

void Shell::build_shell() {  // zone: header + rail | strip/canvas | dock + status
    set_title("Klimt");
    set_default_size(1280, 800);

    // A little CSS for the regions' edges (the rest is libadwaita's look).
    auto css = Gtk::CssProvider::create();
    css->load_from_data(
        ".klimt-rail { padding: 0; }\n"
        ".klimt-chrome { background-color: alpha(@window_fg_color, 0.035); }\n"
        ".klimt-edge-bottom { border-bottom: 1px solid alpha(@borders, 0.9); }\n"
        ".klimt-edge-top { border-top: 1px solid alpha(@borders, 0.9); }\n"
        ".klimt-edge-right { border-right: 1px solid alpha(@borders, 0.9); }\n"
        // The hiding tabs -- Sudoku's drawer tab, in libadwaita's colours:
        // invisible until hovered, always present and clickable (CSS only).
        ".klimt-edge-tab-left, .klimt-edge-tab-right {"
        "  background-color: alpha(@headerbar_bg_color, 0.92); color: @window_fg_color;"
        "  border: 1px solid alpha(@borders, 0.9); min-width: 18px; min-height: 160px;"
        "  font-size: 15px; opacity: 0; transition: opacity 160ms ease; }\n"
        ".klimt-edge-tab-left { border-left: none; border-radius: 0 10px 10px 0;"
        "  box-shadow: 2px 0 8px alpha(black, 0.3); }\n"
        ".klimt-edge-tab-right { border-right: none; border-radius: 10px 0 0 10px;"
        "  box-shadow: -2px 0 8px alpha(black, 0.3); }\n"
        ".klimt-edge-tab-left:hover, .klimt-edge-tab-right:hover { opacity: 1; }\n");
    if (auto display = Gdk::Display::get_default())
        Gtk::StyleContext::add_provider_for_display(display, css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // ── the header: the app mark | the document | undo, redo, Export, menu ──
    auto header = Gtk::make_managed<Gtk::HeaderBar>();
    m_logo_button.set_icon_name("klimt-logo-symbolic");
    m_logo_button.set_has_frame(false);
    m_logo_button.set_tooltip_text("About Klimt");
    m_logo_button.set_action_name("win.about");
    header->pack_start(m_logo_button);
    // s007: Open beside the mark, GNOME's place for it.
    m_open_button.set_label("Open");
    m_open_button.set_action_name("win.open");
    m_open_button.set_tooltip_text("Open an image or a Klimt document  (Ctrl+O)");
    header->pack_start(m_open_button);

    m_title = ADW_WINDOW_TITLE(adw_window_title_new("Untitled", ""));
    gtk_header_bar_set_title_widget(header->gobj(), GTK_WIDGET(m_title));

    m_menu_button.set_icon_name("open-menu-symbolic");
    m_menu_button.set_menu_model(build_menu());
    m_menu_button.set_tooltip_text("Main menu");
    header->pack_end(m_menu_button);
    m_export_button.set_label("Export");
    m_export_button.add_css_class("suggested-action");
    m_export_button.set_action_name("win.export");
    m_export_button.set_tooltip_text("Export a flat PNG, JPEG or TIFF  (Ctrl+Shift+E)");
    header->pack_end(m_export_button);
    m_redo_button.set_icon_name("edit-redo-symbolic");
    m_redo_button.set_action_name("win.redo");
    header->pack_end(m_redo_button);
    m_undo_button.set_icon_name("edit-undo-symbolic");
    m_undo_button.set_action_name("win.undo");
    header->pack_end(m_undo_button);
    set_titlebar(*header);

    // ── the body: rail | (strip over canvas) ────────────────────────────────
    m_rail.add_css_class("klimt-chrome");
    m_rail.add_css_class("klimt-edge-right");
    m_rail.signal_tool().connect(sigc::mem_fun(*this, &Shell::on_tool));
    m_body.append(m_rail);

    auto strip_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    strip_box->add_css_class("klimt-chrome");
    strip_box->add_css_class("klimt-edge-bottom");
    // s008: the strip scrolls sideways when the window is narrower than a
    // tool's options -- it must never make the WINDOW wider (the Wand's
    // options pushed the dock off screen, seen under Xvfb).
    auto strip_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    strip_scroll->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::NEVER);
    strip_scroll->set_propagate_natural_height(true);
    strip_scroll->set_child(m_strip);
    strip_box->append(*strip_scroll);
    build_tabs(m_centre);   // s020: the document tabs, over the strip
    m_strip_box = strip_box;
    m_centre.append(*strip_box);
    m_centre.append(m_canvas);
    m_centre.set_hexpand(true);
    // The rail's hiding tab rides the CANVAS side of the rail (on the rail
    // itself it covered the tools' buttons).
    auto centre = Gtk::make_managed<Gtk::Overlay>();
    centre->set_child(m_centre);
    centre->add_overlay(m_rail_tab);
    centre->set_hexpand(true);
    m_body.append(*centre);

    // ── the dock at the right, collapsible (libadwaita's split view) ────────
    m_split = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar_position(m_split, GTK_PACK_END);
    // The dock's width is exactly m_dock_width (min = max); its tab's drag
    // changes it (s006). The fraction is moot with min = max.
    adw_overlay_split_view_set_min_sidebar_width(m_split, m_dock_width);
    adw_overlay_split_view_set_max_sidebar_width(m_split, m_dock_width);
    adw_overlay_split_view_set_sidebar_width_fraction(m_split, 1.0);
    adw_overlay_split_view_set_collapsed(m_split, FALSE);
    adw_overlay_split_view_set_enable_show_gesture(m_split, FALSE);
    adw_overlay_split_view_set_enable_hide_gesture(m_split, FALSE);
    // The content carries the dock's hiding tab at its right edge -- the
    // dock's edge while it's open, the window's edge when it's hidden.
    auto content = Gtk::make_managed<Gtk::Overlay>();
    content->set_child(m_body);
    content->add_overlay(m_dock_tab);
    m_rail_tab.signal_toggle().connect([this] { set_rail(!m_rail.get_visible()); });
    m_dock_tab.signal_toggle().connect([this] {
        set_dock(!adw_overlay_split_view_get_show_sidebar(m_split));
    });
    m_dock_tab.signal_drag_begin().connect([this] {
        if (!adw_overlay_split_view_get_show_sidebar(m_split)) set_dock(true);
        // From the dock's REAL width: its content has a minimum, and a width
        // asked for below it is not what's on screen (seen under Xvfb, s006).
        m_dock_width = std::max(m_dock_width, m_dock.get_width());
        m_drag_from = m_dock_width;
    });
    m_dock_tab.signal_drag_end().connect([this] {
        if (auto lg = log::get(log::Area::Shell)) lg->info("dock: width {}", m_dock.get_width());
    });
    m_dock_tab.signal_drag_by().connect([this](double dx) {
        // Dragging LEFT widens the dock. Clamped so neither side vanishes.
        int min_w = 0, nat = 0, b1 = 0, b2 = 0;
        m_dock.measure(Gtk::Orientation::HORIZONTAL, -1, min_w, nat, b1, b2);
        const int lo = std::max(260, min_w);
        const int w = std::clamp(static_cast<int>(m_drag_from - dx), lo, std::max(lo, get_width() / 2));
        if (w == m_dock_width) return;
        m_dock_width = w;
        adw_overlay_split_view_set_min_sidebar_width(m_split, w);
        adw_overlay_split_view_set_max_sidebar_width(m_split, w);
    });
    adw_overlay_split_view_set_content(m_split, GTK_WIDGET(content->gobj()));
    adw_overlay_split_view_set_sidebar(m_split, GTK_WIDGET(build_dock().gobj()));
    gtk_widget_set_vexpand(GTK_WIDGET(m_split), TRUE);
    m_outer.append(*Glib::wrap(GTK_WIDGET(m_split)));

    // ── the status line ─────────────────────────────────────────────────────
    auto status_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    status_box->add_css_class("klimt-chrome");
    status_box->add_css_class("klimt-edge-top");
    status_box->append(m_status);
    m_status.set_zoom("100%");
    m_status.signal_cancel().connect(sigc::mem_fun(*this, &Shell::on_job_cancel));
    m_outer.append(*status_box);

    // Toasts over everything ("Deleted “Hair”  Undo").
    m_toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(m_toasts, GTK_WIDGET(m_outer.gobj()));
    gtk_window_set_child(GTK_WINDOW(gobj()), GTK_WIDGET(m_toasts));

    // The canvas surround follows dark / light (neutral either way).
    auto* sm = adw_style_manager_get_default();
    m_canvas.set_dark(adw_style_manager_get_dark(sm));
    g_signal_connect(sm, "notify::dark", G_CALLBACK(+[](AdwStyleManager* m, GParamSpec*, gpointer self) {
        static_cast<CanvasView*>(self)->set_dark(adw_style_manager_get_dark(m));
    }), &m_canvas);
}

// The dock: ONE inspector (Properties) over the stack (Layers, Channels,
// History as tabs on one part) -- docs/ui.md 5.
Gtk::Widget& Shell::build_dock() {  // zone
    m_dock.add_css_class("klimt-chrome");
    // s013: an adjustment's controls can be tall -- Properties and the
    // tabs share the dock through a divider you can drag; each side scrolls.
    auto props = Gtk::make_managed<Gtk::ScrolledWindow>();
    props->set_name("dock.properties");
    props->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    props->set_child(m_properties);
    auto lower = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);

    auto tabs = Gtk::make_managed<Gtk::StackSwitcher>();
    tabs->set_stack(m_dock_stack);
    tabs->set_halign(Gtk::Align::CENTER);
    tabs->set_margin_top(8);
    tabs->set_margin_bottom(4);
    lower->append(*tabs);

    m_layers.set_margin_start(8);
    m_layers.set_margin_end(8);
    m_layers.set_margin_bottom(6);
    m_dock_stack.add(m_layers, "layers", "Layers");
    // s016: the saved selections (colour channels and spot inks come later).
    m_dock_stack.add(m_channels, "channels", "Channels");
    m_history.set_margin_start(8);
    m_history.set_margin_end(8);
    m_history.set_margin_bottom(8);
    m_dock_stack.add(m_history, "history", "History");
    m_dock_stack.set_vexpand(true);
    lower->append(m_dock_stack);
    auto split = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::VERTICAL);
    split->set_name("dock.split");
    split->set_start_child(*props);
    split->set_end_child(*lower);
    split->set_resize_start_child(false);
    split->set_shrink_start_child(true);
    split->set_shrink_end_child(false);
    // s014 m4: where Scott left it last time (360 the first time).
    m_prefs_file = Glib::build_filename(Glib::get_user_config_dir(), "klimt", "prefs.json");
    m_prefs = core::load_prefs(m_prefs_file);
    m_strip.set_presets_file(Glib::build_filename(Glib::get_user_config_dir(), "klimt", "brushes.json"));   // s021
    split->set_position(m_prefs.dock_split);
    split->property_position().signal_changed().connect([this, split] {
        if (!get_mapped()) return;   // GTK settling the layout, not a drag
        const int at = split->get_position();
        if (at < 120 || at == m_prefs.dock_split) return;
        m_prefs.dock_split = at;
        if (m_prefs_soon.connected()) m_prefs_soon.disconnect();
        m_prefs_soon = Glib::signal_timeout().connect([this] {
            if (!core::save_prefs(m_prefs_file, m_prefs))
                if (auto lg = log::get(log::Area::Shell)) lg->warn("prefs: couldn't write {}", m_prefs_file);
            return false;
        }, 600);
    });
    split->set_vexpand(true);
    m_dock.append(*split);
    return m_dock;
}

Glib::RefPtr<Gio::Menu> Shell::build_menu() {  // zone: hamburger model
    auto menu = Gio::Menu::create();

    // s007: the file verbs first.
    auto file = Gio::Menu::create();
    file->append("New…", "win.new");
    file->append("Open…", "win.open");
    file->append("Save", "win.save");
    file->append("Save As…", "win.save-as");
    file->append("Export…", "win.export");
    file->append("Close Tab", "win.tab-close");   // s020
    menu->append_section(file);

    m_edit_menu = Gio::Menu::create();
    m_edit_menu->append("Undo", "win.undo");
    m_edit_menu->append("Redo", "win.redo");
    m_edit_menu->append("Fade…", "win.fade");   // s020: relabelled with the step it fades
    menu->append_section(m_edit_menu);
    auto edit = Gio::Menu::create();
    edit->append("Fill with Foreground", "win.fill-fg");
    edit->append("Fill with Background", "win.fill-bg");
    edit->append("Delete Selected Pixels", "win.delete-selection");
    // s011: transforms -- the selection's pixels, or the whole layer.
    edit->append("Free Transform", "win.free-transform");
    auto tr = Gio::Menu::create();
    tr->append("Flip Horizontal", "win.flip-h");
    tr->append("Flip Vertical", "win.flip-v");
    tr->append("Rotate 90° Clockwise", "win.rotate-cw");
    tr->append("Rotate 90° Counter Clockwise", "win.rotate-ccw");
    tr->append("Rotate 180°", "win.rotate-180");
    edit->append_submenu("Transform", tr);
    menu->append_section(edit);

    auto layer = Gio::Menu::create();
    layer->append("New Layer", "win.layer-new");
    layer->append("New Group", "win.layer-new-group");
    layer->append("Duplicate", "win.layer-duplicate");
    m_dup_to_menu = Gio::Menu::create();   // s022: the other tabs, kept by sync_duplicate_to()
    layer->append_submenu("Duplicate To", m_dup_to_menu);
    layer->append("Group Layers", "win.layer-group");
    layer->append("Ungroup", "win.layer-ungroup");
    layer->append("Clip / Release", "win.layer-clip");
    layer->append("Rename", "win.layer-rename");
    layer->append("Sort by Name", "win.layer-sort");
    layer->append("Delete", "win.layer-delete");
    // s012: Layer > Layer Mask.
    auto lm = Gio::Menu::create();
    auto lm_add = Gio::Menu::create();
    lm_add->append("Reveal All", "win.mask-reveal-all");
    lm_add->append("Hide All", "win.mask-hide-all");
    lm_add->append("Reveal Selection", "win.mask-reveal-sel");
    lm_add->append("Hide Selection", "win.mask-hide-sel");
    lm->append_section(lm_add);
    auto lm_do = Gio::Menu::create();
    lm_do->append("Paint the Mask / the Pixels", "win.mask-edit");
    lm_do->append("Disable / Enable", "win.mask-toggle");
    lm_do->append("Invert", "win.mask-invert");
    lm_do->append("Apply", "win.mask-apply");
    lm_do->append("Delete", "win.mask-delete");
    lm->append_section(lm_do);
    layer->append_submenu("Layer Mask", lm);
    // s013: Layer > New Adjustment Layer.
    auto nal = Gio::Menu::create();
    for (const auto k : core::adjust::kinds())
        nal->append(std::string(core::adjust::label(k)) + "…", "win.adjust-new-" + std::string(core::adjust::key(k)));
    layer->append_submenu("New Adjustment Layer", nal);
    // s019: Layer > Layer Style.
    auto ls = Gio::Menu::create();
    auto ls_fx = Gio::Menu::create();
    ls_fx->append("Drop Shadow", "win.style-drop-shadow");
    ls_fx->append("Outer Glow", "win.style-outer-glow");
    ls_fx->append("Inner Shadow", "win.style-inner-shadow");
    ls_fx->append("Inner Glow", "win.style-inner-glow");   // s023
    ls_fx->append("Satin", "win.style-satin");   // s026
    ls_fx->append("Gradient Overlay", "win.style-gradient-overlay");   // s025
    ls_fx->append("Bevel & Emboss", "win.style-bevel-emboss");   // s021
    ls_fx->append("Color Overlay", "win.style-colour-overlay");
    ls_fx->append("Stroke", "win.style-stroke");
    ls->append_section(ls_fx);
    auto ls_do = Gio::Menu::create();
    ls_do->append("Copy Layer Style", "win.style-copy");
    ls_do->append("Paste Layer Style", "win.style-paste");
    ls_do->append("Clear Layer Style", "win.style-clear");
    ls_do->append("Rasterize Layer Style", "win.style-rasterize");
    ls->append_section(ls_do);
    layer->append_submenu("Layer Style", ls);
    // s008: the Select menu, and Fill / Delete beside Undo.
    auto sel = Gio::Menu::create();
    sel->append("All", "win.select-all");
    sel->append("Deselect", "win.deselect");
    sel->append("Inverse", "win.select-invert");
    sel->append("Feather…", "win.select-feather");
    sel->append("Edit in Quick Mask Mode", "win.quick-mask");   // s016
    sel->append("Color Range…", "win.select-range");          // s024
    sel->append("Save Selection", "win.select-save");          // s016: to the Channels tab
    auto mod = Gio::Menu::create();   // s011 m4
    mod->append("Border…", "win.select-border");
    mod->append("Smooth…", "win.select-smooth");
    mod->append("Expand…", "win.select-expand");
    mod->append("Contract…", "win.select-contract");
    sel->append_submenu("Modify", mod);
    // s009 m2: the Image menu.
    auto image = Gio::Menu::create();
    image->append("Image Size…", "win.image-size");
    image->append("Canvas Size…", "win.canvas-size");
    image->append("Crop to Selection", "win.crop-selection");
    image->append("Reveal All", "win.reveal-all");
    // s011 m3: the whole canvas turned.
    auto turn = Gio::Menu::create();
    turn->append("180°", "win.canvas-180");
    turn->append("90° Clockwise", "win.canvas-cw");
    turn->append("90° Counter Clockwise", "win.canvas-ccw");
    turn->append("Arbitrary…", "win.canvas-rotate");
    turn->append("Flip Canvas Horizontal", "win.canvas-flip-h");
    turn->append("Flip Canvas Vertical", "win.canvas-flip-v");
    image->append_submenu("Image Rotation", turn);
    // s013: Image > Adjustments -- on the pixels, for good.
    auto adj = Gio::Menu::create();
    for (const auto k : core::adjust::kinds())
        adj->append(std::string(core::adjust::label(k)) + "…", "win.adjust-apply-" + std::string(core::adjust::key(k)));
    image->append_submenu("Adjustments", adj);
    // s014: the Filter menu -- on the pixels, for good (Photoshop's submenus).
    auto filt = Gio::Menu::create();
    auto again = Gio::Menu::create();
    again->append("Last Filter", "win.filter-last");
    again->append("Last Filter's Settings…", "win.filter-last-dialog");
    filt->append_section(again);
    auto groups = Gio::Menu::create();
    for (const char* g : {"Blur", "Noise", "Sharpen", "Other"}) {
        auto gm = Gio::Menu::create();
        for (const auto k : core::filter::kinds())
            if (core::filter::group(k) == g)
                gm->append(std::string(core::filter::label(k)) + "…", "win.filter-" + std::string(core::filter::key(k)));
        groups->append_submenu(g, gm);
    }
    filt->append_section(groups);
    auto view = Gio::Menu::create();
    view->append("Zoom In", "win.zoom-in");
    view->append("Zoom Out", "win.zoom-out");
    view->append("Fit on Screen", "win.zoom-fit");
    view->append("Actual Size (100%)", "win.zoom-actual");
    view->append("Hide / Show Panels", "win.toggle-panels");
    auto sub = Gio::Menu::create();
    sub->append_submenu("Image", image);
    sub->append_submenu("Layer", layer);
    sub->append_submenu("Filter", filt);
    sub->append_submenu("Select", sel);
    sub->append_submenu("View", view);
    menu->append_section(sub);

    auto diag = Gio::Menu::create();
    diag->append("Dump registry", "win.dump-registry");
    diag->append("Slow job: Sort by Name (3 s)", "win.layer-sort-slow");
    auto diag_sec = Gio::Menu::create();
    diag_sec->append_submenu("Diagnostics", diag);
    menu->append_section(diag_sec);

    auto app = Gio::Menu::create();
    app->append("Find a Command", "win.command-palette");
    app->append("Open Demo", "win.open-demo");   // s009: the demo no longer opens at launch
    app->append("Keyboard Shortcuts", "win.shortcuts");
    app->append("About Klimt", "win.about");
    app->append("Quit", "win.quit");
    menu->append_section(app);
    return menu;
}

}  // namespace klimt
