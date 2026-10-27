#include "CanvasView.hpp"

#include <gtkmm/gestureclick.h>
#include <glibmm/main.h>
#include <pangomm/layout.h>

#include <algorithm>

namespace klimt {

CanvasView::CanvasView() {
    set_name("canvas");
    set_hexpand(true);
    set_vexpand(true);
    set_focusable(true);
    set_draw_func(sigc::mem_fun(*this, &CanvasView::draw));
    // A click on the canvas takes the keyboard (so Tab, tool keys land here).
    auto click = Gtk::GestureClick::create();
    click->signal_pressed().connect([this](int, double, double) { grab_focus(); });
    add_controller(click);
}

void CanvasView::set_dark(bool dark) {
    if (dark == m_dark) return;
    m_dark = dark;
    queue_draw();
}

void CanvasView::set_image(int width, int height, std::shared_ptr<const std::vector<std::uint32_t>> argb) {
    m_iw = width;
    m_ih = height;
    m_argb = std::move(argb);
    m_surface.reset();
    if (m_argb && width > 0 && height > 0 &&
        m_argb->size() >= static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        // Cairo reads the shared buffer in place (it is never written again:
        // a new composite is a new buffer).
        auto* data = reinterpret_cast<unsigned char*>(const_cast<std::uint32_t*>(m_argb->data()));
        m_surface = Cairo::ImageSurface::create(data, Cairo::Surface::Format::ARGB32, width, height, width * 4);
    }
    queue_draw();
}

void CanvasView::draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
    // The surround: neutral (R = G = B), darker under a dark UI.
    const double g = m_dark ? 0.22 : 0.62;
    cr->set_source_rgb(g, g, g);
    cr->paint();

    // The sheet: the image's aspect (3:4 until there is one), fitted.
    const double margin = 48;
    const double aw = std::max(0.0, w - 2 * margin), ah = std::max(0.0, h - 2 * margin);
    const double aspect = (m_iw > 0 && m_ih > 0) ? static_cast<double>(m_ih) / m_iw : 4.0 / 3.0;
    double sw = aw, sh = aw * aspect;
    if (sh > ah) { sh = ah; sw = ah / aspect; }
    const double x = (w - sw) / 2, y = (h - sh) / 2;
    if (sw < 8 || sh < 8) return;

    // A soft edge: a few widening translucent rings.
    for (int i = 6; i >= 1; --i) {
        cr->set_source_rgba(0, 0, 0, 0.05);
        cr->rectangle(x - i, y - i + 3, sw + 2 * i, sh + 2 * i);
        cr->fill();
    }
    // Transparency: the checkerboard.
    cr->save();
    cr->rectangle(x, y, sw, sh);
    cr->clip();
    const double cell = 10;
    for (double cy = y; cy < y + sh; cy += cell)
        for (double cx = x; cx < x + sw; cx += cell) {
            const bool dark = (static_cast<int>((cx - x) / cell) + static_cast<int>((cy - y) / cell)) % 2;
            const double v = dark ? 0.80 : 0.96;
            cr->set_source_rgb(v, v, v);
            cr->rectangle(cx, cy, cell, cell);
            cr->fill();
        }
    cr->restore();

    if (m_surface) {
        const double zoom = sw / m_iw;
        cr->save();
        cr->rectangle(x, y, sw, sh);
        cr->clip();
        cr->translate(x, y);
        cr->scale(zoom, zoom);
        cr->set_source(m_surface, 0, 0);
        auto pat = cr->get_source_for_surface();
        if (pat) pat->set_filter(zoom < 1 ? Cairo::SurfacePattern::Filter::GOOD : Cairo::SurfacePattern::Filter::NEAREST);
        cr->paint();
        cr->restore();
        if (zoom != m_zoom) {
            m_zoom = zoom;
            // NOT from inside draw: the listener relabels the status line, and
            // a resize queued mid-frame left it unallocated and blank (seen
            // under Xvfb, s006: "snapshot ... without a current allocation").
            if (m_on_zoom) Glib::signal_idle().connect_once([this, zoom] { if (m_on_zoom) m_on_zoom(zoom); });
        }
        return;
    }

    auto layout = create_pango_layout("");
    layout->set_markup("<span size='large' weight='600'>No pixels yet</span>\n"
                       "<span size='small'>The format waits on docs/pixels.md</span>");
    layout->set_alignment(Pango::Alignment::CENTER);
    int lw = 0, lh = 0;
    layout->get_pixel_size(lw, lh);
    cr->set_source_rgba(0.25, 0.25, 0.25, 0.9);
    cr->move_to(x + (sw - lw) / 2, y + (sh - lh) / 2);
    layout->show_in_cairo_context(cr);
}

}  // namespace klimt
