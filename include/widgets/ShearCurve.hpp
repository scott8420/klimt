#pragma once
// s044 m2 -- SHEAR'S CURVE (Photoshop's Shear box): a vertical line on a
// grid, top to bottom the picture's rows, across how far each row moves.
// Click the line to add a point, drag a point to move it, drag it out of the
// box sideways (or double-click it) to take it away; the ends stay (they
// move across only). core::Filter::shear holds the points; the curve drawn
// is core::filter::shear_at's.
#include "core/Filter.hpp"

#include <gtkmm/drawingarea.h>
#include <sigc++/signal.h>

namespace klimt::widgets {

class ShearCurve : public Gtk::DrawingArea {
public:
    explicit ShearCurve(const char* name);
    void set(const core::Filter& f);              // the points to show (a Shear)
    const core::Filter& value() const { return m_f; }
    sigc::signal<void()>& signal_changed() { return m_changed; }
    sigc::signal<void()>& signal_begin() { return m_begin; }   // a drag starts / ends (one undo step for it)
    sigc::signal<void()>& signal_end() { return m_end; }

private:
    core::Filter m_f = core::filter::fresh(core::FilterKind::Shear);
    int  m_drag = -1;                             // the point held, or -1
    bool m_out = false;                           // held out of the box: lets go -> gone
    gint64 m_last_press = 0;                      // a double-click: the same point pressed twice, quickly
    int  m_last_hit = -1;
    sigc::signal<void()> m_changed, m_begin, m_end;
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    double to_x(double v) const;                  // -1..1 -> px
    double to_y(double t) const;                  // 0..1 -> px
    int hit(double x, double y) const;            // the point under (x, y), or -1
    void commit();                                // normalise; tell
};

}  // namespace klimt::widgets
