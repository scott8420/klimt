#pragma once
#include "widgets/Widgets.hpp"
#include "core/Document.hpp"   // the document (layer tree) + its journal
#include "core/Jobs.hpp"       // the worker pool, pumped by a Dispatcher
#include "core/File.hpp"       // s007: documents in and out of files
#include "core/Composite.hpp"  // s007: dirty tiles
#include "core/Paint.hpp"      // s008: the brush
#include "core/Move.hpp"       // s009: the Move tool
#include "core/Canvas.hpp"     // s009: Crop, Image / Canvas Size
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

#include <cstdint>
#include <functional>
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
//   Files    -- New, Open, Save, Export, unsaved.        src/Shell_files.cpp (s007)
//   Select   -- shapes, the Select menu, Fill / Delete.  src/Shell_select.cpp (s008)
//   Paint    -- the Brush / Eraser stroke pipeline.      src/Shell_paint.cpp (s008)
//   Move     -- the Move tool's drag, the arrow nudges.  src/Shell_move.cpp (s009)
//   Canvas   -- Crop, Image Size, Canvas Size (jobs).    src/Shell_canvas.cpp (s009)
//   Glue     -- the ctor + build_ui().                   src/Shell.cpp
//
// Finding things
//   "Where is the window laid out?"     -> Shell_zones.cpp    (build_shell)
//   "Where does Ctrl+Z / Tab / V go?"   -> Shell_bindings.cpp (the window's keys)
//   "Where does a layer verb go?"       -> LayersPanel (each one call into core::layers)
//   "Where is a job landed?"            -> Shell_helpers.cpp  (on_jobs_wake)
//   "Where does Ctrl+S go?"             -> Shell_files.cpp    (on_save)
// ─────────────────────────────────────────────────────────────────────────────
namespace klimt {

class AboutWindow;
class CommandPalette;
class NewDialog;
class NumberDialog;
class ShortcutsDialog;
class SizeDialog;

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();                             // category: glue
    void open_path(const std::string& path);     // category: files: open a file (the command line, Open)
    void start();                                // category: files: launched with no file -- the New dialog (s009)

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

    // ── files (s007) ───────────────────────────────────────────────────────
    void on_new();                               // category: files: File > New (the dialog)
    void on_open();                              // category: files: File > Open (the file chooser)
    void on_save();                              // category: files: Save -- Save As when never saved
    void on_save_as();                           // category: files: Save As (.klimt)
    void on_export();                            // category: files: Export (PNG / JPEG / TIFF)
    bool on_close_request() override;            // category: files: unsaved changes ask first
    void save_to(const std::string& path, std::function<void()> then = {});   // category: files: a save JOB
    void adopt(core::file::Contents c, const std::string& path, const std::string& name);   // category: files: the document becomes c
    void ask_unsaved(const std::string& doing, std::function<void()> go);   // category: files: Save / Discard / Cancel
    bool dirty() const { return m_journal.mark() != m_saved_mark; }
    bool file_job_busy();                        // category: files: one open / save / export at a time
    void update_title();                         // category: files: the name, and a dot when unsaved

    // ── selection (s008) ───────────────────────────────────────────────────
    void on_shape(const CanvasView::Shape& shape);   // category: select: a marquee / lasso finished
    void on_select_all();                        // category: select: Ctrl+A
    void on_deselect();                          // category: select: Ctrl+D
    void on_select_invert();                     // category: select: Ctrl+Shift+I
    void on_fill(bool foreground);               // category: select: Alt / Ctrl+Backspace
    void on_delete_selection();                  // category: select: Delete / Backspace on the canvas
    void run_pixel_job(const std::string& verb, bool fill, ToolRail::RGB colour);   // category: select: Fill / Delete as a job
    void queue_ants();                           // category: select: the outline follows the selection
    void on_feather();                           // category: select: Select > Feather (asks the radius)
    void on_pick(int x, int y, bool background); // category: select: the Eyedropper
    void update_select_actions();                // category: select: grey Deselect when nothing is

    // ── paint (s008 m4) ────────────────────────────────────────────────────
    void on_stroke(CanvasView::StrokeAt at, core::select::Pt p, bool shift, double pressure);   // category: paint: the canvas's stroke
    void paint_pump();                           // category: paint: one job at a time; commit at the end

    // ── move (s009 m1) ─────────────────────────────────────────────────────
    void on_move(CanvasView::MoveAt at, int dx, int dy);   // category: move: the Move tool's drag
    void on_nudge(int dx, int dy);               // category: move: the arrow keys with the Move tool
    void move_pump();                            // category: move: one job at a time; commit at the end
    pixel::Px background_px() const;             // category: move: the background colour, the document's format

    // ── canvas (s009 m2) ───────────────────────────────────────────────────
    using CanvasPlan = std::function<std::optional<core::canvas::Reshape>(const core::canvas::Before&, core::Jobs::Token&)>;
    void run_canvas_job(const std::string& label, CanvasPlan plan);   // category: canvas: plan on a worker, land as one step
    void on_crop_box(core::select::Pt a, core::select::Pt b);          // category: canvas: the Crop tool's box, committed
    void on_crop_selection();                    // category: canvas: Image > Crop to Selection
    void on_reveal_all();                        // category: canvas: Image > Reveal All (s010)
    void on_image_size();                        // category: canvas: Image > Image Size (the dialog)
    void on_canvas_size();                       // category: canvas: Image > Canvas Size (the dialog)

    // ── helpers ────────────────────────────────────────────────────────────
    void load_demo();                             // category: helper: a demo document, LOADED (not undoable)
    void open_demo();                             // category: files: Help > Open Demo (asks about unsaved changes)
    bool has_image() const { return m_doc.image().width > 0 && m_doc.image().height > 0; }
    void on_selection();                          // category: helper: the Layers selection moved -> Properties, actions, status
    void update_layer_actions();                  // category: helper: grey the layer verbs by the selection
    void update_undo_ui();                        // category: helper: Undo / Redo say what they would do
    void update_summary();                        // category: helper: the title's subtitle + the status line
    void toast(const std::string& text, bool offer_undo = false);   // category: helper: an Adw toast
    core::Jobs::Id start_job(const std::string& label, core::Jobs::Work work);   // category: helper: submit + follow
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
    widgets::Button     m_open_button;         // s007
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
    Gtk::Widget*       m_strip_box = nullptr;  // the strip's frame (Tab hides it); managed
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
    std::unique_ptr<NewDialog>       m_new;       // s007: same again
    std::unique_ptr<NumberDialog>    m_feather;   // s008
    std::unique_ptr<SizeDialog>      m_image_size, m_canvas_size;   // s009

    // ── the file (s007) ────────────────────────────────────────────────────
    std::string        m_path;                 // the .klimt this document saves to; "" = never saved
    std::string        m_name = "No Image";    // what the title says ("photo.png", "Untitled"); s009: none at start
    std::string        m_folder;               // where the last chooser was (view state)
    std::uint64_t      m_saved_mark = 0;       // m_journal.mark() when it last matched a file
    core::Jobs::Id     m_file_job = 0;         // the open / save / export in flight

    // ── the stroke in progress (s008 m4) ───────────────────────────────────
    struct Stroke {
        bool                active = false, ending = false;
        core::Document::Id  id;
        core::paint::Brush  brush;
        std::shared_ptr<const pixel::TileMap> original, current, cov, mask;
        std::vector<core::paint::Dab> pending;      // dabs not yet painted
        core::paint::Dab    last;
        double              carry = 0;
        core::Jobs::Id      job = 0;
    };
    Stroke             m_stroke;
    core::select::Pt   m_stroke_last;              // Shift-click draws from here
    core::Document::Id m_stroke_last_id;
    bool               m_stroke_last_valid = false;

    // ── the move in progress (s009 m1): a drag (a gesture) or a nudge ───────
    struct Move {
        bool                active = false, ending = false, gesture = false;
        std::shared_ptr<const core::move::Plan> plan;
        int                 want_x = 0, want_y = 0;   // the offset asked for
        int                 done_x = 0, done_y = 0;   // the offset the layers hold
        core::Jobs::Id      job = 0;
    };
    Move               m_move;
    struct Nudges {                                    // a run of arrow presses
        std::shared_ptr<const core::move::Plan> plan;
        std::vector<core::Document::Id> sel;
        std::map<core::Document::Id, std::shared_ptr<const pixel::TileMap>> landed;
        std::shared_ptr<const pixel::TileMap> mask;
        int x = 0, y = 0;
    };
    Nudges             m_nudge;

    // ── the ants (s008) ────────────────────────────────────────────────────
    std::shared_ptr<const pixel::TileMap> m_ants_for;   // the mask the canvas's ants are of
    bool               m_ants_known = false;
    bool               m_ants_queued = false;
    core::Jobs::Id     m_ants_job = 0;
    bool               m_closing = false;      // a close already confirmed (or saved)

    // THE UI NEVER WAITS, wired. Declared LAST so the pool is destroyed FIRST:
    // it cancels and joins its workers while the dispatcher and document live.
    std::vector<core::Jobs::Id> m_job_ids;     // jobs submitted and not yet over
    // s007 m4: the composite on screen is per tile. m_shown is the snapshot
    // the canvas's tiles are (or are being) made from; m_dirty the tiles not
    // yet landed since it changed. Render jobs are silent (not in the status
    // line), in batches so a big image fills in as it's made.
    std::vector<core::Jobs::Id> m_renders;     // the batches in flight
    core::Layers          m_shown;
    core::Image           m_shown_image;
    core::TileSet         m_dirty;
    bool                  m_render_queued = false;
    sigc::connection      m_job_poll;
    Glib::Dispatcher      m_jobs_wake;
    core::Jobs            m_jobs;
};

}  // namespace klimt
