#pragma once
// RulerView -- a RULER beside the canvas (s031), and the corner where the two
// meet. It draws what core::rulers says for the canvas's view: ticks, labels
// counted from the document's zero point, the pointer's place. Right-click:
// the unit. The corner: drag it onto the picture to set the zero point;
// double-click it to put the zero point back at the top-left. docs/rulers.md.
#include "core/Rulers.hpp"

#include <gtkmm/drawingarea.h>
#include <sigc++/signal.h>

#include <functional>

namespace klimt {

class CanvasView;

class RulerView : public Gtk::DrawingArea {
public:
    static constexpr int kThick = 20;   // px across
    RulerView(CanvasView& canvas, bool horizontal);
    // What the numbers count in: the document's resolution, the side
    // measured (Percent), its zero point; and the unit.
    struct Doc { double dpi = 72, extent = 0, origin = 0; bool any = false; };
    void set_doc(const Doc& d) { m_doc = d; queue_draw(); }
    void set_unit(core::rulers::Unit u) { m_unit = u; queue_draw(); }
    void set_pointer(double screen) { m_pointer = screen; queue_draw(); }   // canvas coords; < 0: none
    void set_dark(bool d) { m_dark = d; queue_draw(); }
    // Right-click: the unit picked.
    sigc::signal<void(core::rulers::Unit)>& signal_unit() { return m_unit_signal; }
    // s032: a GUIDE dragged out of it -- where the pointer is (this widget's
    // coords) while dragging; the drop once (a click: x = -1e9).
    sigc::signal<void(double, double)>& signal_guide_drag() { return m_guide_drag; }
    sigc::signal<void(double, double)>& signal_guide_drop() { return m_guide_drop; }

private:
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void pick_unit(double x, double y);
    CanvasView& m_canvas;
    bool m_h;
    Doc m_doc;
    core::rulers::Unit m_unit = core::rulers::Unit::Pixels;
    double m_pointer = -1;
    bool m_dark = true;
    sigc::signal<void(core::rulers::Unit)> m_unit_signal;
    sigc::signal<void(double, double)> m_guide_drag, m_guide_drop;   // s032
    double m_gx0 = 0, m_gy0 = 0;
};

// The corner: a target to drag the zero point from (and double-click: reset).
class RulerCorner : public Gtk::DrawingArea {
public:
    RulerCorner();
    void set_dark(bool d) { m_dark = d; queue_draw(); }
    // While dragging: where the pointer is, in THIS widget's coordinates
    // (the Shell maps them to the canvas); the end: the same, once.
    sigc::signal<void(double, double)>& signal_drag() { return m_drag; }
    sigc::signal<void(double, double)>& signal_drop() { return m_drop; }
    sigc::signal<void()>& signal_reset() { return m_reset; }
private:
    bool m_dark = true;
    double m_x0 = 0, m_y0 = 0;
    sigc::signal<void(double, double)> m_drag, m_drop;
    sigc::signal<void()> m_reset;
};

}  // namespace klimt
