#pragma once
// FilterDialog -- the Filter menu (s014): one filter's settings, applied to
// the selected layer's pixels for good on OK (the Shell runs it as a job, one
// step). While it is up the canvas previews it live: the Shell filters what
// the view shows on a worker, a moment after the last change. Escape / Cancel
// / closing: nothing happens. docs/filters.md.
#include "core/Filter.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/drawingarea.h>
#include <algorithm>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <functional>
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
    // s039: whether the layer has a mask (Lens Blur's Layer Mask depth map).
    void set_has_mask(bool on) { m_has_mask = on; }
    // s041 m4: the document's saved channels (id, name) -- Lens Blur's Source lists them.
    void set_channels(std::vector<std::pair<std::string, std::string>> ch) { m_channels = std::move(ch); }
    // s039 m2: SET FOCAL POINT -- the button armed (the dialog lets the canvas
    // have a click) or not; the Shell answers with the depth there (0 .. 1).
    sigc::signal<void(bool)>& signal_focal_pick() { return m_focal_signal; }
    void set_focal_at(double depth);   // the focal distance that keeps that depth sharp; disarms
    // s040 m2: Radial Blur's centre picked on the picture (in % of it); disarms.
    void set_centre_at(double cx, double cy);
    // s040 m2: the picture's size -- the Blur Centre box takes its shape.
    void set_image_size(int w, int h) { m_iw = std::max(1, w); m_ih = std::max(1, h); }
    void focal_pick_off();             // disarms (Escape, the source changed, closed)
    bool focal_picking() const;

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
    // s034: Smart Sharpen's Remove, the rows only Motion has, the box rows go into (an expander's).
    Gtk::DropDown*            m_remove = nullptr;
    Gtk::DropDown*            m_shape = nullptr;   // m3: Maximum / Minimum's Preserve
    std::vector<Gtk::Widget*> m_motion_only;
    Gtk::Box*                 m_into = nullptr;
    void smart_rows();
    void reduce_rows();
    void extreme_rows();
    void lens_rows();                                  // s039
    void radial_rows();                                // s040
    void randomize_button();                           // s042 m3: a new seed
    // s041 m3: a labelled dropdown, synced with the filter.
    struct Choice { Gtk::DropDown* dd; std::function<int()> get; };
    std::vector<Choice> m_choices;
    void choice(const char* name, const char* label, std::vector<Glib::ustring> items, std::function<int()> get,
                std::function<void(int)> put, const char* tip = nullptr);
    Gtk::DropDown*            m_method = nullptr;      // s040: Spin / Zoom
    Gtk::DropDown*            m_quality = nullptr;     // Draft / Good / Best
    Gtk::DrawingArea*         m_centre_box = nullptr;  // s040 m2: the Blur Centre, dragged
    int                       m_iw = 4, m_ih = 3;
    void centre_from(double x, double y);
    void noise_rows(double lo, double hi, int digits, bool log);   // Add Noise's, Lens Blur's
    Gtk::ToggleButton*        m_focal_pick = nullptr;  // s039 m2
    sigc::signal<void(bool)>  m_focal_signal;
    Gtk::DropDown*            m_depth = nullptr;       // s039: the depth map's source
    Gtk::CheckButton*         m_invert = nullptr;
    Gtk::Label*               m_depth_note = nullptr;
    std::vector<Gtk::Widget*> m_depth_only;            // the rows that need a source
    bool                      m_has_mask = false;
    std::vector<std::pair<std::string, std::string>> m_channels;   // s041 m4
    Glib::RefPtr<Gtk::StringList> depth_model() const;
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
