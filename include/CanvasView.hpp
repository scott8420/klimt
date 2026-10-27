#pragma once
// CanvasView -- where the image goes (s005, docs/ui.md 1). The surround is a
// NEUTRAL grey that does not follow the theme's tint (you judge colour against
// it); the image floats on it with a soft edge. Focusable: Tab on the canvas
// hides the panels (the Shell hears it).
//
// s007 m4 -- ZOOM AND PAN, and the picture as TILES:
//   * the composite arrives as 256 x 256 ARGB tiles (set_tiles), each landed
//     when its worker finishes -- a change re-composites only the tiles it
//     touched (core::changed_tiles), and a big image fills in as it's made;
//   * the view: a zoom and the image point at the window's centre. Fit (the
//     default, and Ctrl+0) follows the window's size; any zoom or pan leaves
//     fit. Only the tiles in view are drawn, each snapped to whole device
//     pixels so no seam shows between them;
//   * Ctrl+scroll zooms at the pointer; scroll pans (Shift: sideways);
//     middle-drag, Space+drag and the Hand tool pan; the Zoom tool clicks in
//     (Alt: out); keys through zoom_in / zoom_out / fit / actual_size.
#include "core/Selection.hpp"   // s008: the ants' segments, the op

#include <gtkmm/drawingarea.h>
#include <cairomm/surface.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace klimt {

class CanvasView : public Gtk::DrawingArea {
public:
    CanvasView();
    void set_dark(bool dark);

    // A document of this size (a new or opened one, or a resize): the tiles
    // empty, and the view fits it if the size changed.
    void set_document(int width, int height);
    struct Tile {
        int col = 0, row = 0, w = 0, h = 0;                       // w x h: the part inside the image
        std::shared_ptr<const std::vector<std::uint32_t>> argb;   // Cairo ARGB32, rows of w
    };
    void set_tiles(const std::vector<Tile>& tiles);
    int  tiles_shown() const;                                     // tiles holding a picture

    // ── the view ────────────────────────────────────────────────────────────
    void fit();                        // the whole image in the window (follows resizes)
    void actual_size();                // 100%, centred where the view is
    void zoom_in();                    // the next step up, about the view's centre
    void zoom_out();
    double zoom() const { return m_zoom; }
    bool   fitting() const { return m_fit; }
    // The tile the view's centre is on, for ordering work (visible first).
    void   view_centre(double& x, double& y) const { x = m_cx; y = m_cy; }
    void set_tool(const std::string& id, int variant = 0) { m_tool = id; m_variant = variant; update_cursor(); queue_draw(); }

    // ── s008: the selection on the canvas ──────────────────────────────────
    // The marching ants: the selection's outline (image coords), animated
    // while there is one. nullptr / empty: no ants.
    void set_outline(std::shared_ptr<const std::vector<core::select::Seg>> segs);
    // A marquee or lasso the user finished (Select tool): its kind, its
    // points in image coords (a rectangle / ellipse: two corners), and the
    // op the modifiers at the press chose. Kind None: a click without a drag
    // (Photoshop's "click to deselect").
    struct Shape {
        enum class Kind { None, Rect, Ellipse, Lasso, Point } kind = Kind::None;   // Point: the Wand's click
        std::vector<core::select::Pt> pts;
        core::select::Op op = core::select::Op::Replace;
    };
    sigc::signal<void(const Shape&)>& signal_shape() { return m_shape_signal; }
    // s008 m4: a Brush / Eraser stroke: Begin (with Shift: a straight line
    // from where the last stroke ended), Move, End. Image coords.
    enum class StrokeAt { Begin, Move, End };
    sigc::signal<void(StrokeAt, core::select::Pt, bool)>& signal_stroke() { return m_stroke_signal; }
    void set_brush_size(double px) { m_brush = px; queue_draw(); }   // the outline at the pointer
    // s008 m3: the Eyedropper clicked at an image point; `background`: Alt.
    sigc::signal<void(int, int, bool)>& signal_pick() { return m_pick_signal; }
    // Told the zoom whenever it changes: 0.37 -> "37%".
    void set_on_zoom(std::function<void(double)> f) { m_on_zoom = std::move(f); }

private:
    void draw(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void layout(int w, int h);                          // fit: the zoom for this size
    void zoom_at(double z, double sx, double sy);        // keep the image point under (sx, sy)
    void pan_by(double dx, double dy);                   // screen pixels
    void clamp_centre();
    void tell_zoom();
    void update_cursor();
    void draw_ants(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    void draw_rubber(const Cairo::RefPtr<Cairo::Context>& cr);
    bool selecting() const { return m_tool == "select" && m_variant <= 3; }   // Rectangle, Ellipse, Lasso, Wand
    bool painting() const { return m_tool == "brush" || m_tool == "eraser"; }
    double to_image_x(double sx) const { return m_cx + (sx - m_vw / 2.0) / m_zoom; }
    double to_image_y(double sy) const { return m_cy + (sy - m_vh / 2.0) / m_zoom; }
    double to_screen_x(double ix) const { return m_vw / 2.0 + (ix - m_cx) * m_zoom; }
    double to_screen_y(double iy) const { return m_vh / 2.0 + (iy - m_cy) * m_zoom; }

    bool   m_dark = true;
    int    m_iw = 0, m_ih = 0;                           // the image
    int    m_cols = 0, m_rows = 0;
    // A tile on screen: a surface whose memory CAIRO owns (s008: aliasing a
    // shared buffer let the buffer die before the surface -- Cairo copies
    // from it when a surface with a snapshot is destroyed; ASan caught it).
    struct Held {
        int w = 0, h = 0;
        Cairo::RefPtr<Cairo::ImageSurface> surface;
    };
    std::vector<Held> m_grid;                            // cols * rows
    Cairo::RefPtr<Cairo::ImageSurface> m_checker;        // the transparency pattern, one cell pair

    // The view: m_zoom screen px per image px; (m_cx, m_cy) the image point
    // at the window's centre. m_fit: follow the window.
    double m_zoom = 1, m_cx = 0, m_cy = 0;
    bool   m_fit = true;
    int    m_vw = 0, m_vh = 0;
    double m_told = -1;
    double m_px = -1, m_py = -1;                         // the pointer, for Ctrl+scroll
    bool   m_space = false, m_panning = false;
    double m_pan_cx = 0, m_pan_cy = 0;
    std::string m_tool;
    int         m_variant = 0;
    std::function<void(double)> m_on_zoom;

    // s008: the ants, and the shape being dragged.
    std::shared_ptr<const std::vector<core::select::Seg>> m_ants;
    double            m_ant_phase = 0;
    sigc::connection  m_ant_timer;
    bool              m_rubber = false;               // a marquee / lasso in progress
    Shape             m_drag;                          // its points so far (image coords)
    double            m_press_x = 0, m_press_y = 0;    // screen
    sigc::signal<void(const Shape&)> m_shape_signal;
    sigc::signal<void(int, int, bool)> m_pick_signal;
    sigc::signal<void(StrokeAt, core::select::Pt, bool)> m_stroke_signal;
    bool   m_stroking = false;
    double m_brush = 30;
};

}  // namespace klimt
