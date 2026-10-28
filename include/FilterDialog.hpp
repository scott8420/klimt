#pragma once
// FilterDialog -- the Filter menu (s014): one filter's settings, applied to
// the selected layer's pixels for good on OK (the Shell runs it as a job, one
// step). While it is up the canvas previews it live: the Shell filters what
// the view shows on a worker, a moment after the last change. Escape / Cancel
// / closing: nothing happens. docs/filters.md.
#include "core/Filter.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <string>
#include <vector>

namespace klimt {

class FilterDialog : public Gtk::Window {
public:
    explicit FilterDialog(Gtk::Window& parent);
    ~FilterDialog() override;
    // Ask for one of `start`'s kind (its settings to begin with), about `layer`.
    void ask(const core::Filter& start, const std::string& layer);
    // s018: LIVE -- OK adds it to the layer's live filters (editable later)
    // instead of changing the pixels; remembered by the Shell. `editing`: the
    // dialog edits a live filter already there (no Live box: it is live).
    void set_live(bool live, bool editing);
    bool live() const { return m_editing || m_live.get_active(); }
    bool editing() const { return m_editing; }
    bool preview() const { return m_preview.get_active(); }
    const core::Filter& value() const { return m_f; }
    void set_busy(bool b);   // the preview is being made

    sigc::signal<void()>& signal_changed() { return m_changed; }    // the settings or Preview moved
    sigc::signal<void(const core::Filter&)>& signal_ok() { return m_ok; }
    sigc::signal<void()>& signal_closed() { return m_closed; }      // OK or not: the preview goes

private:
    // A labelled value: a slider (log when `log`) and a number field.
    void row(const char* name, const char* label, double lo, double hi, int digits, bool log, const char* unit, double* field);
    void rebuild();
    void sync(int keep_slider = -1);   // the controls from m_f (but that row's slider)

    struct Row { Gtk::Scale* s; Gtk::SpinButton* n; double lo, hi; bool log; double* field; };
    core::Filter         m_f;
    double               m_threshold = 0;   // the int fields, through double rows
    double               m_angle = 0;
    std::vector<Row>     m_rows;
    Gtk::CheckButton*    m_gauss = nullptr;
    Gtk::CheckButton*    m_uniform = nullptr;
    Gtk::CheckButton*    m_mono = nullptr;
    bool                 m_syncing = false;
    widgets::Box         m_body;
    widgets::Label       m_busy;
    Gtk::CheckButton     m_preview{"Preview"};
    Gtk::CheckButton     m_live{"Live -- editable later"};   // s018
    bool                 m_editing = false;
    widgets::Button      m_ok_button, m_cancel_button;
    sigc::signal<void()> m_changed, m_closed;
    sigc::signal<void(const core::Filter&)> m_ok;
};

}  // namespace klimt
