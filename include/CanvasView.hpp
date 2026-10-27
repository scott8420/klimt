#pragma once
// CanvasView -- where the image goes (s005, docs/ui.md 1). The surround is a
// NEUTRAL grey that does not follow the theme's tint (you judge colour against
// it); the image floats on it with a soft edge. Until there are pixels it
// shows the sheet's place, honestly empty. Focusable: Tab on the canvas hides
// the panels (the Shell hears it).
#include <gtkmm/drawingarea.h>
#include <cairomm/surface.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace klimt {

class CanvasView : public Gtk::DrawingArea {
public:
    CanvasView();
    void set_dark(bool dark);
    // The composite to show (s006): Cairo ARGB32, width x height. Shared so
    // the surface can point at it without a copy. nullptr: no pixels.
    void set_image(int width, int height, std::shared_ptr<const std::vector<std::uint32_t>> argb);
    // Told the zoom (the fit's scale) whenever it changes: "66%".
    void set_on_zoom(std::function<void(double)> f) { m_on_zoom = std::move(f); }

private:
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    bool m_dark = true;
    int  m_iw = 0, m_ih = 0;
    std::shared_ptr<const std::vector<std::uint32_t>> m_argb;
    Cairo::RefPtr<Cairo::ImageSurface> m_surface;
    double m_zoom = 0;
    std::function<void(double)> m_on_zoom;
};

}  // namespace klimt
