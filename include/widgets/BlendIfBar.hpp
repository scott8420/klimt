#pragma once
// s072 -- BLEND IF's slider (Photoshop's Blending Options): a ramp from
// black to white (or to the channel's colour) with two triangles under it,
// the black point and the white point. Drag one to move it; Alt-drag it to
// SPLIT it -- the two halves then move apart (the fade between them). A
// split triangle's halves are grabbed one at a time. Double-click: all
// passing again. core::BlendRange holds the four points.
#include "core/Document.hpp"

#include <gtkmm/drawingarea.h>
#include <sigc++/signal.h>

namespace klimt::widgets {

class BlendIfBar : public Gtk::DrawingArea {
public:
    explicit BlendIfBar(const char* name);
    void set(const core::BlendRange& r, int channel);   // channel 0 gray, 1..3 R G B: the ramp's colour
    const core::BlendRange& value() const { return m_r; }
    sigc::signal<void()>& signal_changed() { return m_changed; }
    sigc::signal<void()>& signal_end() { return m_end; }   // a drag let go

private:
    core::BlendRange m_r;
    int    m_channel = 0;
    int    m_held = -1;          // 0 lo0, 1 lo1, 2 hi0, 3 hi1; -1 none
    bool   m_both = false;       // a joined triangle held: both halves move
    bool   m_split = false;      // Alt: a joined triangle to split, its side chosen by the first motion
    double m_x0 = 0;
    int    m_start[4] = {0, 0, 255, 255};
    sigc::signal<void()> m_changed, m_end;
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    double to_x(int v) const;
    int    to_v(double x) const;
    int&   at(int k);
};

}  // namespace klimt::widgets
