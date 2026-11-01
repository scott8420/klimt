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
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/colordialogbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/togglebutton.h>

#include <functional>
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
    // s064: Levels / Curves' eyedroppers. Armed (black / grey / white) or not
    // -- the owner sends the canvas's next picks to `dropped`. Rebuilding for
    // another kind lets go (and says so).
    sigc::signal<void(core::adjust::Dropper)>& signal_dropper() { return m_dropper_sig; }
    core::adjust::Dropper dropper() const { return m_dropper; }
    void dropped(const std::array<double, 3>& rgb);
    void drop_off();                      // let go, no signal
    sigc::signal<void()>& signal_end() { return m_end; }

private:
    void rebuild();
    void paint();                         // controls from m_a (under m_painting)
    void emit();                          // m_a changed by a control
    void reset();
    Gtk::Scale* slider(const char* name, const char* label, double lo, double hi, int* field);
    // s061: a double setting (Exposure's), `digits` after the point.
    Gtk::Scale* dslider(const char* name, const char* label, double lo, double hi, double step, int digits, double* field);
    // s063: a slider with a word at each end (Colour Balance's Cyan .. Red).
    Gtk::Scale* pair_slider(const char* name, const char* left, const char* right, double lo, double hi, int* field);
    // s063: a "page" dropdown (Colour Balance's tone, Channel Mixer's output,
    // Selective Colour's colours) picks which settings the paged sliders set.
    Gtk::DropDown* pager(const char* name, const char* label, const std::vector<Glib::ustring>& pages, guint start,
                         std::function<int*(guint page, std::size_t i)> field_of);
    void repage();                        // the paged sliders bound to the page shown
    void begin_end_on(Gtk::Widget& w);    // a drag on w is a begin .. end
    void dropper_row();                   // s064: the three eyedroppers
    int  channel() const;                 // Levels / Curves: 0 = RGB, 1..3

    // Levels.
    void draw_levels(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void levels_press(double x, double y);
    void levels_drag(double x, double y);
    // Curves.
    void draw_curves(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void draw_threshold(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);   // s062
    void threshold_at(double x);
    void curves_press(double x, double y);
    void curves_drag(double x, double y);
    void curves_release();
    double graph_side(int w, int h) const;
    // s063: Gradient Map -- the bar, its stops below it (click under the bar
    // to add one, drag to move, drag off to remove) and the midpoints.
    void draw_gradient(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void gradient_press(double x, double y);
    void gradient_drag(double x, double y);
    void gradient_release();
    // s064: Hue/Saturation's range bar -- the hues before and after, the
    // picked range's band and fall-off with four handles to drag.
    void draw_huebar(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void huebar_press(double x, double y);
    void huebar_drag(double x);
    int  hue_page() const;                // 0 Master, 1..6 Reds .. Magentas
    double grad_x(double at) const;       // a place on the gradient -> x in the strip
    double grad_t(double x) const;        // and back, 0..1

    core::Adjust m_a;
    core::AdjustKind m_built = core::AdjustKind::None;
    bool m_painting = false;
    std::optional<core::adjust::Histogram> m_hist;

    Gtk::Box  m_body{Gtk::Orientation::VERTICAL, 8};
    struct Bound { Gtk::Scale* s; int* field; double* dfield = nullptr; };
    std::vector<Bound> m_sliders;
    Gtk::CheckButton* m_colorize = nullptr;
    // s062: Black & White's Tint; Photo Filter's preset, colour and Preserve Luminosity.
    Gtk::CheckButton* m_tint = nullptr;
    Gtk::Scale*       m_tint_h = nullptr;
    Gtk::Scale*       m_tint_s = nullptr;
    Gtk::DropDown*    m_preset = nullptr;
    Gtk::ColorDialogButton* m_filter_colour = nullptr;
    Gtk::CheckButton* m_luma = nullptr;
    Gtk::DropDown*    m_channel = nullptr;
    // s063: the page dropdown and the sliders it rebinds (indices in m_sliders).
    Gtk::DropDown*    m_page = nullptr;
    std::vector<std::size_t> m_paged;
    std::function<int*(guint, std::size_t)> m_page_field;
    // s063: Channel Mixer's Monochrome and its Total (what the sliders add to).
    Gtk::CheckButton* m_mono = nullptr;
    Gtk::Label*       m_total = nullptr;
    guint             m_mixer_page = 0;   // the output shown before Monochrome
    // s063: Selective Colour's method (Relative; Absolute grouped with it).
    Gtk::CheckButton* m_relative = nullptr;
    Gtk::CheckButton* m_absolute = nullptr;
    // s063: Gradient Map's controls; the stop picked; what a drag moves.
    Gtk::DropDown*    m_grad_preset = nullptr;
    Gtk::ColorDialogButton* m_stop_colour = nullptr;
    Gtk::SpinButton*  m_stop_at = nullptr;
    Gtk::Button*      m_stop_del = nullptr;
    Gtk::CheckButton* m_reverse = nullptr;
    Gtk::CheckButton* m_dither = nullptr;
    int  m_stop = 0;
    int  m_grad_drag = 0;                 // 0 nothing, 1 a stop, 2 a midpoint (of the segment ending at m_mid)
    int  m_mid = -1;
    // s064: the range bar's handle being dragged (-1 none) and the hue at its middle while it is.
    bool m_huesat_ranges = false;
    Gtk::Button* m_auto = nullptr;        // s064: Levels / Curves' Auto (needs the histogram)
    std::array<Gtk::ToggleButton*, 3> m_drop_btn{};
    core::adjust::Dropper m_dropper = core::adjust::Dropper::None;
    sigc::signal<void(core::adjust::Dropper)> m_dropper_sig;
    int  m_band_handle = -1;
    double m_band_centre = 0;
    bool m_stop_off = false;              // a stop dragged off the strip: removed on release
    void totals();
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
