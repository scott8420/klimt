#include "widgets/ShearCurve.hpp"

#include <gtkmm/gesturedrag.h>

#include <algorithm>
#include <cmath>

namespace klimt::widgets {

namespace F = core::filter;
namespace {
constexpr double kPad = 10;   // the box's margin, px
}

ShearCurve::ShearCurve(const char* name) {
    set_name(name);
    set_content_width(200);
    set_content_height(200);
    set_halign(Gtk::Align::CENTER);
    set_cursor("crosshair");
    set_tooltip_text("Click the line: a point; drag it across to shear the rows; drag it out sideways, or double-click it, to take it away");
    set_draw_func(sigc::mem_fun(*this, &ShearCurve::draw));
    auto drag = Gtk::GestureDrag::create();
    drag->signal_drag_begin().connect([this](double x, double y) {
        const double w = get_width(), h = get_height();
        const double t = std::clamp((y - kPad) / std::max(1.0, h - 2 * kPad), 0.0, 1.0);
        const double v = std::clamp((x - w / 2) / std::max(1.0, (w - 2 * kPad) / 2), -1.0, 1.0);
        m_begin.emit();
        m_drag = hit(x, y);
        m_out = false;
        // A second press on the same point soon after: a double-click -- it goes
        // (the drag takes the presses, so a click gesture never sees the second).
        const gint64 now = g_get_monotonic_time();
        const bool twice = m_drag >= 0 && m_drag == m_last_hit && now - m_last_press < 400000;
        m_last_press = now;
        m_last_hit = m_drag;
        if (twice && m_drag > 0 && m_drag + 1 < static_cast<int>(m_f.shear.size())) {
            m_f.shear.erase(m_f.shear.begin() + m_drag);
            m_drag = -1;
            m_last_hit = -1;
            commit();
            return;
        }
        if (m_drag < 0 && static_cast<int>(m_f.shear.size()) < core::filter::kShearPoints) {   // a new point, on the line where clicked
            m_f.shear.push_back({t, v});
            std::stable_sort(m_f.shear.begin(), m_f.shear.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
            m_drag = F::shear_hit(m_f, t, v, 1e-9, 1e-9);
            commit();
        }
    });
    drag->signal_drag_update().connect([this, drag](double dx, double dy) {
        if (m_drag < 0 || m_drag >= static_cast<int>(m_f.shear.size())) return;
        double sx = 0, sy = 0;
        drag->get_start_point(sx, sy);
        const double x = sx + dx, y = sy + dy, w = get_width(), h = get_height();
        auto& p = m_f.shear[static_cast<std::size_t>(m_drag)];
        const bool end = m_drag == 0 || m_drag + 1 == static_cast<int>(m_f.shear.size());
        m_out = !end && (x < -20 || x > w + 20);
        p[1] = std::clamp((x - w / 2) / std::max(1.0, (w - 2 * kPad) / 2), -1.0, 1.0);
        if (!end) {   // between its neighbours
            const double lo = m_f.shear[static_cast<std::size_t>(m_drag - 1)][0] + 0.011, hi = m_f.shear[static_cast<std::size_t>(m_drag + 1)][0] - 0.011;
            p[0] = std::clamp((y - kPad) / std::max(1.0, h - 2 * kPad), std::min(lo, hi), std::max(lo, hi));
        }
        queue_draw();
        if (!m_out) m_changed.emit();
    });
    drag->signal_drag_end().connect([this](double, double) {
        if (m_out && m_drag > 0 && m_drag + 1 < static_cast<int>(m_f.shear.size())) m_f.shear.erase(m_f.shear.begin() + m_drag);
        m_drag = -1;
        m_out = false;
        commit();
        m_end.emit();
    });
    add_controller(drag);
}

void ShearCurve::set(const core::Filter& f) {
    m_f = f;
    m_f.kind = core::FilterKind::Shear;
    m_f = F::normalised(m_f);
    queue_draw();
}

double ShearCurve::to_x(double v) const { return get_width() / 2.0 + v * (get_width() - 2 * kPad) / 2; }
double ShearCurve::to_y(double t) const { return kPad + t * (get_height() - 2 * kPad); }

int ShearCurve::hit(double x, double y) const {
    const double w = get_width(), h = get_height();
    const double t = (y - kPad) / std::max(1.0, h - 2 * kPad), v = (x - w / 2) / std::max(1.0, (w - 2 * kPad) / 2);
    return F::shear_hit(m_f, t, v, 7 / std::max(1.0, h - 2 * kPad), 7 / std::max(1.0, (w - 2 * kPad) / 2));
}

void ShearCurve::commit() {
    m_f = F::normalised(m_f);
    queue_draw();
    m_changed.emit();
}

void ShearCurve::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    cr->set_source_rgb(0.12, 0.12, 0.13);
    cr->rectangle(0, 0, w, h);
    cr->fill();
    // The grid: quarters; the middle line brighter.
    cr->set_line_width(1);
    for (int k = 0; k <= 4; ++k) {
        cr->set_source_rgba(0.85, 0.85, 0.88, k == 2 ? 0.35 : 0.12);
        const double gx = std::round(kPad + k * (w - 2 * kPad) / 4) + 0.5, gy = std::round(kPad + k * (h - 2 * kPad) / 4) + 0.5;
        cr->move_to(gx, kPad); cr->line_to(gx, h - kPad);
        cr->move_to(kPad, gy); cr->line_to(w - kPad, gy);
        cr->stroke();
    }
    // The curve.
    cr->set_source_rgb(0.35, 0.65, 1.0);
    cr->set_line_width(2);
    for (int i = 0; i <= 100; ++i) {
        const double t = i / 100.0, x = to_x(F::shear_at(m_f, t)), y = to_y(t);
        if (i == 0) cr->move_to(x, y); else cr->line_to(x, y);
    }
    cr->stroke();
    // The points (the held one filled; one held out of the box: red).
    for (std::size_t i = 0; i < m_f.shear.size(); ++i) {
        const double x = to_x(m_f.shear[i][1]), y = to_y(m_f.shear[i][0]);
        cr->begin_new_sub_path();
        cr->arc(x, y, 4, 0, 2 * 3.14159265358979);
        if (static_cast<int>(i) == m_drag) {
            if (m_out) cr->set_source_rgb(0.9, 0.3, 0.3); else cr->set_source_rgb(1, 1, 1);
            cr->fill();
        } else {
            cr->set_source_rgb(1, 1, 1);
            cr->set_line_width(1.5);
            cr->stroke();
        }
    }
    cr->set_source_rgba(1, 1, 1, 0.35);
    cr->set_line_width(1);
    cr->rectangle(0.5, 0.5, w - 1, h - 1);
    cr->stroke();
}

}  // namespace klimt::widgets
