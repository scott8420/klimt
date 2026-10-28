#pragma once
// LayersPanel -- the Layers panel on core::Document (s003). The first UI
// milestone: the layer tree you can see and use.
//
// Pole B of docs/tree-and-dnd.md: a ListBox rebuilt from the model on every
// change (a layer stack is tens of rows, not tens of thousands). Positional
// inside (rows), identity at the edge: every row carries its layer's id as its
// widget name, and the panel's selection is a list of ids.
//
// The panel WRITES NOTHING ITSELF. Every user choice is one call into
// core::layers (which goes through the journal); then the panel says where the
// selection goes. The rows repaint from the document on its change signal,
// once per idle, after the writes (jot s045: never rebuild under a live click).
//
// What is VIEW state, not document state (and so not undoable): which groups
// are collapsed, the selection, a rename in progress. Photoshop does the same.
#include "widgets/Widgets.hpp"
#include "core/Document.hpp"

#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/listboxrow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/togglebutton.h>

#include <functional>
#include <set>
#include <sigc++/signal.h>
#include <string>
#include <vector>

namespace klimt {

class LayersPanel : public widgets::Box {
public:
    using Id = core::Document::Id;

    LayersPanel(core::Document& doc, core::DocJournal& journal);

    // The selection, in panel order. The primary (the one Properties shows
    // first) is the first.
    const std::vector<Id>& selection() const { return m_sel; }
    Id primary() const { return m_sel.empty() ? Id{} : m_sel.front(); }
    bool dragging() const { return !m_dragging.empty(); }   // s022: a row's drag in flight
    // Select (after the repaint the last write queued); `keys` gives the
    // selected row the keyboard so the next key lands (a delete, an undo).
    void select(const std::vector<Id>& ids, bool keys);
    // s007: a different document -- forget the old one's selection and
    // collapsed groups (its ids may be reused by the new one).
    void forget_view() { m_sel.clear(); m_collapsed.clear(); queue_repaint(); }
    void queue_repaint();
    // Called after every repaint: the Shell greys its layer actions here.
    void set_on_change(std::function<void()> f) { m_changed = std::move(f); }

    // ── the verbs the panel's buttons and the window's keys call ────────────
    void new_layer();
    void new_group();
    void duplicate();
    void group();
    void ungroup();
    void remove();
    void raise();
    void lower();
    void toggle_clip();
    void rename_primary();   // F2 / double-click: the name becomes a field

    // s012: LAYER MASKS on the rows. The mask chip: a click paints the MASK
    // (the Brush, Eraser and Fill go to it) -- clicking the layer's own icon
    // goes back to its pixels; Shift-click turns the mask off / on;
    // Alt-click shows the mask itself on the canvas.
    // "" : painting pixels. s013: an adjustment layer has no pixels -- picked
    // alone, its mask is always what's painted.
    Id   mask_target() const;
    void set_mask_target(const Id& id);
    sigc::signal<void(const Id&)>& signal_mask_view() { return m_mask_view; }   // Alt-click a chip
    void set_mask_viewing(const Id& id) { m_mask_viewing = id; queue_repaint(); }

    // Is there something for each verb to act on? (The Shell greys by these.)
    bool has_selection() const { return !m_sel.empty(); }
    bool primary_is_group() const;
    bool primary_is_clipped() const;

private:
    void repaint();
    Gtk::ListBoxRow* build_row(const Id& id);
    void start_rename(const Id& id);
    void commit_rename(const std::string& text);
    void on_rows_selected();
    std::vector<Id> in_walk_order(const std::vector<Id>& ids) const;
    // ── drag and drop (s003 m2; docs/tree-and-dnd.md) ──────────────────────
    void add_dnd(Gtk::ListBoxRow& row, const Id& id);       // source + target on a row
    core::layers::Where where_on(const Id& id, double y, int height) const;
    void mark_drop(Gtk::ListBoxRow* row, const char* css);  // the drop line; nullptr clears
    bool drop(const Id& target, core::layers::Where where, bool copy = false);
    static bool copying(Gdk::ModifierType state);   // s022: Alt or Ctrl held -- a drag copies

    core::Document&   m_doc;
    core::DocJournal& m_journal;

    std::vector<Id>   m_sel;
    std::set<Id>      m_collapsed;          // view state
    Id                m_renaming;           // the row whose name is a field
    Gtk::Entry*       m_rename_field = nullptr;   // that field, this repaint (managed by its row)
    bool              m_painting = false;   // a repaint sets fields: not a user choice
    bool              m_repaint_queued = false;
    bool              m_keys_to_list = false;
    std::function<void()> m_changed;
    std::vector<Id>   m_dragging;               // what the drag in flight carries
    Gtk::ListBoxRow*  m_drop_row = nullptr;     // the row showing the drop line
    const char*       m_drop_css = nullptr;
    Id                m_mask_target;            // s012: the layer whose mask is painted
    Id                m_mask_viewing;           // s012: the mask shown on the canvas
    sigc::signal<void(const Id&)> m_mask_view;

    // The rows, then the verbs. (Blend / opacity / fill / locks moved to the
    // PropertiesPanel at s005 -- docs/ui.md call 2: the panel is the stack.)
    widgets::ScrolledWindow m_scroll;
    widgets::ListBox      m_list;
};

}  // namespace klimt
