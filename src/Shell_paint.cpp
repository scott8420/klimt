#include "Shell.hpp"
#include "core/Patterns.hpp"   // s033: the brush's texture
#include "core/ColourChannels.hpp"   // s026
#include "Log.hpp"
#include "core/Composite.hpp"
#include "core/Heal.hpp"   // s015 m4

#include <chrono>

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
// s018 m4: the tools that only ever paint pixels (not a mask).
bool erase_only_pixels(bool sampled, bool smudge) { return sampled || smudge; }
// s028: a seed for a stroke's dice -- the clock and a count, so two strokes
// in the same tick still differ.
std::uint64_t stroke_seed() {
    static std::uint64_t n = 0;
    const auto t = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return (t ^ (++n * 0x9E3779B97F4A7C15ULL)) | 1;
}
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
    Pa::Dab p{pt.x, pt.y, static_cast<float>(pressure)};
    m_canvas.pen_tilt(p.tx, p.ty);   // s017: the pen's lean (0 for a mouse)
    using A = CanvasView::StrokeAt;
    if (at == A::Begin) {
        if (m_stroke.active) return;
        // s012: the pen's eraser end erases with the Eraser's own options.
        const bool erase = m_rail.tool() == "eraser" || m_canvas.pen_eraser();
        const bool clone = m_rail.tool() == "clone" && !erase;   // s015
        const bool heal = m_rail.tool() == "heal" && !erase;     // s015 m4
        const bool spot = heal && m_strip.variant("heal") == 0;
        const bool sampled = clone || heal;
        const bool smudge = m_rail.tool() == "smudge" && !erase;   // s017
        const std::string verb = erase ? "Eraser" : clone ? "Clone Stamp" : spot ? "Spot Healing" : heal ? "Healing Brush" : smudge ? "Smudge" : "Brush";
        if (sampled && !spot && !m_clone.set) { toast(verb + ": Alt-click (or Ctrl-click) where to take from first"); return; }
        const auto o = m_strip.brush(erase ? "eraser" : clone ? "clone" : heal ? "heal" : smudge ? "smudge" : "brush");
        Pa::Brush b;
        b.size = o.size;
        b.hardness = o.hardness / 100.0;
        b.opacity = o.opacity / 100.0;
        b.erase = erase;
        b.pressure_size = o.pressure_size;        // s009 m3: the pen
        b.pressure_opacity = o.pressure_opacity;
        b.flow = o.flow / 100.0;                  // s012 m1
        b.tip = static_cast<Pa::Tip>(std::clamp(o.tip, 0, 2));   // s017: the tip
        b.angle = o.angle;
        b.roundness = o.roundness / 100.0;
        b.tilt = o.tilt;
        b.strength = o.strength / 100.0;          // s017: Smudge
        // s028: the dynamics
        b.dyn.size_jitter = o.size_jitter / 100.0;
        b.dyn.min_size = o.min_size / 100.0;
        b.dyn.angle_jitter = o.angle_jitter / 100.0;
        b.dyn.roundness_jitter = o.roundness_jitter / 100.0;
        b.dyn.min_roundness = o.min_roundness / 100.0;
        b.dyn.scatter = o.scatter / 100.0;
        b.dyn.both_axes = o.both_axes;
        b.dyn.count = o.count;
        b.dyn.count_jitter = o.count_jitter / 100.0;
        // s033: the texture -- a built-in by key, or yours from the library.
        if (o.tex_on) {
            b.texture.on = true;
            b.texture.scale = o.tex_scale / 100.0;
            b.texture.depth = o.tex_depth / 100.0;
            b.texture.subtract = o.tex_subtract;
            b.texture.invert = o.tex_invert;
            if (o.tex_pattern.rfind("u:", 0) == 0) {
                b.texture.image = core::pattern::find(o.tex_pattern.substr(2));
                if (!b.texture.image) b.texture.on = false;   // gone from the library
            } else b.texture.builtin = std::max(0, core::pattern::index_of(o.tex_pattern));
        }
        if (!erase && !clone && !heal && !smudge) {   // colour: the Brush's own
            b.dyn.fg_bg = o.fg_bg / 100.0;
            b.dyn.hue = o.hue / 100.0;
            b.dyn.sat = o.sat / 100.0;
            b.dyn.bri = o.bri / 100.0;
            b.dyn.per_tip = o.per_tip;
        }
        const auto fmt = m_doc.image().format;
        const auto fg = m_rail.foreground(), bg = m_rail.background();
        b.colour = core::from_srgb(fmt, fg.r, fg.g, fg.b);
        b.background = core::from_srgb(fmt, bg.r, bg.g, bg.b);
        // s016: Quick Mask -- the Brush and Eraser paint the selection.
        if (m_quick_mask && !sampled && !smudge) {
            on_selection_stroke(p, erase, b, o.smoothing);
            return;
        }
        if (m_quick_mask) { toast(verb + ": leave Quick Mask first (Q)"); return; }
        const auto id = m_layers.primary();
        // s012: the layer's MASK is the target -- white shows, black hides; the
        // Eraser paints the background colour (Photoshop's way).
        const auto lm = m_doc.get(id);
        // s018 m4: the stack's "Paint Filter Mask" -- the filter mask is the target.
        const bool on_fmask = lm && !lm->filters.empty() && m_properties.filter_mask_target() == id && !erase_only_pixels(sampled, smudge);
        const bool on_mask = on_fmask || (lm && lm->mask && m_layers.mask_target() == id);
        if (on_mask && (sampled || smudge)) {
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
        m_stroke.on_filter_mask = on_fmask;
        m_stroke.original = on_fmask ? (l.filter_mask ? l.filter_mask : std::make_shared<const pixel::TileMap>(m_doc.blank_mask()))
                          : on_mask ? l.mask : std::make_shared<const pixel::TileMap>(l.pixels ? *l.pixels : m_doc.blank());
        m_stroke.current = m_stroke.original;
        m_stroke.cov = std::make_shared<const pixel::TileMap>(Pa::new_coverage(m_doc.image().width, m_doc.image().height));
        m_stroke.mask = m_doc.selection_ptr();
        m_stroke.carry = 1e9;   // the first dab lands at the press
        m_stroke.jitter = Pa::Jitter(stroke_seed());   // s028: fresh dice each stroke
        // s028 m2: Color Dynamics -- the Brush on pixels only; the colours from
        // the colour pair, each dab's made in the document's mode.
        if (b.dyn.colour() && !erase && !sampled && !smudge && !on_mask) {
            const float f3[3] = {static_cast<float>(fg.r), static_cast<float>(fg.g), static_cast<float>(fg.b)};
            const float b3[3] = {static_cast<float>(bg.r), static_cast<float>(bg.g), static_cast<float>(bg.b)};
            m_stroke.jitter.colours(fmt, f3, b3);
            m_stroke.tint = std::make_shared<const pixel::TileMap>(Pa::new_tint(fmt, m_doc.image().width, m_doc.image().height));
        }
        m_journal.begin(on_fmask ? (erase ? "Eraser on Filter Mask" : "Brush on Filter Mask") : on_mask ? (erase ? "Eraser on Mask" : "Brush on Mask") : (sampled || smudge ? verb : erase ? "Eraser" : "Brush Stroke"), {id});
        if (smudge) m_stroke.smudge = std::make_shared<Pa::SmudgeCarry>();   // s017
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
        const auto dabs = m_stroke.jitter.apply(Pa::dabs_along(from, p, b, m_stroke.carry), m_stroke.brush);   // s028
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
        const auto dabs = m_stroke.jitter.apply(Pa::dabs_along(m_stroke.last, *q, m_stroke.brush, m_stroke.carry), m_stroke.brush);
        m_stroke.pending.insert(m_stroke.pending.end(), dabs.begin(), dabs.end());
        m_stroke.last = *q;
        paint_pump();
        return;
    }
    // End: the brush catches up to the pen; finish once the work in flight has landed.
    m_stroke.smoother.push(p);
    if (const auto q = m_stroke.smoother.finish()) {
        const auto dabs = m_stroke.jitter.apply(Pa::dabs_along(m_stroke.last, *q, m_stroke.brush, m_stroke.carry), m_stroke.brush);
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
                                           b = m_stroke.brush, carry = m_stroke.smudge, tint = m_stroke.tint](core::Jobs::Token&) -> core::Jobs::Landing {
        core::TileSet touched = core::tiles_of(im);
        std::shared_ptr<const pixel::TileMap> cov2 = cov, cur2, tint2 = tint;
        if (carry)   // s017: Smudge -- dab by dab on what's there (one job at a time owns the finger)
            cur2 = std::make_shared<const pixel::TileMap>(Pa::smudge(*current, mask.get(), b, dabs, *carry, touched));
        else {
            std::optional<pixel::TileMap> t;   // s028 m2: the stroke's colours, moved on
            if (tint) t = *tint;
            cov2 = std::make_shared<const pixel::TileMap>(Pa::add_pen_dabs(*cov, b, dabs, touched, t ? &*t : nullptr));
            if (t) tint2 = std::make_shared<const pixel::TileMap>(std::move(*t));
            cur2 = std::make_shared<const pixel::TileMap>(Pa::apply(*original, *current, *cov2, mask.get(), b, touched, tint2.get()));
        }
        return [this, cov2, cur2, tint2] {
            m_stroke.job = 0;
            // The stroke was ended from outside (an undo mid-stroke closed
            // the gesture): its work is dropped, nothing written around the door.
            if (!m_stroke.active || !m_journal.in_gesture()) { m_stroke = {}; return; }
            m_stroke.cov = cov2;
            m_stroke.tint = tint2;   // s028 m2
            const bool unchanged = same_tiles(*cur2, *m_stroke.current);
            m_stroke.current = cur2;
            if (m_stroke.on_selection && !unchanged) {   // s016: Quick Mask -- the selection record
                core::Layer v;
                v.name = "Selection";
                v.pixels = cur2;
                m_doc.put(core::kSelectionId, cur2->tile_count() ? std::optional<core::Layer>(v) : std::nullopt);
            } else if (auto l = unchanged ? std::nullopt : m_doc.get(m_stroke.id)) {
                if (m_stroke.on_filter_mask) l->filter_mask = cur2;   // s018 m4
                else if (m_stroke.on_mask) l->mask = cur2;   // s012: a mask stays a mask, even with no tiles
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

// s016: a Quick Mask stroke -- the selection is the target (the Brush's own
// engine on the mask-format map). No selection counts as all selected (no
// red), so the first black stroke masks what it covers. One stroke, one step.
void Shell::on_selection_stroke(const Pa::Dab& p, bool erase, Pa::Brush b, int smoothing) {
    const auto fg = m_rail.foreground(), bg = m_rail.background();
    b.erase = false;
    b.alpha_lock = false;
    b.colour = pixel::Px{};
    b.colour.c[0] = 1.f - mask_hide_of(erase ? bg : fg);   // white selects, black masks
    const auto im = m_doc.image();
    m_stroke = {};
    m_stroke.active = true;
    m_stroke.id = core::kSelectionId;
    m_stroke.brush = b;
    m_stroke.on_selection = true;
    const auto* sel = m_doc.selection();
    m_stroke.original = std::make_shared<const pixel::TileMap>(
        sel ? *sel : core::select::all_mask(m_doc.mask_format(), im.width, im.height));
    m_stroke.current = m_stroke.original;
    m_stroke.cov = std::make_shared<const pixel::TileMap>(Pa::new_coverage(im.width, im.height));
    m_stroke.carry = 1e9;
    m_stroke.jitter = Pa::Jitter(stroke_seed());   // s028
    m_journal.begin(erase ? "Quick Mask: Erase" : "Quick Mask: Paint", {core::kSelectionId});
    const auto dabs = m_stroke.jitter.apply(Pa::dabs_along(p, p, b, m_stroke.carry), m_stroke.brush);   // s028
    m_stroke.pending.insert(m_stroke.pending.end(), dabs.begin(), dabs.end());
    m_stroke.last = p;
    const double zoom = std::max(1e-3, m_canvas.zoom());
    m_stroke.smoother = Pa::Smoother(smoothing / 100.0 * 60.0 / zoom);
    m_stroke.smoother.push(p);
    paint_pump();
}

// s016: Select > Save Selection -- a new channel in the Channels tab.
void Shell::on_save_selection() {
    if (!m_doc.selection()) { toast("Save Selection: select something first"); return; }
    const auto id = core::select::save_channel(m_journal, m_doc);
    if (!id.empty()) toast("Saved as “" + m_doc.get(id)->name + "” -- the Channels tab; click it to select it again");
}

void Shell::update_channels() {
    std::vector<ChannelsPanel::Item> items;
    for (const auto& id : m_doc.channel_order()) items.push_back({id, m_doc.get(id)->name});
    m_channels.show(items, m_doc.selection() != nullptr);
    // s026: the colour channels (the picture's name: its mode).
    const auto f = m_doc.image().format;
    const char* comp = f.mode == pixel::Mode::RGB ? "RGB" : f.mode == pixel::Mode::CMYK ? "CMYK" : f.mode == pixel::Mode::Gray ? "Gray" : "Lab";
    m_channels.show_colour(has_image() ? core::select::colour_channels(f) : std::vector<std::string>{}, comp, m_channel_view);
}

// s016: Q -- Quick Mask on / off. View state: the strokes made in it are the steps.
void Shell::on_quick_mask() {
    if (!has_image()) return;
    m_quick_mask = !m_quick_mask;
    m_ants_known = false;   // the ants hide in Quick Mask, and come back
    queue_ants();
    queue_render();
    toast(m_quick_mask ? "Quick Mask: paint black to mask (red), white to select -- Q to leave"
                       : "Quick Mask off -- the selection is what you painted");
}

// s016: Heal > Patch. Begin: the selection, the layer and its source (All
// layers: flattened on a worker first); Move: where to take from; End: the
// last preview lands as ONE step ("Patch") once it matches the release.
void Shell::on_patch(CanvasView::MoveAt at, int dx, int dy) {
    using M = CanvasView::MoveAt;
    if (at == M::Begin) {
        if (m_patch.active) return;
        const auto id = m_layers.primary();
        const auto sel = m_doc.selection_ptr();
        if (!sel) { toast("Patch: select what's wrong first, then drag it onto good pixels"); return; }
        Pa::Brush probe;
        if (const auto why = Pa::refusal(m_doc, id, probe); !why.empty()) { toast("Patch: " + why); return; }
        const auto mode = m_doc.image().format.mode;
        const bool all = m_strip.clone().all_layers;
        if (mode != pixel::Mode::RGB && (all || mode != pixel::Mode::Gray)) { toast("Patch: RGB and Gray documents only, for now"); return; }
        const auto l = *m_doc.get(id);
        m_patch = {};
        m_patch.active = true;
        m_patch.id = id;
        m_patch.sel = sel;
        m_patch.alpha_lock = (l.locks & core::lock::Alpha) != 0;
        m_patch.expect = l.pixels;
        m_patch.original = std::make_shared<const pixel::TileMap>(l.pixels ? *l.pixels : m_doc.blank());
        if (!all) m_patch.ref = m_patch.original;
        else {
            const auto im = m_doc.image();
            auto layers = std::make_shared<const core::Layers>(m_doc.snapshot());
            m_patch.ref_job = m_jobs.submit("Patch source", [this, im, layers](core::Jobs::Token&) -> core::Jobs::Landing {
                auto flat = core::file::flatten(im, *layers);
                auto src = flat ? std::make_shared<const pixel::TileMap>(std::move(*flat)) : nullptr;
                return [this, src] {
                    m_patch.ref_job = 0;
                    if (!m_patch.active) return;
                    m_patch.ref = src;
                    patch_pump();
                };
            });
        }
        return;
    }
    if (!m_patch.active) return;
    m_patch.want_x = dx;
    m_patch.want_y = dy;
    m_canvas.set_ants_offset(dx, dy);
    if (at == M::End) m_patch.ending = true;
    patch_pump();
}

void Shell::patch_pump() {
    if (!m_patch.active || m_patch.job || m_patch.ref_job) return;
    if (!m_patch.ref) { m_patch = {}; m_canvas.set_ants_offset(0, 0); return; }   // flattening failed
    const bool current = m_patch.done_x == m_patch.want_x && m_patch.done_y == m_patch.want_y;
    if (current && m_patch.ending) {
        // Released: the last preview is the step (none when it didn't move).
        const auto id = m_patch.id;
        const auto result = m_patch.result;
        const auto expect = m_patch.expect;
        m_canvas.set_ants_offset(0, 0);
        m_preview.erase(id);
        m_patch = {};
        if (result && !core::layers::set_pixels_if(m_journal, m_doc, id, expect, *result, "Patch")) {
            const auto now = m_doc.get(id);
            toast(now && now->pixels == expect ? "Patch: nothing to change here" : "Patch: the layer changed meanwhile -- nothing done");
        }
        queue_render();
        return;
    }
    if (current) return;
    const int dx = m_patch.want_x, dy = m_patch.want_y;
    if (dx == 0 && dy == 0) {   // back where it started: no patch
        m_patch.done_x = m_patch.done_y = 0;
        m_patch.result.reset();
        if (m_preview.erase(m_patch.id)) queue_render();
        patch_pump();
        return;
    }
    const auto im = m_doc.image();
    m_patch.job = m_jobs.submit("Patch", [this, im, original = m_patch.original, ref = m_patch.ref, sel = m_patch.sel,
                                          lock = m_patch.alpha_lock, dx, dy](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto out = core::heal::patched(im, *original, *ref, *sel, dx, dy, lock, [&t] { return t.cancelled(); });
        auto res = out ? std::make_shared<const pixel::TileMap>(std::move(*out)) : nullptr;
        return [this, res, dx, dy] {
            m_patch.job = 0;
            if (!m_patch.active) return;
            m_patch.done_x = dx;
            m_patch.done_y = dy;
            m_patch.result = res;
            if (res) m_preview[m_patch.id] = res;
            queue_render();
            patch_pump();
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

// s029: the tool's SETTINGS in a popover -- from a right-click on its rail
// button (any tool, chosen or not) or on the canvas (at the pointer). The
// popover is made for the occasion and goes when it closes.
void Shell::show_tool_settings(const std::string& tool, Gtk::Widget& at, const Gdk::Rectangle* where) {
    auto* pop = new Gtk::Popover();
    pop->set_parent(at);
    // Beside what it points at, never above / below: the Brush's panel is
    // taller than the room over or under the pointer in a short window, and
    // GTK closes a popover it can't place (seen under Xvfb, 620 px high).
    // Beside, it slides up and down to fit. The side with more room.
    pop->set_position(where && where->get_x() > at.get_width() / 2 ? Gtk::PositionType::LEFT : Gtk::PositionType::RIGHT);
    if (where) pop->set_pointing_to(*where);
    pop->set_child(*m_strip.settings_panel(tool, pop));
    pop->signal_closed().connect([pop] {
        Glib::signal_idle().connect_once([pop] { pop->unparent(); delete pop; });
    });
    pop->popup();
}

}  // namespace klimt
