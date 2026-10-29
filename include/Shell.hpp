#pragma once
#include "widgets/Widgets.hpp"
#include "core/Document.hpp"   // the document (layer tree) + its journal
#include "core/Jobs.hpp"       // the worker pool, pumped by a Dispatcher
#include "core/File.hpp"       // s007: documents in and out of files
#include "core/Composite.hpp"  // s007: dirty tiles
#include "core/Paint.hpp"      // s008: the brush
#include "core/Move.hpp"       // s009: the Move tool
#include "core/Canvas.hpp"     // s009: Crop, Image / Canvas Size
#include "core/Transform.hpp"  // s011: Free Transform
#include "core/Prefs.hpp"      // s014 m4: settings remembered between runs
#include "core/Tabs.hpp"       // s020: several documents in one window
#include "core/Fade.hpp"       // s020: Edit > Fade
#include "CanvasView.hpp"
#include "ContextStrip.hpp"
#include "EdgeTab.hpp"
#include "ChannelsPanel.hpp"   // s016: saved selections
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
#include <optional>
#include <set>
#include <string>
#include <vector>

// libadwaita's C types (s005: there is no C++ binding; the Shell holds its
// few Adw widgets as C pointers, owned by their GTK parents).
typedef struct _AdwOverlaySplitView AdwOverlaySplitView;
typedef struct _AdwToastOverlay AdwToastOverlay;
typedef struct _AdwWindowTitle AdwWindowTitle;
typedef struct _AdwTabView AdwTabView;   // s020
typedef struct _AdwTabBar AdwTabBar;
typedef struct _AdwTabPage AdwTabPage;

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
//   Transform-- Free Transform, flips, quarter turns.    src/Shell_transform.cpp (s011)
//   Masks    -- the Layer Mask menu, the mask view.     src/Shell_masks.cpp (s012)
//   Tabs     -- document tabs: switch, close, the bar.  src/Shell_tabs.cpp (s020)
//   Fade     -- Edit > Fade: the dialog, its preview.   src/Shell_fade.cpp (s020)
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
class AdjustDialog;
class FilterDialog;
class FadeDialog;
class ColourRangeDialog;
class ShortcutsDialog;
class SizeDialog;

class Shell : public Gtk::ApplicationWindow {
public:
    Shell();
    ~Shell() override;

    void build_ui();                             // category: glue
    void open_path(const std::string& path);     // category: files: open a file (the command line, Open)
    void open_paths(const std::vector<std::string>& paths);   // category: files: s020 -- several, one tab each, in turn
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
    void adopt(core::file::Contents c, const std::string& path, const std::string& name, const std::string& source = {});   // category: files: the document becomes c
    void ask_unsaved(const std::string& doing, std::function<void()> go);   // category: files: Save / Discard / Cancel
    bool dirty() const { return m_tabs.dirty(m_tabs.live()); }
    bool file_job_busy();                        // category: files: one open / save / export at a time
    void update_title();                         // category: files: the name, and a dot when unsaved

    // ── tabs (s020) ────────────────────────────────────────────────────────
    void build_tabs(Gtk::Box& into);             // category: tabs: the bar (over the strip) and its view
    bool doc_busy(bool say);                     // category: tabs: a job / stroke / preview on the live document?
    bool tab_reusable() const;                   // category: tabs: the live tab is empty and clean -- New / Open fill it
    bool new_tab();                              // category: tabs: an empty tab after the live one, shown
    bool show_tab(std::size_t i);                // category: tabs: tab i becomes the live document
    void after_switch();                         // category: tabs: everything that follows the document, again
    void remember_tab();                         // category: tabs: the live tab's view, kept while it is parked
    void close_tab(std::size_t i);               // category: tabs: Ctrl+W / the tab's x -- asks if unsaved
    void close_all_then(std::function<void()> done, std::set<std::uint64_t> answered = {});   // category: tabs: quitting -- each unsaved tab asks
    void sync_tabs();                            // category: tabs: the pages follow the model (titles, order, selection)
    void step_tab(int by);                       // category: tabs: Ctrl+Tab / Ctrl+Shift+Tab
    AdwTabPage* page_of(std::uint64_t key) const;   // category: tabs: the bar's page of a tab
    void copy_to_tab(std::uint64_t key, const std::vector<core::Document::Id>& ids);   // category: tabs: s022 -- layers into another tab (one step there)
    void sync_duplicate_to();                    // category: tabs: s022 -- Layer > Duplicate To lists the other tabs

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
    void on_modify(core::select::Modify k);      // category: select: Select > Modify > ... (asks; a job) (s011 m4)
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

    // ── transform (s011 m2) ────────────────────────────────────────────────
    void on_free_transform();                    // category: transform: Ctrl+T -- the box on the canvas
    void on_xform_change(const std::array<double, 6>& m);   // category: transform: the box moved
    void xform_pump();                           // category: transform: the preview, one job at a time
    void xform_commit();                         // category: transform: Enter -- the step, bicubic, on a worker
    void xform_cancel();                         // category: transform: Esc / Ctrl+Z -- nothing was written
    void xform_end();                            // category: transform: the mode is over
    bool xform_current(const core::xform::Plan& p) const;   // category: transform: the plan's pixels still there?
    void on_turn(core::xform::Turn t);           // category: transform: Image > Image Rotation (s011 m3)
    void on_rotate_canvas();                     // category: transform: Image > Image Rotation > Arbitrary (asks)
    void on_straighten(core::select::Pt a, core::select::Pt b);   // category: transform: the Crop tool's Straighten
    void run_xform(const std::string& label, std::function<core::xform::Affine(const pixel::Rect&)> map);   // category: transform: a flip / turn as a job

    // ── layer masks (s012 m3) ──────────────────────────────────────────────
    void on_mask_add(int from);                  // category: masks: Layer > Layer Mask > Reveal / Hide ... (-1: the panel's button)
    void on_mask_verb(const std::string& verb);  // category: masks: delete / apply / toggle / invert / edit
    void on_mask_view(const std::string& id);    // category: masks: Alt-click a chip -- the mask on the canvas
    float mask_hide_of(ToolRail::RGB c) const;   // category: masks: a colour as what it hides
    void on_gradient(core::select::Pt a, core::select::Pt b);   // category: masks: the Gradient tool's drag (a job) (s012 m4)

    // ── adjustments (s013) ─────────────────────────────────────────────────
    void on_adjust_new(core::AdjustKind k);      // category: adjust: Layer > New Adjustment Layer (and the panel's button)
    void on_adjust_apply(core::AdjustKind k);    // category: adjust: Image > Adjustments -- the dialog, then a job
    // s014: the Filter menu (Shell_filter.cpp).
    // s015: retouching (Shell_paint.cpp).
    void on_clone_source(core::select::Pt p);    // category: paint: Alt- / Ctrl-click with the Clone Stamp
    void show_clone_marker();                    // category: paint: the source's crosshair follows m_clone
    void heal_finish();                          // category: paint: a Heal stroke's last dab landed -- heal it (a job)
    void on_quick_mask();                        // category: select: s016 -- Q: Quick Mask on / off (view state)
    void on_save_selection();                    // category: select: s016 -- Select > Save Selection (a channel)
    void update_channels();                      // category: select: s016 -- the Channels tab follows the document
    void on_selection_stroke(const core::paint::Dab& p, bool erase, core::paint::Brush b, int smoothing);   // category: paint: s016 -- a Quick Mask stroke
    void on_patch(CanvasView::MoveAt at, int dx, int dy);   // category: paint: s016 -- Heal > Patch, the drag
    void patch_pump();                           // category: paint: one patch preview job at a time; the landing on release
    void on_filter(core::FilterKind k);          // category: filter: Filter > ... -- the dialog, then a job
    void on_filter_last(bool dialog);            // category: filter: Last Filter (Ctrl+F) / its settings (Ctrl+Alt+F)
    bool filter_layer_ok(const std::string& verb);   // category: filter: the primary layer takes a filter? (toasts why not)
    void filter_run(const core::Filter& f);      // category: filter: on m_filter_target's pixels, a job; one step
    void filter_preview_soon();                  // category: filter: the preview, a moment after the last change
    void filter_preview_stop();                  // category: filter: drop the preview and its job
    void on_live_edit(const core::Document::Id& id, std::size_t i);
    void on_style(const std::string& verb);      // category: layer: s019 -- Layer > Layer Style   // category: filter: s018 -- a live filter's dialog
    void on_colour_range();                      // category: select: s024 -- Select > Color Range (the dialog)
    void on_channel_view(int k);                 // category: select: s026 -- a colour channel alone, as grey (-1: the picture)
    void on_colour_load(int k, int op);          // category: select: s026 -- a colour channel as the selection (a job)
    void on_colour_dup(int k);                   // category: select: s026 -- a colour channel as a saved channel (a job)
    int                m_channel_view = -1;      // s026: the colour channel the canvas shows alone (-1: the picture)
    void range_preview_soon();                   // category: select: its preview, a moment after the last change
    void range_run();                            // category: select: OK -- a job, ONE step
    void on_fade();                              // category: fade: s020 -- Edit > Fade (the dialog)
    void fade_preview_soon();                    // category: fade: the preview, a moment after the last change
    void fade_preview_stop();                    // category: fade: drop the preview and its job
    void fade_run();                             // category: fade: OK -- a job, ONE step
    void on_histogram(const core::Document::Id& id);   // category: adjust: what's below an adjustment layer, counted (a job)

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
    core::Tabs         m_tabs{m_doc, m_journal};   // s020: the live pair + the parked tabs
    LayersPanel        m_layers{m_doc, m_journal};
    PropertiesPanel    m_properties{m_doc, m_journal};
    HistoryPanel       m_history{m_journal};
    ChannelsPanel      m_channels;                 // s016

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
    std::unique_ptr<NumberDialog>    m_rotate_dialog;               // s011
    std::map<int, std::unique_ptr<NumberDialog>> m_modify_dialogs;  // s011 m4: one per Modify kind
    std::unique_ptr<AdjustDialog>    m_adjust_dialog;               // s013: Image > Adjustments
    std::unique_ptr<FilterDialog>    m_filter_dialog;               // s014: the Filter menu
    std::unique_ptr<FadeDialog>      m_fade_dialog;                 // s020: Edit > Fade
    std::unique_ptr<ColourRangeDialog> m_range_dialog;              // s024: Select > Color Range
    std::shared_ptr<const pixel::TileMap> m_range_preview;          // its preview, shown as Quick Mask's red
    sigc::connection            m_range_soon;
    core::Jobs::Id              m_range_job = 0;

    // ── the file (s007) ────────────────────────────────────────────────────
    // s020: the live tab's file (the .klimt it saves to, its name, its saved
    // mark) lives in m_tabs.live_info(); these name it.
    std::string&       path_() { return m_tabs.live_info().path; }
    std::string&       name_() { return m_tabs.live_info().name; }
    std::string        m_folder;               // where the last chooser was (view state)
    core::Jobs::Id     m_file_job = 0;         // the open / save / export in flight
    std::vector<std::string> m_open_queue;     // s020: files still to open (the command line, a second launch)
    void open_next();                          // category: files: the queue's next, once no file job runs
    void place_paths(const std::vector<std::string>& paths);   // category: files: s022 -- files dropped on the canvas: layers here
    void build_file_drops();                   // category: files: s022 -- drops from the file manager
    void save_session();                       // category: files: s023 -- the open tabs, for next time
    bool restore_session();                    // category: files: s023 -- started with no file: last time's tabs
    std::string        m_restore_front;        // s023: the tab to bring to the front once the queue is done

    // ── the stroke in progress (s008 m4) ───────────────────────────────────
    struct Stroke {
        bool                active = false, ending = false;
        core::Document::Id  id;
        core::paint::Brush  brush;
        std::shared_ptr<const pixel::TileMap> original, current, cov, mask;
        std::vector<core::paint::Dab> pending;      // dabs not yet painted
        core::paint::Dab    last;
        core::paint::Smoother smoother;             // s012: the stabiliser (Smoothing)
        bool                on_mask = false;        // s012: painting the layer's mask
        bool                on_selection = false;   // s016: Quick Mask -- painting the selection
        bool                on_filter_mask = false; // s018 m4: painting the layer's filter mask
        std::shared_ptr<core::paint::SmudgeCarry> smudge;   // s017: a Smudge stroke's finger (its jobs run in turn)
        double              carry = 0;
        core::Jobs::Id      job = 0;
        core::Jobs::Id      source_job = 0;          // s015: All Layers -- the flattened source, before any dab
        // s015 m4: a Heal stroke -- healed when the last dab lands, from `ref`
        // (the layer as it began, or the flattened picture), with `heal_brush`
        // (the stroke's own options; Spot shows a grey trail meanwhile).
        bool                heal = false, spot = false;
        std::shared_ptr<const pixel::TileMap> ref;
        core::paint::Brush  heal_brush;
    };
    Stroke             m_stroke;
    core::paint::CloneSource m_clone;               // s015: where the Clone Stamp takes from
    // s016: the Patch drag -- the selection healed from where it's dragged to,
    // previewed (m_preview) one job at a time; release lands the last one.
    struct Patch {
        bool                active = false, ending = false, alpha_lock = false;
        core::Document::Id  id;
        std::shared_ptr<const pixel::TileMap> expect, original, ref, sel, result;   // expect: the layer's pixels at the press
        int                 want_x = 0, want_y = 0, done_x = 0, done_y = 0;
        core::Jobs::Id      job = 0, ref_job = 0;
    };
    Patch              m_patch;
    // s016: Quick Mask -- the Brush and Eraser paint the selection; the
    // unselected shows red. View state, not a step.
    bool               m_quick_mask = false;
    std::shared_ptr<const pixel::TileMap> m_shown_qm;   // the selection the red was drawn from
    bool               m_shown_qm_on = false;
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

    // ── Free Transform in progress (s011 m2) ───────────────────────────────
    struct Xform {
        bool                active = false, planning = false, committing = false, has_done = false;
        std::shared_ptr<const core::xform::Plan> plan;
        core::xform::Affine want, done;
        core::Jobs::Id      job = 0;
    };
    Xform              m_xform;
    // What the render shows in place of a layer's pixels (the preview).
    std::map<core::Document::Id, std::shared_ptr<const pixel::TileMap>> m_preview;
    std::string        m_mask_view;              // s012: the layer whose mask the canvas shows ("" : the image)
    // s013: Image > Adjustments' live preview -- the adjustment shown on the
    // layer it will change (a stand-in adjustment layer clipped to it).
    core::Document::Id m_adjust_target;
    std::optional<core::Adjust> m_adjust_preview;
    // s014: the Filter menu -- its layer, the last one run (Ctrl+F), the
    // preview's pending timer and job.
    core::Document::Id          m_filter_target;
    std::optional<core::Filter> m_last_filter;
    sigc::connection            m_filter_soon;
    core::Jobs::Id              m_filter_job = 0;
    std::uint32_t               m_filter_seed = 0;
    bool                        m_filter_live = false;
    std::optional<core::Styles> m_style_clip;            // s019: Copy Layer Style   // s018: the dialog's Live box, remembered
    std::optional<std::size_t>  m_filter_edit;           // s018: the live filter the dialog edits
    core::Jobs::Id              m_filter_landing = 0;   // OK's job: the preview stays until it lands
    // s020: Edit > Fade -- what it fades, the preview's timer and job, OK's job.
    std::optional<core::fade::Source> m_fade_src;
    sigc::connection            m_fade_soon;
    core::Jobs::Id              m_fade_job = 0, m_fade_landing = 0;
    Glib::RefPtr<Gio::SimpleAction> m_act_fade;
    // s014 m4: the app's settings (~/.config/klimt/prefs.json); saved a moment
    // after a change (the divider dragged).
    core::Prefs                 m_prefs;
    std::string                 m_prefs_file;
    sigc::connection            m_prefs_soon;

    // ── the tabs (s020) ────────────────────────────────────────────────────
    AdwTabView*        m_tab_view = nullptr;     // pages with empty children: the bar's model
    AdwTabBar*         m_tab_bar = nullptr;
    bool               m_tab_sync = false;       // the pages are being set from the model
    std::map<std::uint64_t, AdwTabPage*> m_pages;   // a tab's key -> its page
    Glib::RefPtr<Gio::Menu> m_dup_to_menu;          // s022: Layer > Duplicate To > the other tabs
    struct TabView {                             // what a parked tab keeps of the window
        CanvasView::View view;
        std::vector<core::Document::Id> selection;
        std::string      mask_view;
        bool             quick_mask = false;
        core::paint::CloneSource clone;
        bool             known = false;
    };
    std::map<std::uint64_t, TabView> m_tab_views;

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
