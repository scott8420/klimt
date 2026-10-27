#include "Shell.hpp"
#include "Log.hpp"
#include "core/Composite.hpp"

// Shell_paint.cpp -- PAINT (s008 m4). A Brush / Eraser stroke from the canvas.
//
// The press opens a journal GESTURE (the whole stroke = one step). Pointer
// moves become dab positions on the UI thread (cheap); the pixel work runs on
// a worker, ONE job at a time: it takes every dab still pending, adds them to
// the stroke's coverage and recomputes the touched tiles from the original
// (core::paint), and its landing writes the layer inside the gesture. When it
// lands, the next job takes what has piled up meanwhile -- so a fast drag
// never queues work behind itself, and the UI thread never paints a pixel.
// The release commits the gesture once the last job has landed.

namespace klimt {

namespace Pa = core::paint;

void Shell::on_stroke(CanvasView::StrokeAt at, core::select::Pt pt, bool shift, double pressure) {
    const Pa::Dab p{pt.x, pt.y, static_cast<float>(pressure)};
    using A = CanvasView::StrokeAt;
    if (at == A::Begin) {
        if (m_stroke.active) return;
        // s012: the pen's eraser end erases with the Eraser's own options.
        const bool erase = m_rail.tool() == "eraser" || m_canvas.pen_eraser();
        const auto o = m_strip.brush(erase ? "eraser" : "brush");
        Pa::Brush b;
        b.size = o.size;
        b.hardness = o.hardness / 100.0;
        b.opacity = o.opacity / 100.0;
        b.erase = erase;
        b.pressure_size = o.pressure_size;        // s009 m3: the pen
        b.pressure_opacity = o.pressure_opacity;
        b.flow = o.flow / 100.0;                  // s012 m1
        const auto fmt = m_doc.image().format;
        const auto fg = m_rail.foreground(), bg = m_rail.background();
        b.colour = core::from_srgb(fmt, fg.r, fg.g, fg.b);
        b.background = core::from_srgb(fmt, bg.r, bg.g, bg.b);
        const auto id = m_layers.primary();
        // s012: the layer's MASK is the target -- white shows, black hides; the
        // Eraser paints the background colour (Photoshop's way).
        const auto lm = m_doc.get(id);
        const bool on_mask = lm && lm->mask && m_layers.mask_target() == id;
        if (on_mask) {
            if (m_doc.locked_all(id)) { toast(std::string(erase ? "Eraser" : "Brush") + ": “" + lm->name + "” is locked"); return; }
            const auto mf = m_doc.mask_format();
            b.colour = pixel::Px{};
            b.colour.c[0] = mask_hide_of(erase ? bg : fg);
            b.erase = false;
            (void)mf;
        } else if (const auto why = Pa::refusal(m_doc, id, b); !why.empty()) {
            toast(std::string(erase ? "Eraser" : "Brush") + ": " + why);
            return;
        }
        const auto l = *m_doc.get(id);
        b.alpha_lock = !on_mask && (l.locks & core::lock::Alpha) != 0;
        m_stroke = {};
        m_stroke.active = true;
        m_stroke.id = id;
        m_stroke.brush = b;
        m_stroke.on_mask = on_mask;
        m_stroke.original = on_mask ? l.mask : std::make_shared<const pixel::TileMap>(l.pixels ? *l.pixels : m_doc.blank());
        m_stroke.current = m_stroke.original;
        m_stroke.cov = std::make_shared<const pixel::TileMap>(Pa::new_coverage(m_doc.image().width, m_doc.image().height));
        m_stroke.mask = m_doc.selection_ptr();
        m_stroke.carry = 1e9;   // the first dab lands at the press
        m_journal.begin(on_mask ? (erase ? "Eraser on Mask" : "Brush on Mask") : (erase ? "Eraser" : "Brush Stroke"), {id});
        // Shift-click: a straight line from where the last stroke ended.
        const bool line = shift && m_stroke_last_valid && m_stroke_last_id == id;
        const Pa::Dab from = line ? Pa::Dab{m_stroke_last.x, m_stroke_last.y, p.p} : p;
        const auto dabs = Pa::dabs_along(from, p, b, m_stroke.carry);
        m_stroke.pending.insert(m_stroke.pending.end(), dabs.begin(), dabs.end());
        m_stroke.last = p;
        // s012: Smoothing -- a string of up to 60 screen px, in image px.
        const double zoom = std::max(1e-3, m_canvas.zoom());
        m_stroke.smoother = Pa::Smoother(line ? 0.0 : o.smoothing / 100.0 * 60.0 / zoom);
        m_stroke.smoother.push(p);
        paint_pump();
        return;
    }
    if (!m_stroke.active) return;
    if (at == A::Move) {
        const auto q = m_stroke.smoother.push(p);   // where the brush is, after the string
        if (!q) return;
        const auto dabs = Pa::dabs_along(m_stroke.last, *q, m_stroke.brush, m_stroke.carry);
        m_stroke.pending.insert(m_stroke.pending.end(), dabs.begin(), dabs.end());
        m_stroke.last = *q;
        paint_pump();
        return;
    }
    // End: the brush catches up to the pen; finish once the work in flight has landed.
    m_stroke.smoother.push(p);
    if (const auto q = m_stroke.smoother.finish()) {
        const auto dabs = Pa::dabs_along(m_stroke.last, *q, m_stroke.brush, m_stroke.carry);
        m_stroke.pending.insert(m_stroke.pending.end(), dabs.begin(), dabs.end());
        m_stroke.last = *q;
    }
    m_stroke.ending = true;
    paint_pump();
}

void Shell::paint_pump() {
    if (!m_stroke.active || m_stroke.job) return;
    if (m_stroke.pending.empty()) {
        if (!m_stroke.ending) return;
        // The stroke is done: the gesture becomes ONE step.
        if (m_journal.in_gesture()) m_journal.commit();
        m_stroke_last = {m_stroke.last.x, m_stroke.last.y};
        m_stroke_last_id = m_stroke.id;
        m_stroke_last_valid = true;
        m_stroke = {};
        return;
    }
    auto dabs = std::move(m_stroke.pending);
    m_stroke.pending.clear();
    const auto im = m_doc.image();
    m_stroke.job = m_jobs.submit("Paint", [this, im, dabs = std::move(dabs), original = m_stroke.original,
                                           current = m_stroke.current, cov = m_stroke.cov, mask = m_stroke.mask,
                                           b = m_stroke.brush](core::Jobs::Token&) -> core::Jobs::Landing {
        core::TileSet touched = core::tiles_of(im);
        auto cov2 = std::make_shared<const pixel::TileMap>(Pa::add_pen_dabs(*cov, b, dabs, touched));
        auto cur2 = std::make_shared<const pixel::TileMap>(Pa::apply(*original, *current, *cov2, mask.get(), b, touched));
        return [this, cov2, cur2] {
            m_stroke.job = 0;
            // The stroke was ended from outside (an undo mid-stroke closed
            // the gesture): its work is dropped, nothing written around the door.
            if (!m_stroke.active || !m_journal.in_gesture()) { m_stroke = {}; return; }
            m_stroke.cov = cov2;
            m_stroke.current = cur2;
            if (auto l = m_doc.get(m_stroke.id)) {
                if (m_stroke.on_mask) l->mask = cur2;   // s012: a mask stays a mask, even with no tiles
                else l->pixels = cur2->tile_count() ? cur2 : nullptr;
                m_doc.put(m_stroke.id, *l);   // inside the gesture: the door is open
            }
            paint_pump();
        };
    });
}

}  // namespace klimt
