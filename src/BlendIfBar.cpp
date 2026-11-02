#include "widgets/BlendIfBar.hpp"

#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>

#include <algorithm>
#include <cmath>

namespace klimt::widgets {

namespace {
constexpr double kPad = 7;      // room for a triangle at either end
constexpr double kRamp = 12;    // the ramp's height
constexpr double kTri = 9;      // a triangle's height
}  // namespace

BlendIfBar::BlendIfBar(const char* name) {
    set_name(name);
    set_content_height(static_cast<int>(kRamp + kTri + 4));
    set_hexpand(true);
    set_draw_func(sigc::mem_fun(*this, &BlendIfBar::draw));
    set_tooltip_text("Drag a triangle to move it; Alt-drag to split it (a fade between the halves); double-click: all");

    auto drag = Gtk::GestureDrag::create();
    drag->signal_drag_begin().connect([this, drag](double x, double) {
        m_held = -1;
        m_x0 = x;
        for (int k = 0; k < 4; ++k) m_start[k] = at(k);
        // The nearest point; of a joined pair, both (Alt: one half, by the
        // first motion's side).
        double best = 1e9;
        for (int k = 0; k < 4; ++k) {
            const double d = std::abs(to_x(at(k)) - x);
            if (d < best - 0.5 || (std::abs(d - best) <= 0.5 && ((k == 1 && x > to_x(at(k))) || (k == 3 && x > to_x(at(k)))))) { best = d; m_held = k; }
        }
        if (best > 12) { m_held = -1; return; }
        const int pair = m_held < 2 ? 0 : 2;
        const bool joined = at(pair) == at(pair + 1);
        const bool alt = (drag->get_current_event_state() & Gdk::ModifierType::ALT_MASK) == Gdk::ModifierType::ALT_MASK;
        m_both = joined && !alt;
        m_split = joined && alt;
        if (m_both) m_held = pair;
    });
    drag->signal_drag_update().connect([this](double dx, double) {
        if (m_held < 0) return;
        if (m_split) {   // the side the half goes
            if (std::abs(dx) < 2) return;
            const int pair = m_held < 2 ? 0 : 2;
            m_held = dx < 0 ? pair : pair + 1;
            m_split = false;
        }
        const int v = to_v(to_x(m_start[m_held]) + dx);
        auto& r = m_r;
        if (m_both) {
            if (m_held == 0) { r.lo0 = r.lo1 = std::clamp(v, 0, r.hi0); }
            else             { r.hi0 = r.hi1 = std::clamp(v, r.lo1, 255); }
        } else {
            switch (m_held) {
                case 0: r.lo0 = std::clamp(v, 0, r.lo1); break;
                case 1: r.lo1 = std::clamp(v, r.lo0, r.hi0); break;
                case 2: r.hi0 = std::clamp(v, r.lo1, r.hi1); break;
                case 3: r.hi1 = std::clamp(v, r.hi0, 255); break;
            }
        }
        queue_draw();
        m_changed.emit();
    });
    drag->signal_drag_end().connect([this](double, double) {
        if (m_held >= 0) m_end.emit();
        m_held = -1;
    });
    add_controller(drag);
    auto click = Gtk::GestureClick::create();
    click->signal_pressed().connect([this](int n, double, double) {
        if (n != 2) return;
        m_r = core::BlendRange{};
        queue_draw();
        m_changed.emit();
        m_end.emit();
    });
    add_controller(click);
}

void BlendIfBar::set(const core::BlendRange& r, int channel) {
    if (m_held >= 0) return;   // mid-drag: ours is newer
    m_r = r;
    m_channel = channel;
    queue_draw();
}

int& BlendIfBar::at(int k) { return k == 0 ? m_r.lo0 : k == 1 ? m_r.lo1 : k == 2 ? m_r.hi0 : m_r.hi1; }

double BlendIfBar::to_x(int v) const {
    const double w = std::max(1, get_width());
    return kPad + (w - 2 * kPad) * v / 255.0;
}

int BlendIfBar::to_v(double x) const {
    const double w = std::max(1.0, get_width() - 2 * kPad);
    return std::clamp(static_cast<int>(std::lround((x - kPad) / w * 255.0)), 0, 255);
}

void BlendIfBar::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int) {
    auto g = Cairo::LinearGradient::create(kPad, 0, w - kPad, 0);
    const double tr = m_channel == 1 || m_channel == 0 ? 1 : 0, tg = m_channel == 2 || m_channel == 0 ? 1 : 0, tb = m_channel == 3 || m_channel == 0 ? 1 : 0;
    g->add_color_stop_rgb(0, 0, 0, 0);
    g->add_color_stop_rgb(1, tr, tg, tb);
    cr->rectangle(kPad, 1, w - 2 * kPad, kRamp);
    cr->set_source(g);
    cr->fill_preserve();
    cr->set_source_rgba(0.5, 0.5, 0.5, 0.8);
    cr->set_line_width(1);
    cr->stroke();
    // A triangle per point (a joined pair: one whole; split: two halves).
    const auto tri = [&](double x, int half, bool dark) {   // half: 0 whole, -1 left, +1 right
        const double top = kRamp + 2, base = top + kTri, hw = kTri * 0.7;
        cr->move_to(x, top);
        if (half <= 0) cr->line_to(x - hw, base); else cr->line_to(x, base);
        if (half >= 0) cr->line_to(x + hw, base); else cr->line_to(x, base);
        cr->close_path();
        cr->set_source_rgb(dark ? 0.1 : 0.95, dark ? 0.1 : 0.95, dark ? 0.1 : 0.95);
        cr->fill_preserve();
        cr->set_source_rgb(0.55, 0.55, 0.55);
        cr->stroke();
    };
    if (m_r.lo0 == m_r.lo1) tri(to_x(m_r.lo0), 0, true);
    else { tri(to_x(m_r.lo0), -1, true); tri(to_x(m_r.lo1), 1, true); }
    if (m_r.hi0 == m_r.hi1) tri(to_x(m_r.hi0), 0, false);
    else { tri(to_x(m_r.hi0), -1, false); tri(to_x(m_r.hi1), 1, false); }
}

}  // namespace klimt::widgets
