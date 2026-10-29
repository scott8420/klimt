#pragma once
#include <functional>
// PropertiesPanel -- ONE inspector for whatever is selected (s005, docs/ui.md
// 5; call 2: blend / opacity / fill / locks left the Layers panel for here).
// Today: the selected layers' blend, opacity, fill and locks -- shown for the
// first, set on ALL of them as one step (layers under an All lock skipped). With pixels:
// its adjustment stack and live filters (no dialogs), its styles; a
// selection's feather and refine; text's type settings.
//
// Writes nothing itself: every control is one call into core::layers (the
// journal's one door). A repaint sets fields under m_painting -- not a choice.
#include "AdjustEditor.hpp"
#include "FilterLayerEditor.hpp"   // s030
#include "StylesEditor.hpp"   // s019
#include "widgets/Widgets.hpp"
#include "core/Document.hpp"

#include <gtkmm/dropdown.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/togglebutton.h>

#include <string>
#include <vector>

namespace klimt {

class PropertiesPanel : public widgets::Box {
public:
    using Id = core::Document::Id;
    PropertiesPanel(core::Document& doc, core::DocJournal& journal);
    void patterns_changed() { m_styles.patterns_changed(); }   // s033: Define Pattern
    // What is selected (panel order). Repaints the fields from the document.
    void show(const std::vector<Id>& selection);
    // s013: an adjustment layer's controls (Levels / Curves draw the
    // histogram of what is below it -- the Shell makes it on a worker when
    // asked, and hands it back for that layer).
    sigc::signal<void(const Id&)>& signal_histogram_wanted() { return m_hist_wanted; }
    void set_histogram(const Id& id, const core::adjust::Histogram& h);
    // s018: a live filter's settings wanted (double-click it): its layer, its place.
    sigc::signal<void(const Id&, std::size_t)>& signal_live_edit() { return m_live_edit; }
    // s018 m4: the layer whose FILTER MASK the brushes paint ("" none) -- the
    // stack's "Paint Mask" toggle; it lets go when another layer is picked.
    const Id& filter_mask_target() const { return m_fmask_target; }
    sigc::signal<void(bool)>& signal_filter_mask_paint() { return m_fmask_signal; }

private:
    void paint();
    void on_blend();
    void on_opacity();
    void on_fill();
    void on_lock();
    void paint_live(const core::Layer& l);   // s018: the stack's rows
    Id primary() const { return m_sel.empty() ? Id{} : m_sel.front(); }

    core::Document&   m_doc;
    core::DocJournal& m_journal;
    std::vector<Id>   m_sel;
    bool              m_painting = false;

    widgets::Label        m_title;      // "Retouch"
    widgets::Label        m_subtitle;   // "pixel layer" / "group of 4"
    widgets::Box          m_fields;     // hidden when nothing is selected
    widgets::DropDown     m_blend;
    widgets::Scale        m_opacity;
    widgets::Scale        m_fill;
    widgets::ToggleButton m_lock_alpha, m_lock_pixels, m_lock_position, m_lock_all;
    widgets::Label        m_later;
    AdjustEditor          m_adjust;     // s013
    Id                    m_adjust_id;  // the layer m_adjust shows
    FilterLayerEditor     m_filter;     // s030: a filter layer's settings
    Id                    m_filter_id;  // the layer m_filter shows
    Id                    m_hist_id;    // the layer whose histogram m_adjust has (or was asked for)
    sigc::signal<void(const Id&)> m_hist_wanted;
    // s018: the live filters -- a heading, a row each, Rasterize.
    StylesEditor          m_styles;     // s019
    widgets::Box          m_live;
    widgets::Box          m_live_rows;
    std::vector<core::LiveFilter> m_live_shown;
    std::vector<std::function<void()>> m_blend_follow;   // s023: the rows' blending labels follow
    Id                    m_live_id;
    bool                  m_live_had_mask = false;
    sigc::signal<void(const Id&, std::size_t)> m_live_edit;
    Id                    m_fmask_target;
    sigc::signal<void(bool)> m_fmask_signal;
};

}  // namespace klimt
