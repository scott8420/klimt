#pragma once
#include "widgets/Widgets.hpp"
#include "core/Document.hpp"   // the document (layer tree) + its journal
#include "core/Jobs.hpp"       // the worker pool, pumped by a Dispatcher
#include "CanvasView.hpp"
#include "ContextStrip.hpp"
#include "EdgeTab.hpp"
#include "HistoryPanel.hpp"
#include "LayersPanel.hpp"
#include "PropertiesPanel.hpp"
#include "StatusLine.hpp"
#include "ToolRail.hpp"

#include <gtkmm/applicationwindow.h>
#include <giomm/menu.h>
#include <giomm/simpleaction.h>
#include <glibmm/dispatcher.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

// libadwaita's C types (s005: there is no C++ binding; the Shell holds its
// few Adw widgets as C pointers, owned by their GTK parents).
typedef struct _AdwOverlaySplitView AdwOverlaySplitView;
typedef struct _AdwToastOverlay AdwToastOverlay;
typedef struct _AdwWindowTitle AdwWindowTitle;

// ─────────────────────────────────────────────────────────────────────────────
// Shell -- the Klimt window (s005: the frame of docs/ui.md), AND the
// split-class exemplar.
//
//   +---------------------------------------------------------------+
//   | [K]          Untitled -- 8 layers        [<-][->][Export][=]   |  header
//   +----+--------------------------------------------+-------------+
//   |    | context strip (the tool's options)         | Properties  |
//   |rail+--------------------------------------------+-------------+
//   |    |   canvas (neutral surround)                | Layers |    |
//   |    |                                            | Channels   |
//   |    |                                            | History    |
//   +----+--------------------------------------------+-------------+
//   | 8 layers · 1 selected            [job ====  Cancel]      100% |  status
//   +---------------------------------------------------------------+
//
// Each region is its own class (ToolRail, ContextStrip, CanvasView,
// PropertiesPanel, LayersPanel, StatusLine); the Shell builds the frame,
// owns the document + journal + job pool, and wires them together.
//
// Split across TUs by CATEGORY OF CODE (CANON: "the header is the index"):
//   Zones    -- the frame, the dock, the menu.          src/Shell_zones.cpp
//   Bindings -- actions, the window's keys.              src/Shell_bindings.cpp
//   Handlers -- what a user action does (on_*).          src/Shell_handlers.cpp
//   Helpers  -- shared sync: labels, greying, jobs.      src/Shell_helpers.cpp
//   Glue     -- the ctor + build_ui().                   src/Shell.cpp
//
// Finding things
//   "Where is the window laid out?"     -> Shell_zones.cpp    (build_shell)
//   "Where does Ctrl+Z / Tab / V go?"   -> Shell_bindings.cpp (the window's keys)
//   "Where does a layer verb go?"       -> LayersPanel (each one call into core::layers)
//   "Where is a job landed?"            -> Shell_helpers.cpp  (on_jobs_wake)
// ─────────────────────────────────────────────────────────────────────────────
namespace klimt {

class AboutWindow;
class CommandPalette;
class ShortcutsDialog;

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();                             // category: glue

private:
    // ── zones ──────────────────────────────────────────────────────────────
    void build_shell();                          // category: zone: header + rail | strip/canvas | dock + status
    Gtk::Widget& build_dock();                   // category: zone: Properties over Layers / Channels / History
    Glib::RefPtr<Gio::Menu> build_menu();        // category: zone: hamburger model

    // ── bindings ───────────────────────────────────────────────────────────
    void bind_actions();                         // category: bindings: actions + the window's keys

    // ── handlers ───────────────────────────────────────────────────────────
    void on_dump_registry();                     // category: handler: print the live widget tree
    void on_about();                             // category: handler: the About window
    void on_shortcuts();                         // category: handler: the keyboard reference
    void on_undo();                              // category: handler: Ctrl+Z / menu / button
    void on_redo();                              // category: handler: Ctrl+Shift+Z, Ctrl+Y / menu / button
    void on_sort(bool slow);                     // category: handler: Sort by Name -- a JOB
    void on_job_cancel();                        // category: handler: Cancel in the status line
    void on_tool(const std::string& id);         // category: handler: the rail or a tool key chose a tool
    void on_toggle_panels();                     // category: handler: Tab on the canvas
    void on_history_go(std::size_t position);    // category: handler: a History row -- walk the journal there
    void on_palette();                           // category: handler: Ctrl+K -- find a command
    void set_dock(bool open);                    // category: handler: the dock's edge tab / Tab
    void set_rail(bool open);                    // category: handler: the rail's edge tab / Tab

    // ── helpers ────────────────────────────────────────────────────────────
    void load_demo();                             // category: helper: a demo document, LOADED (not undoable)
    void on_selection();                          // category: helper: the Layers selection moved -> Properties, actions, status
    void update_layer_actions();                  // category: helper: grey the layer verbs by the selection
    void update_undo_ui();                        // category: helper: Undo / Redo say what they would do
    void update_summary();                        // category: helper: the title's subtitle + the status line
    void toast(const std::string& text, bool offer_undo = false);   // category: helper: an Adw toast
    void start_job(const std::string& label, core::Jobs::Work work);   // category: helper: submit + follow
    void on_jobs_wake();                          // category: helper: (UI thread) land finished jobs
    bool poll_jobs();                             // category: helper: the status line follows progress
    void queue_render();                          // category: helper: the document changed -> composite it (a job)
    bool focus_is_text() const;                   // category: helper: the keyboard is in a text box

    // ── the header ─────────────────────────────────────────────────────────
    widgets::Button     m_logo_button;
    widgets::MenuButton m_menu_button;
    widgets::Button     m_undo_button;
    widgets::Button     m_redo_button;
    widgets::Button     m_export_button;
    AdwWindowTitle*     m_title = nullptr;     // "Untitled" / "8 layers"

    // ── the document. Declaration order matters: the store, the journal that
    // holds a reference to it, then the panels that hold both.
    core::Document     m_doc;
    core::DocJournal   m_journal{m_doc};
    LayersPanel        m_layers{m_doc, m_journal};
    PropertiesPanel    m_properties{m_doc, m_journal};
    HistoryPanel       m_history{m_journal};

    // ── the regions ────────────────────────────────────────────────────────
    ToolRail           m_rail;
    ContextStrip       m_strip;
    CanvasView         m_canvas;
    StatusLine         m_status;
    widgets::Box       m_outer;                // vertical: split view, status
    widgets::Box       m_body;                 // horizontal: rail, centre
    widgets::Box       m_centre;               // vertical: strip, canvas
    widgets::Box       m_dock;                 // vertical: properties, stack switcher, stack
    widgets::Stack     m_dock_stack;           // Layers / Channels / History
    AdwOverlaySplitView* m_split = nullptr;    // content | dock (collapsible)
    AdwToastOverlay*     m_toasts = nullptr;
    bool               m_panels = true;        // Tab hides rail, strip and dock
    // The hiding tabs (s006): click hides / shows; the dock's drag sizes it.
    EdgeTab            m_rail_tab{"shell.rail_tab", EdgeTab::Side::Left, false};
    EdgeTab            m_dock_tab{"shell.dock_tab", EdgeTab::Side::Right, true};
    int                m_dock_width = 340;     // view state; the drag sets it
    int                m_drag_from = 340;

    Glib::RefPtr<Gio::Menu>         m_edit_menu;   // Undo / Redo items, re-labelled live
    Glib::RefPtr<Gio::SimpleAction> m_act_undo, m_act_redo;
    std::map<std::string, Glib::RefPtr<Gio::SimpleAction>> m_layer_acts;   // "layer-new" -> action

    std::unique_ptr<AboutWindow>     m_about;
    std::unique_ptr<ShortcutsDialog> m_shortcuts;
    std::unique_ptr<CommandPalette>  m_palette;   // same lifetime stone as About

    // THE UI NEVER WAITS, wired. Declared LAST so the pool is destroyed FIRST:
    // it cancels and joins its workers while the dispatcher and document live.
    std::vector<core::Jobs::Id> m_job_ids;     // jobs submitted and not yet over
    core::Jobs::Id        m_render = 0;        // the composite in flight (silent: not in the status line)
    bool                  m_render_queued = false;
    sigc::connection      m_job_poll;
    Glib::Dispatcher      m_jobs_wake;
    core::Jobs            m_jobs;
};

}  // namespace klimt
