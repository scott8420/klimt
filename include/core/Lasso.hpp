#pragma once
// core::select::LassoTrace -- the Lasso's outline while it is drawn (s038).
// GTK-free; the canvas feeds it the pointer and Alt, the selftest walks it.
//
// Photoshop's Lasso: a freehand drag; hold Alt DURING the drag and the
// outline turns straight -- a rubber band from the last corner to the
// pointer, the button may be let go, and each click lays a corner. Let Alt go
// with the button down and it is freehand again from there; let Alt go with
// the button up, or Enter, a double-click, or a click back on the start,
// and it closes. Backspace takes the last corner; Esc drops the lot.
//
// Alt AT THE PRESS is the Subtract op (the modifiers at the press pick the
// op), so it does not also start a straight side: Alt must be let go once
// in the trace before it means "straight" (it is then "armed").
#include "core/Selection.hpp"

#include <vector>

namespace klimt::core::select {

class LassoTrace {
public:
    enum class State { Idle, Drawing, Open, Done, Cancelled };

    // `near`: how close (image px) counts as "on the start" / "the same
    // point" (the canvas passes a few screen px over the zoom); `step`: the
    // freehand's least move between points (image px).
    void start(Pt p, bool alt_at_press, double near, double step);
    void set_scale(double near, double step) { m_near = near; m_step = step; }

    // The button down while Open: a corner -- or the close.
    void press(Pt p, bool alt);
    // The pointer moved (button down: Drawing; up: Open, the band follows).
    void move(Pt p, bool alt);
    // The button let go.
    void release(Pt p, bool alt);
    void alt_up();          // Alt let go (the key, wherever it came from)
    void finish();          // Enter: close it as it is
    void cancel();          // Esc
    bool back();            // Backspace: the last corner (false: nothing to take)

    State state() const { return m_state; }
    bool active() const { return m_state == State::Drawing || m_state == State::Open; }
    bool open() const { return m_state == State::Open; }
    bool done() const { return m_state == State::Done; }
    bool straight() const { return m_straight; }     // a straight side is being laid
    bool went_straight() const { return m_went_straight; }
    const std::vector<Pt>& points() const { return m_pts; }
    Pt tip() const { return m_tip; }                 // the band's end (when straight())

private:
    void corner(Pt p);      // a corner, unless it's on the last one
    static double dist(Pt a, Pt b);

    State m_state = State::Idle;
    std::vector<Pt> m_pts;
    std::vector<std::size_t> m_corners;   // indices in m_pts that Backspace can take back to
    Pt m_tip{};
    bool m_straight = false, m_armed = false, m_went_straight = false;
    double m_near = 4, m_step = 1;
};

}  // namespace klimt::core::select
