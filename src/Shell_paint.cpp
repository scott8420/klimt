#include "Shell.hpp"
#include "Log.hpp"
#include "core/Composite.hpp"
#include "core/Heal.hpp"   // s015 m4

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

namespace {
// s015 m4: the very same tiles? (A stroke off the image, or a heal that found
// nothing, writes nothing -- and so leaves no step.)
bool same_tiles(const pixel::TileMap& a, const pixel::TileMap& b) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (int r = 0; r < a.rows(); ++r)
        for (int c = 0; c < a.cols(); ++c)
            if (!a.same_tile(b, c, r)) return false;
    return true;
}
}  // namespace

void Shell::on_stroke(CanvasView::StrokeAt at, core::select::Pt pt, bool shift, double pressure) {
    const Pa::Dab p{pt.x, pt.y, static_cast<float>(pressure)};
    using A = CanvasView::StrokeAt;
    if (at == A::Begin) {
        if (m_stroke.active) return;
        // s012: the pen's eraser end erases with the Eraser's own options.
        const bool erase = m_rail.tool() == "eraser" || m_canvas.pen_eraser();
        const bool clone = m_rail.tool() == "clone" && !erase;   // s015
        const bool heal = m_rail.tool() == "heal" && !erase;     // s015 m4
        const bool spot = heal && m_strip.variant("heal") == 0;
        const bool sampled = clone || heal;
        const std::string verb = erase ? "Eraser" : clone ? "Clone Stamp" : spot ? "Spot Healing" : heal ? "Healing Brush" : "Brush";
        if (sampled && !spot && !m_clone.set) { toast(verb + ": Alt-click (or Ctrl-click) where to take from first"); return; }
        const auto o = m_strip.brush(erase ? "eraser" : clone ? "clone" : heal ? "heal" : "brush");
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
        if (on_mask && sampled) {
            toast(verb + ": it paints pixels, not the mask -- Layer > Layer Mask > Paint the Mask / the Pixels");
            return;
        }
        const bool all_layers = sampled && m_strip.clone().all_layers;
        if (all_layers && m_doc.image().format.mode != pixel::Mode::RGB) {
            toast(verb + ": All layers samples RGB documents only, for now");
            return;
        }
        if (on_mask) {
            if (m_doc.locked_all(id)) { toast(std::string(erase ? "Eraser" : "Brush") + ": “" + lm->name + "” is locked"); return; }
            const auto mf = m_doc.mask_format();
            b.colour = pixel::Px{};
            b.colour.c[0] = mask_hide_of(erase ? bg : fg);
            b.erase = false;
            (void)mf;
        } else if (const auto why = Pa::refusal(m_doc, id, b); !why.empty()) {
            toast(verb + ": " + why);
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
        m_journal.begin(on_mask ? (erase ? "Eraser on Mask" : "Brush on Mask") : (sampled ? verb : erase ? "Eraser" : "Brush Stroke"), {id});
        // Shift-click: a straight line from where the last stroke ended.
        const bool line = shift && m_stroke_last_valid && m_stroke_last_id == id;
        const Pa::Dab from = line ? Pa::Dab{m_stroke_last.x, m_stroke_last.y, p.p} : p;
        // s015: the Clone Stamp -- the offset from the source; the source
        // itself the layer as the stroke began, or (All layers) the flattened
        // picture, made on a worker before any dab is painted.
        // s015 m4: a Heal stroke paints as a clone (Healing) or a grey trail
        // (Spot) while you drag, and heals when the last dab lands.
        if (sampled) {
            if (!spot) {
                m_clone.begin({from.x, from.y});
                m_stroke.brush.dx = m_clone.dx;
                m_stroke.brush.dy = m_clone.dy;
            }
            m_stroke.heal = heal;
            m_stroke.spot = spot;
            m_stroke.heal_brush = m_stroke.brush;
            if (spot) {
                m_stroke.brush.colour = core::from_srgb(fmt, 0.5, 0.5, 0.5);
                m_stroke.brush.opacity = 0.5;
            }
            if (!all_layers) {
                m_stroke.ref = m_stroke.original;
                if (!spot) m_stroke.brush.source = m_stroke.original;
            } else {
                const auto im = m_doc.image();
                auto layers = std::make_shared<const core::Layers>(m_doc.snapshot());
                m_stroke.source_job = m_jobs.submit("Clone source", [this, im, layers](core::Jobs::Token&) -> core::Jobs::Landing {
                    auto flat = core::file::flatten(im, *layers);
                    auto src = flat ? std::make_shared<const pixel::TileMap>(std::move(*flat)) : nullptr;
                    return [this, src] {
                        m_stroke.source_job = 0;
                        if (!m_stroke.active) return;
                        m_stroke.ref = src;
                        if (!m_stroke.spot) m_stroke.brush.source = src;
                        paint_pump();
                    };
                });
            }
            if (!spot) show_clone_marker();
        }
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
    if (!m_stroke.active || m_stroke.job || m_stroke.source_job) return;   // s015: the clone source first
    if (m_stroke.pending.empty()) {
        if (!m_stroke.ending) return;
        if (m_stroke.heal) { heal_finish(); return; }   // s015 m4: heal, then the step
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
            const bool unchanged = same_tiles(*cur2, *m_stroke.current);
            m_stroke.current = cur2;
            if (auto l = unchanged ? std::nullopt : m_doc.get(m_stroke.id)) {
                if (m_stroke.on_mask) l->mask = cur2;   // s012: a mask stays a mask, even with no tiles
                else l->pixels = cur2->tile_count() ? cur2 : nullptr;
                m_doc.put(m_stroke.id, *l);   // inside the gesture: the door is open
            }
            paint_pump();
        };
    });
}

// s015 m4: the Heal stroke's last dab has landed -- heal it on a worker (the
// colours from around the stroke, the texture from the source); its landing
// writes the layer inside the gesture, and the pump commits the ONE step.
void Shell::heal_finish() {
    m_stroke.heal = false;
    if (!m_stroke.ref) return;
    const auto im = m_doc.image();
    m_stroke.job = m_jobs.submit("Heal", [this, im, original = m_stroke.original, ref = m_stroke.ref, cov = m_stroke.cov,
                                          mask = m_stroke.mask, hb = m_stroke.heal_brush, spot = m_stroke.spot](core::Jobs::Token& t) -> core::Jobs::Landing {
        int dx = static_cast<int>(std::lround(hb.dx)), dy = static_cast<int>(std::lround(hb.dy));
        bool found = true;
        if (spot) {
            const auto o = core::heal::spot_offset(*ref, *cov, hb.size);
            if (o) { dx = o->dx; dy = o->dy; } else found = false;
        }
        std::shared_ptr<const pixel::TileMap> out = original;   // nothing fits: the layer as it was
        if (found) {
            auto healed = core::heal::healing_source(*ref, dx, dy, *cov, hb.size, [&t] { return t.cancelled(); });
            if (!healed) return [] {};
            Pa::Brush fb = hb;
            fb.source = std::make_shared<const pixel::TileMap>(std::move(*healed));
            fb.dx = fb.dy = 0;
            out = std::make_shared<const pixel::TileMap>(Pa::apply(*original, *original, *cov, mask.get(), fb, core::heal::covered(im, *cov)));
        }
        return [this, out, found] {
            m_stroke.job = 0;
            if (!m_stroke.active || !m_journal.in_gesture()) { m_stroke = {}; return; }
            if (!found) toast("Spot Healing: no clear place near enough to take from -- try a smaller stroke");
            if (auto l = same_tiles(*out, *m_stroke.current) ? std::nullopt : m_doc.get(m_stroke.id)) {
                l->pixels = out->tile_count() ? out : nullptr;
                m_doc.put(m_stroke.id, *l);   // inside the gesture: the door is open
            }
            paint_pump();
        };
    });
}

// s015: Alt- / Ctrl-click with the Clone Stamp -- where it takes from. View
// state, not a step (Photoshop's).
void Shell::on_clone_source(core::select::Pt p) {
    m_clone.aligned = m_strip.clone().aligned;
    m_clone.set_point(p);
    show_clone_marker();
    if (auto lg = log::get(log::Area::Shell)) lg->info("clone source: {:.0f}, {:.0f}", p.x, p.y);
}

void Shell::show_clone_marker() {
    m_canvas.set_clone_marker(m_clone.set, m_clone.point, m_clone.fixed, m_clone.dx, m_clone.dy);
}

}  // namespace klimt
