#pragma once
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
    // What is selected (panel order). Repaints the fields from the document.
    void show(const std::vector<Id>& selection);
    // s013: an adjustment layer's controls (Levels / Curves draw the
    // histogram of what is below it -- the Shell makes it on a worker when
    // asked, and hands it back for that layer).
    sigc::signal<void(const Id&)>& signal_histogram_wanted() { return m_hist_wanted; }
    void set_histogram(const Id& id, const core::adjust::Histogram& h);

private:
    void paint();
    void on_blend();
    void on_opacity();
    void on_fill();
    void on_lock();
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
    Id                    m_hist_id;    // the layer whose histogram m_adjust has (or was asked for)
    sigc::signal<void(const Id&)> m_hist_wanted;
};

}  // namespace klimt
