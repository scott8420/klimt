#pragma once
// AdjustEditor -- the controls for ONE adjustment (s013, docs/adjustments.md):
// Brightness / Contrast and Hue / Saturation as sliders; Levels as a
// histogram with its input sliders, an output bar and number fields; Curves
// as a graph you click to add points and drag (drag a point off the graph to
// remove it). The Properties panel shows one for an adjustment layer; the
// Image > Adjustments dialog shows one for the pixels.
//
// Writes nothing: it says what the settings are now (`changed`, every move,
// live) and when a drag starts and ends (`begin` / `end`), so its owner can
// make a drag ONE step.
#include "core/Adjust.hpp"

#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/spinbutton.h>

#include <optional>
#include <vector>

namespace klimt {

class AdjustEditor : public Gtk::Box {
public:
    AdjustEditor();
    // Show these settings (a different kind rebuilds the controls). Emits nothing.
    void set(const core::Adjust& a);
    const core::Adjust& value() const { return m_a; }
    // What Levels and Curves draw behind themselves; nullopt: none yet.
    void set_histogram(const std::optional<core::adjust::Histogram>& h);

    sigc::signal<void(const core::Adjust&)>& signal_changed() { return m_changed; }
    sigc::signal<void()>& signal_begin() { return m_begin; }
    sigc::signal<void()>& signal_end() { return m_end; }

private:
    void rebuild();
    void paint();                         // controls from m_a (under m_painting)
    void emit();                          // m_a changed by a control
    void reset();
    Gtk::Scale* slider(const char* name, const char* label, double lo, double hi, int* field);
    void begin_end_on(Gtk::Widget& w);    // a drag on w is a begin .. end
    int  channel() const;                 // Levels / Curves: 0 = RGB, 1..3

    // Levels.
    void draw_levels(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void levels_press(double x, double y);
    void levels_drag(double x, double y);
    // Curves.
    void draw_curves(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void curves_press(double x, double y);
    void curves_drag(double x, double y);
    void curves_release();
    double graph_side(int w, int h) const;

    core::Adjust m_a;
    core::AdjustKind m_built = core::AdjustKind::None;
    bool m_painting = false;
    std::optional<core::adjust::Histogram> m_hist;

    Gtk::Box  m_body{Gtk::Orientation::VERTICAL, 8};
    struct Bound { Gtk::Scale* s; int* field; };
    std::vector<Bound> m_sliders;
    Gtk::CheckButton* m_colorize = nullptr;
    Gtk::DropDown*    m_channel = nullptr;
    Gtk::DrawingArea* m_graph = nullptr;
    // Levels' number fields: in black, gamma, in white, out black, out white.
    Gtk::SpinButton*  m_lv[5] = {};
    // Curves: the point being dragged / last picked, and its fields.
    int  m_point = -1;
    bool m_point_off = false;             // dragged off the graph: removed on release
    Gtk::SpinButton* m_cin = nullptr;
    Gtk::SpinButton* m_cout = nullptr;
    int  m_lv_handle = -1;                // Levels: 0 black, 1 grey, 2 white, 3 out black, 4 out white
    double m_press_x = 0, m_press_y = 0;

    sigc::signal<void(const core::Adjust&)> m_changed;
    sigc::signal<void()> m_begin, m_end;
};

}  // namespace klimt
