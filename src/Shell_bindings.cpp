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
    m_doc.set_on_change([this] { m_layers.queue_repaint(); queue_render(); });
    m_canvas.set_on_zoom([this](double z) {
        m_status.set_zoom(std::to_string(static_cast<int>(z * 100 + 0.5)) + "%");
    });
    m_journal.set_selection_source([this] { return m_layers.selection(); });
    m_journal.set_on_change([this] { update_undo_ui(); m_history.repaint(); });
    m_history.set_on_go(sigc::mem_fun(*this, &Shell::on_history_go));
    m_layers.set_on_change([this] { on_selection(); });
    load_demo();

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
            if (act) {
                if (auto lg = log::get(log::Area::Shell)) lg->info("window key -> {}", act);
                activate_action(act);   // the FULL name: "undo" alone would do nothing
                return true;
            }
            if (mods != NONE) return false;
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

    update_undo_ui();
    update_layer_actions();
}

}  // namespace klimt
