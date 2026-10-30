// core::select::LassoTrace -- the Lasso's outline while drawn (s038). See the header.
#include "core/Lasso.hpp"

#include <cmath>

namespace klimt::core::select {

double LassoTrace::dist(Pt a, Pt b) { return std::hypot(a.x - b.x, a.y - b.y); }

void LassoTrace::start(Pt p, bool alt_at_press, double near, double step) {
    *this = {};
    m_near = near;
    m_step = step;
    m_state = State::Drawing;
    m_pts = {p};
    m_tip = p;
    m_armed = !alt_at_press;
}

void LassoTrace::corner(Pt p) {
    if (!m_pts.empty() && dist(p, m_pts.back()) < 1e-9) {   // on the last point: that point is the corner
        m_corners.push_back(m_pts.size() - 1);
        return;
    }
    m_corners.push_back(m_pts.size());
    m_pts.push_back(p);
}

void LassoTrace::press(Pt p, bool alt) {
    if (m_state != State::Open) return;
    if (!alt) m_armed = true;
    // Back on the start, or on the last corner (a double-click): close.
    if ((m_pts.size() >= 3 && dist(p, m_pts.front()) <= m_near) || dist(p, m_pts.back()) <= m_near) {
        m_straight = false;
        m_state = State::Done;
        return;
    }
    corner(p);
    m_tip = p;
    m_state = State::Drawing;
    m_straight = alt && m_armed;   // still Alt: the next side is straight too
}

void LassoTrace::move(Pt p, bool alt) {
    if (m_state == State::Open) {
        m_tip = p;
        if (!alt) alt_up();   // Alt went while the keys were elsewhere
        return;
    }
    if (m_state != State::Drawing) return;
    if (!alt) m_armed = true;
    const bool str = alt && m_armed;
    if (str && !m_straight) {          // Alt down: a corner here, then the band
        corner(p);
        m_straight = m_went_straight = true;
    } else if (!str && m_straight) {   // Alt up, button down: the side lands, freehand on
        corner(p);
        m_straight = false;
    } else if (!str) {
        if (dist(p, m_pts.back()) >= m_step) m_pts.push_back(p);
    }
    m_tip = p;
}

void LassoTrace::release(Pt p, bool alt) {
    if (m_state != State::Drawing) return;
    if (!alt) m_armed = true;
    if (alt && m_armed) {   // Alt held: the corner lands, the trace stays open
        corner(p);
        m_tip = p;
        m_straight = m_went_straight = true;
        m_state = State::Open;
        return;
    }
    if (m_straight) corner(p);
    else if (dist(p, m_pts.back()) > 1e-9) m_pts.push_back(p);
    m_straight = false;
    m_state = State::Done;
}

void LassoTrace::alt_up() {
    if (m_state == State::Open) { m_straight = false; m_state = State::Done; }
    // Drawing: the next move() sees Alt up and goes freehand.
}

void LassoTrace::finish() {
    if (!active()) return;
    if (m_state == State::Drawing && m_straight) corner(m_tip);
    m_straight = false;
    m_state = State::Done;
}

void LassoTrace::cancel() {
    if (!active()) return;
    m_state = State::Cancelled;
    m_straight = false;
}

bool LassoTrace::back() {
    if (!active() || m_corners.empty()) return false;
    const std::size_t at = m_corners.back();
    m_corners.pop_back();
    if (at == 0) return false;   // the start stays
    m_pts.resize(at);
    while (!m_corners.empty() && m_corners.back() >= m_pts.size()) m_corners.pop_back();
    return true;
}

}  // namespace klimt::core::select
