#include "Shell.hpp"
#include "SizeDialog.hpp"       // s009: complete for ~unique_ptr<SizeDialog>
#include "AboutWindow.hpp"      // complete type for ~unique_ptr<AboutWindow> in ~Shell()
#include "ShortcutsDialog.hpp"  // ditto
#include "CommandPalette.hpp"   // ditto
#include "NewDialog.hpp"        // ditto
#include "NumberDialog.hpp"
#include "AdjustDialog.hpp"
#include "FilterDialog.hpp"   // s013     // ditto
#include "FadeDialog.hpp"     // s020
#include "ColourRangeDialog.hpp"   // s024
#include "Log.hpp"

// Shell.cpp -- GLUE. The ctor and the ordered build_ui() pass; every other
// method body lives in a category file (see the index in Shell.hpp).

namespace klimt {

Shell::Shell()
    : m_logo_button("shell.logo_button"),
      m_menu_button("shell.menu_button"),
      m_undo_button("shell.undo"),
      m_redo_button("shell.redo"),
      m_export_button("shell.export"),
      m_open_button("shell.open"),
      m_outer("shell.outer", Gtk::Orientation::VERTICAL),
      m_body("shell.body", Gtk::Orientation::HORIZONTAL),
      m_centre("shell.centre", Gtk::Orientation::VERTICAL),
      m_dock("shell.dock", Gtk::Orientation::VERTICAL),
      m_dock_stack("shell.dock.stack") {
    set_name("shell.window");
}

Shell::~Shell() {
    m_te_blink.disconnect();   // s046
    if (m_im) { g_signal_handlers_disconnect_by_data(m_im, this); g_object_unref(m_im); m_im = nullptr; }
    // s014 m4: a divider dragged just before quitting is still remembered.
    if (m_prefs_soon.connected()) {
        m_prefs_soon.disconnect();
        core::save_prefs(m_prefs_file, m_prefs);
    }
}

// The ordered pass: the frame, then the wiring (which loads the demo
// document), then the first paint of everything that follows the document.
void Shell::build_ui() {
    build_shell();     // zone
    bind_actions();    // bindings
    on_tool(m_rail.tool());
    update_summary();
    if (auto lg = log::get(log::Area::Shell)) lg->info("ready");
}

}  // namespace klimt
