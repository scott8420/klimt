// core::refine::Session -- Refine to Edges' memory (s038). See the header.
#include "core/Refine.hpp"

#include <algorithm>
#include <cmath>

namespace klimt::core::refine {

void add_stroke(path::Hints& h, const path::Stroke& s) {
    const auto crosses = [&](const path::Stroke& e) {
        const double reach = e.radius + s.radius;
        for (const auto& a : e.pts)
            for (const auto& b : s.pts)
                if (std::hypot(a.x - b.x, a.y - b.y) <= reach) return true;
        return false;
    };
    std::erase_if(h.strokes, [&](const path::Stroke& e) { return e.figure != s.figure && crosses(e); });
    h.strokes.push_back(s);
}

void Session::start(Mask rough, Mask now, path::Hints hints) {
    *this = {};
    m_on = true;
    m_rough = std::move(rough);
    m_hints = std::move(hints);
    m_made.push_back({std::move(now), m_hints});
}

bool Session::stroke(const path::Stroke& s, const Mask& now) {
    // Where is `now` among the selections this session made? The latest
    // match: the same pointer can't come twice, but be safe.
    std::size_t at = m_made.size();
    if (m_on)
        for (std::size_t k = m_made.size(); k-- > 0;)
            if (m_made[k].sel == now) { at = k; break; }
    const bool carried = at < m_made.size();
    if (!carried) start(now, now);   // something else: a fresh session from it
    else if (at + 1 < m_made.size()) {   // Ctrl+Z went back: the strokes go back too
        m_hints = m_made[at].hints;
        m_made.resize(at + 1);
    }
    // at == the last: keep every stroke (one may still be running)
    add_stroke(m_hints, s);
    return carried;
}

void Session::landed(Mask sel, path::Hints hints) {
    if (!m_on) return;
    m_made.push_back({std::move(sel), std::move(hints)});
}

}  // namespace klimt::core::refine
