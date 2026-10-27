#include "Shell.hpp"
#include "Log.hpp"
#include "core/Tools.hpp"

#include <gtkmm/eventcontrollerkey.h>
#include <glibmm/variant.h>

// Shell_bindings.cpp -- BINDINGS. What's wired to what: the win.* actions and
// the window's own keys. No slot bodies here -- those are handlers.

namespace klimt {

void Shell::bind_actions() {  // bindings
    add_action("dump-registry", sigc::mem_fun(*this, &Shell::on_dump_registry));
    add_action("about",         sigc::mem_fun(*this, &Shell::on_about));
    add_action("shortcuts",     sigc::mem_fun(*this, &Shell::on_shortcuts));
    add_action("quit",          [this]() { close(); });
    // s007: the file verbs (Shell_files.cpp).
    add_action("new",     sigc::mem_fun(*this, &Shell::on_new));
    add_action("open",    sigc::mem_fun(*this, &Shell::on_open));
    // s009: held so they grey while there is no image.
    m_layer_acts["save"]    = add_action("save",    sigc::mem_fun(*this, &Shell::on_save));
    m_layer_acts["save-as"] = add_action("save-as", sigc::mem_fun(*this, &Shell::on_save_as));
    m_layer_acts["export"]  = add_action("export",  sigc::mem_fun(*this, &Shell::on_export));
    add_action("open-demo", sigc::mem_fun(*this, &Shell::open_demo));   // s009: Help > Open Demo
    // s007 m4: the view (view state: not steps).
    add_action("zoom-in",     [this] { m_canvas.zoom_in(); });
    add_action("zoom-out",    [this] { m_canvas.zoom_out(); });
    add_action("zoom-fit",    [this] { m_canvas.fit(); });
    add_action("zoom-actual", [this] { m_canvas.actual_size(); });
    // s008: the selection (Shell_select.cpp). Held in m_layer_acts so they grey.
    m_layer_acts["select-all"]     = add_action("select-all", sigc::mem_fun(*this, &Shell::on_select_all));
    m_layer_acts["deselect"]       = add_action("deselect", sigc::mem_fun(*this, &Shell::on_deselect));
    m_layer_acts["select-invert"]  = add_action("select-invert", sigc::mem_fun(*this, &Shell::on_select_invert));
    m_layer_acts["fill-fg"]        = add_action("fill-fg", [this] { on_fill(true); });
    m_layer_acts["fill-bg"]        = add_action("fill-bg", [this] { on_fill(false); });
    m_layer_acts["delete-selection"] = add_action("delete-selection", sigc::mem_fun(*this, &Shell::on_delete_selection));
    m_canvas.signal_shape().connect(sigc::mem_fun(*this, &Shell::on_shape));
    m_canvas.signal_pick().connect(sigc::mem_fun(*this, &Shell::on_pick));
    m_canvas.signal_stroke().connect(sigc::mem_fun(*this, &Shell::on_stroke));
    m_canvas.signal_move().connect(sigc::mem_fun(*this, &Shell::on_move));   // s009
    m_canvas.signal_crop().connect(sigc::mem_fun(*this, &Shell::on_crop_box));
    // s009 m2: the Image menu (Shell_canvas.cpp).
    m_layer_acts["image-size"]  = add_action("image-size", sigc::mem_fun(*this, &Shell::on_image_size));
    m_layer_acts["canvas-size"] = add_action("canvas-size", sigc::mem_fun(*this, &Shell::on_canvas_size));
    m_layer_acts["crop-selection"] = add_action("crop-selection", sigc::mem_fun(*this, &Shell::on_crop_selection));
    m_strip.signal_brush_changed().connect([this] { m_canvas.set_brush_size(m_strip.brush(m_rail.tool()).size); });
    m_layer_acts["select-feather"] = add_action("select-feather", sigc::mem_fun(*this, &Shell::on_feather));
    m_strip.signal_variant().connect([this](const std::string& id, int v) {
        if (id == m_rail.tool()) m_canvas.set_tool(id, v);
    });
    add_action("toggle-panels", sigc::mem_fun(*this, &Shell::on_toggle_panels));
    add_action("command-palette", sigc::mem_fun(*this, &Shell::on_palette));
    // win.tool("brush") -- the palette's tool rows (a parameterised action).
    add_action_with_parameter("tool", Glib::Variant<Glib::ustring>::variant_type(),
        [this](const Glib::VariantBase& v) {
            const std::string id = Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(v).get();
            m_rail.set_tool(id);
            on_tool(id);
        });

    // Undo / Redo held so update_undo_ui can grey them; the layer verbs held
    // so update_layer_actions can grey them by the selection.
    m_act_undo = add_action("undo", sigc::mem_fun(*this, &Shell::on_undo));
    m_act_redo = add_action("redo", sigc::mem_fun(*this, &Shell::on_redo));
    const auto layer = [this](const char* name, void (LayersPanel::*verb)()) {
        m_layer_acts[name] = add_action(name, [this, verb] { (m_layers.*verb)(); });
    };
    layer("layer-new",       &LayersPanel::new_layer);
    layer("layer-new-group", &LayersPanel::new_group);
    layer("layer-duplicate", &LayersPanel::duplicate);
    layer("layer-group",     &LayersPanel::group);
    layer("layer-ungroup",   &LayersPanel::ungroup);
    // Delete says what it did, with Undo on the toast.
    m_layer_acts["layer-delete"] = add_action("layer-delete", [this] {
        const auto before = m_journal.size();
        const auto top = m_journal.undo_label();
        m_layers.remove();
        if (m_journal.size() != before || m_journal.undo_label() != top)
            toast(m_journal.undo_label(), true);
    });
    layer("layer-raise",     &LayersPanel::raise);
    layer("layer-lower",     &LayersPanel::lower);
    layer("layer-clip",      &LayersPanel::toggle_clip);
    layer("layer-rename",    &LayersPanel::rename_primary);
    m_layer_acts["layer-sort"] = add_action("layer-sort", [this] { on_sort(false); });
    add_action("layer-sort-slow", [this] { on_sort(true); });

    // The pool's wake is a worker's call; the Dispatcher hops it to the UI
    // thread, where pump() lands finished jobs through the journal.
    m_jobs_wake.connect(sigc::mem_fun(*this, &Shell::on_jobs_wake));
    m_jobs.set_wake([this] { m_jobs_wake.emit(); });

    // The model <-> journal <-> UI wiring. The store asks the journal whether
    // the door is open; the journal reads the panel's selection and tells the
    // UI when Undo / Redo change; the store's changes repaint the panel, whose
    // repaint (and any selection change) reaches Properties and the status.
    m_doc.set_door([this] { return m_journal.open(); });
    m_doc.set_on_change([this] { m_layers.queue_repaint(); queue_render(); queue_ants(); });
    m_canvas.set_on_zoom([this](double z) {
        m_status.set_zoom(std::to_string(static_cast<int>(z * 100 + 0.5)) + "%");
    });
    m_journal.set_selection_source([this] { return m_layers.selection(); });
    m_journal.set_on_change([this] { update_undo_ui(); m_history.repaint(); update_title(); });
    m_history.set_on_go(sigc::mem_fun(*this, &Shell::on_history_go));
    m_layers.set_on_change([this] { on_selection(); });
    // s009 (Scott, s008 -> s009, answer 3): Klimt starts with NO image -- the
    // New dialog comes up (App::on_activate -> start()); the demo is under
    // Help > Open Demo.
    m_saved_mark = m_journal.mark();
    update_title();

    // ── the window's own keys ───────────────────────────────────────────────
    // NOT accelerators: an accelerator fires before the focused widget, so it
    // would take Ctrl+Z, Tab and the tool letters from every text box. The
    // WINDOW hears them in the BUBBLE phase -- only keys nothing else took --
    // and steps aside when the focus is in a text box.
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType state) {
            const auto mods = state & (Gdk::ModifierType::CONTROL_MASK |
                                       Gdk::ModifierType::SHIFT_MASK |
                                       Gdk::ModifierType::ALT_MASK);
            const auto NONE  = Gdk::ModifierType{};
            const auto CTRL  = Gdk::ModifierType::CONTROL_MASK;
            const auto SHIFT = Gdk::ModifierType::SHIFT_MASK;
            if (focus_is_text()) return false;
            const char* act = nullptr;
            if ((keyval == GDK_KEY_z || keyval == GDK_KEY_Z) && mods == CTRL)               act = "win.undo";
            else if ((keyval == GDK_KEY_z || keyval == GDK_KEY_Z) && mods == (CTRL | SHIFT)) act = "win.redo";
            else if ((keyval == GDK_KEY_y || keyval == GDK_KEY_Y) && mods == CTRL)          act = "win.redo";
            // s008: here, not accelerators -- a text box keeps Ctrl+A,
            // Backspace and Delete for itself (and the Layers list hears its
            // own Delete first).
            else if ((keyval == GDK_KEY_a || keyval == GDK_KEY_A) && mods == CTRL)          act = "win.select-all";
            else if ((keyval == GDK_KEY_BackSpace) && mods == Gdk::ModifierType::ALT_MASK)   act = "win.fill-fg";
            else if ((keyval == GDK_KEY_BackSpace) && mods == CTRL)                          act = "win.fill-bg";
            else if ((keyval == GDK_KEY_BackSpace || keyval == GDK_KEY_Delete) && mods == NONE) act = "win.delete-selection";
            if (act) {
                if (auto lg = log::get(log::Area::Shell)) lg->info("window key -> {}", act);
                activate_action(act);   // the FULL name: "undo" alone would do nothing
                return true;
            }
            if (mods != NONE) return false;
            // s008: the colour pair -- X swaps, D resets (Photoshop's).
            if (keyval == GDK_KEY_x) { m_rail.swap_colours(); return true; }
            // s008 m4: [ and ] -- the brush's size, a step at a time.
            if ((keyval == GDK_KEY_bracketleft || keyval == GDK_KEY_bracketright) &&
                (m_rail.tool() == "brush" || m_rail.tool() == "eraser")) {
                const int s = m_strip.brush(m_rail.tool()).size;
                const int step = s < 10 ? 1 : s < 50 ? 5 : s < 200 ? 10 : 50;
                m_strip.set_brush_size(m_rail.tool(), keyval == GDK_KEY_bracketright ? s + step : s - step);
                return true;
            }
            if (keyval == GDK_KEY_d) { m_rail.reset_colours(); return true; }
            // A tool's letter (the tool registry's keys).
            const gunichar ch = gdk_keyval_to_unicode(keyval);
            if (ch < 128)
                if (const auto* t = core::tool_for_key(static_cast<char>(ch))) {
                    m_rail.set_tool(t->id);
                    on_tool(t->id);
                    return true;
                }
            return false;
        }, false);
    add_controller(keys);

    // Tab ON THE CANVAS hides the panels (Photoshop's habit). CAPTURE on the
    // canvas, so it comes before the window's focus-moving Tab; anywhere else
    // Tab still moves the keyboard between widgets.
    auto tab = Gtk::EventControllerKey::create();
    tab->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    tab->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType state) {
        if (keyval != GDK_KEY_Tab) return false;
        if ((state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK |
                      Gdk::ModifierType::ALT_MASK)) != Gdk::ModifierType{}) return false;
        on_toggle_panels();
        return true;
    }, false);
    m_canvas.add_controller(tab);

    // s009: the arrows nudge with the Move tool (1 px; Shift: 10) -- CAPTURE
    // on the canvas, before GTK moves the keyboard focus with them.
    auto arrows = Gtk::EventControllerKey::create();
    arrows->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    arrows->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType state) {
        // s009 m2: the Crop tool's box -- Enter crops, Escape drops it.
        if (m_rail.tool() == "crop" && m_canvas.has_crop()) {
            if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) { m_canvas.commit_crop(); return true; }
            if (keyval == GDK_KEY_Escape) { m_canvas.clear_crop(); return true; }
        }
        if (m_rail.tool() != "move") return false;
        const auto mods = state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK | Gdk::ModifierType::ALT_MASK);
        if (mods != Gdk::ModifierType{} && mods != Gdk::ModifierType::SHIFT_MASK) return false;
        const int step = mods == Gdk::ModifierType::SHIFT_MASK ? 10 : 1;
        int dx = 0, dy = 0;
        switch (keyval) {
            case GDK_KEY_Left:  case GDK_KEY_KP_Left:  dx = -step; break;
            case GDK_KEY_Right: case GDK_KEY_KP_Right: dx = step;  break;
            case GDK_KEY_Up:    case GDK_KEY_KP_Up:    dy = -step; break;
            case GDK_KEY_Down:  case GDK_KEY_KP_Down:  dy = step;  break;
            default: return false;
        }
        on_nudge(dx, dy);
        return true;
    }, false);
    m_canvas.add_controller(arrows);

    update_undo_ui();
    update_layer_actions();
    update_select_actions();
}

}  // namespace klimt
