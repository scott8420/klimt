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

#include <array>
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
        // s010 m4: the same picture at 1/2, 1/4, ... (each its own w x h):
        // drawn when zoomed out, instead of scaling the whole tile down.
        struct Mip { int w = 0, h = 0; std::shared_ptr<const std::vector<std::uint32_t>> argb; };
        std::vector<Mip> mips;
    };
    static constexpr int kMips = 5;                               // down to 1/32 (8 px a tile)
    void set_tiles(const std::vector<Tile>& tiles);
    int  tiles_shown() const;                                     // tiles holding a picture

    // ── the view ────────────────────────────────────────────────────────────
    void fit();                        // the whole image in the window (follows resizes)
    void actual_size();                // 100%, centred where the view is
    void zoom_in();                    // the next step up, about the view's centre
    void zoom_out();
    double zoom() const { return m_zoom; }
    bool   fitting() const { return m_fit; }
    // s020: a document tab keeps its view (zoom, centre, fit) while parked.
    struct View { double zoom = 1, cx = 0, cy = 0; bool fit = true; };
    View   view() const { return {m_zoom, m_cx, m_cy, m_fit}; }
    void   set_view(const View& v);
    // The tile the view's centre is on, for ordering work (visible first).
    void   view_centre(double& x, double& y) const { x = m_cx; y = m_cy; }
    // s014: the image pixels the view shows (clipped to the image) -- what a
    // filter's preview works on.
    pixel::Rect visible() const;
    void set_tool(const std::string& id, int variant = 0) {
        if (id != "crop") m_crop = false;   // s009: the box belongs to the Crop tool
        m_tool = id; m_variant = variant; update_cursor(); queue_draw();
    }

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
    // s009 m3: the last argument is the pen's pressure, 0..1 (a mouse: 1).
    sigc::signal<void(StrokeAt, core::select::Pt, bool, double)>& signal_stroke() { return m_stroke_signal; }
    void set_brush_size(double px) { m_brush = px; queue_draw(); }   // the outline at the pointer
    // s017: the outline's shape -- the tip (core::paint::Tip as int), angle, roundness 0..1.
    void set_brush_shape(int tip, double angle, double roundness) { m_tip = tip; m_tip_angle = angle; m_tip_round = roundness; queue_draw(); }
    // s017: the pen's tilt in the event being handled (-1..1 each way; 0 for a mouse).
    void pen_tilt(float& x, float& y) const { x = m_tilt_x; y = m_tilt_y; }
    // s012 m1: the stroke under way is the pen's eraser end (the Shell erases).
    bool pen_eraser() const { return m_pen_eraser; }
    // s009 m1: the Move tool's drag: Begin, Move, End, each with the offset
    // from the press in WHOLE image pixels (Shift: along one axis only).
    enum class MoveAt { Begin, Move, End };
    sigc::signal<void(MoveAt, int, int)>& signal_move() { return m_move_signal; }
    // s016: the Heal tool's Patch -- a drag (image px offset) from the press.
    sigc::signal<void(MoveAt, int, int)>& signal_patch() { return m_patch_signal; }
    // s016: the ants drawn moved by (dx, dy) -- the patch being dragged.
    void set_ants_offset(int dx, int dy) { m_ants_dx = dx; m_ants_dy = dy; queue_draw(); }
    // s009 m2: the Crop tool's box. A drag draws it; dragging inside moves
    // it, near an edge or corner resizes it (Shift: square). Enter or a
    // double-click inside commits -> signal_crop (whole image pixels, may
    // reach past the canvas); Escape or another tool clears it.
    sigc::signal<void(core::select::Pt, core::select::Pt)>& signal_crop() { return m_crop_signal; }
    bool has_crop() const { return m_crop; }
    void clear_crop() { m_crop = false; m_crop_mode = CropMode::None; queue_draw(); }
    void set_crop(double x0, double y0, double x1, double y1);   // e.g. Crop to Selection's preview
    bool commit_crop();                                          // Enter: true if there was a box
    // s011 m3: the Crop tool's Straighten: drag along what should be level
    // (or upright); let go -> signal_straighten (image coords).
    sigc::signal<void(core::select::Pt, core::select::Pt)>& signal_straighten() { return m_straighten_signal; }
    // s012 m4: the Gradient tool's drag: where it starts and ends (Shift: 45° steps).
    sigc::signal<void(core::select::Pt, core::select::Pt)>& signal_gradient() { return m_gradient_signal; }
    // s011 m2: FREE TRANSFORM. While on, every left-drag on the canvas works
    // the box (whatever the tool): a handle scales (corners keep the
    // proportions; Shift frees them; Alt scales about the centre), inside
    // moves (Shift: one axis), outside rotates (Shift: 15-degree steps).
    // Each change emits signal_transform with the whole map (image coords);
    // a double-click inside asks to commit. The Shell decides Enter / Esc.
    void begin_transform(double x, double y, double w, double h);
    void end_transform();
    bool transforming() const { return m_xf.on; }
    void nudge_transform(double dx, double dy);                  // the arrows, in transform
    core::select::Pt transform_offset() const { return {m_xf.tx, m_xf.ty}; }
    sigc::signal<void(const std::array<double, 6>&)>& signal_transform() { return m_xf_signal; }   // a b c d e f
    sigc::signal<void()>& signal_transform_commit() { return m_xf_commit; }
    // s008 m3: the Eyedropper clicked at an image point; `background`: Alt.
    sigc::signal<void(int, int, bool)>& signal_pick() { return m_pick_signal; }
    // s015: Alt- / Ctrl-click with the Clone Stamp: the source point.
    sigc::signal<void(core::select::Pt)>& signal_source() { return m_source_signal; }
    // s015: the source's crosshair -- at `point` until a stroke fixes the
    // offset, then (fixed) the pointer + (dx, dy).
    void set_clone_marker(bool set, core::select::Pt point, bool fixed, double dx, double dy) {
        m_clone_set = set; m_clone_pt = point; m_clone_fixed = fixed; m_clone_dx = dx; m_clone_dy = dy; queue_draw();
    }
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
    bool painting() const { return m_tool == "brush" || m_tool == "eraser" || m_tool == "clone" || m_tool == "heal" || m_tool == "smudge"; }
    // s015 m4: the tools that take from a source point (Alt- / Ctrl-click).
    bool sourced() const { return m_tool == "clone" || (m_tool == "heal" && m_variant == 1); }
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
        std::vector<Cairo::RefPtr<Cairo::ImageSurface>> mips;   // s010 m4: 1/2, 1/4, ...
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
    sigc::signal<void(core::select::Pt)> m_source_signal;   // s015
    bool   m_clone_set = false, m_clone_fixed = false;
    core::select::Pt m_clone_pt;
    double m_clone_dx = 0, m_clone_dy = 0;
    sigc::signal<void(StrokeAt, core::select::Pt, bool, double)> m_stroke_signal;
    bool   m_stroking = false;
    bool   m_pen_eraser = false;                       // s012: this stroke is the pen's eraser end
    bool   m_moving = false;                           // s009: a Move drag in progress
    // s009 m2: the crop box (image coords, x0 < x1, y0 < y1 once let go).
    enum class CropMode { None, New, Move, Resize };
    bool     m_crop = false;
    double   m_cx0 = 0, m_cy0 = 0, m_cx1 = 0, m_cy1 = 0;
    double   m_crop_from[4] = {0, 0, 0, 0};             // the box at the press
    CropMode m_crop_mode = CropMode::None;
    int      m_crop_edges = 0;                          // 1 left, 2 right, 4 top, 8 bottom
    void     crop_press(double sx, double sy);
    void     crop_drag(double sx, double sy, bool shift);
    void     crop_release();
    void     draw_crop(const Cairo::RefPtr<Cairo::Context>& cr, int w, int h);
    int      crop_hit(double sx, double sy) const;      // edges under the pointer; -1 inside; 0 outside
    sigc::signal<void(core::select::Pt, core::select::Pt)> m_crop_signal;
    bool             m_line = false;                    // s011: a Straighten line being dragged
    core::select::Pt m_line_a, m_line_b;
    sigc::signal<void(core::select::Pt, core::select::Pt)> m_straighten_signal;
    bool             m_line_gradient = false;           // s012: the line is a gradient's
    sigc::signal<void(core::select::Pt, core::select::Pt)> m_gradient_signal;
    // s011 m2: the transform box. M = T(c + t) R(ang) S(sx, sy) T(-c).
    struct Xf {
        bool   on = false;
        double bx = 0, by = 0, bw = 0, bh = 0;          // the box, image coords
        double tx = 0, ty = 0, sx = 1, sy = 1, ang = 0;
        enum class Mode { None, Move, Scale, Rotate } mode = Mode::None;
        int    hx = 0, hy = 0;                           // the handle: -1 / 0 / 1 each
        double tx0 = 0, ty0 = 0, sx0 = 1, sy0 = 1, ang0 = 0;
        double px0 = 0, py0 = 0;                         // the press, image coords
    } m_xf;
    sigc::signal<void(const std::array<double, 6>&)> m_xf_signal;
    sigc::signal<void()> m_xf_commit;
    std::array<double, 6> xf_matrix() const;
    core::select::Pt xf_world(double lx, double ly) const;   // a box-centred local point -> image
    void xf_press(double sx, double sy);
    void xf_drag(double sx, double sy, bool shift, bool alt);
    bool xf_inside(double sx, double sy) const;
    void draw_transform(const Cairo::RefPtr<Cairo::Context>& cr);
    sigc::signal<void(MoveAt, int, int)> m_move_signal;
    sigc::signal<void(MoveAt, int, int)> m_patch_signal;   // s016
    bool   m_patching = false;
    int    m_ants_dx = 0, m_ants_dy = 0;
    double m_brush = 30;
    int    m_tip = 0;                                  // s017
    double m_tip_angle = 0, m_tip_round = 1;
    float  m_tilt_x = 0, m_tilt_y = 0;
};

}  // namespace klimt
