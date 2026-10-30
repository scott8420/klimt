#include "Shell.hpp"
#include "Log.hpp"

#include <adwaita.h>
#include <gtkmm/editable.h>
#include <gtkmm/textview.h>
#include <glibmm/main.h>
#include "core/Composite.hpp"
#include "core/Selection.hpp"   // s013: the preview's mask
#include "core/ColourChannels.hpp"   // s026

#include <cmath>

// Shell_helpers.cpp -- HELPERS. Shared sync called from zones, bindings and
// handlers: the demo document, greying, labels, toasts, jobs.

namespace klimt {

// A demo document so the panels have something to show: LOADED, not a user
// choice, so it is not a step. Replaced by File > Open when there are pixels.
// s039 m3: the Refine brush in hand, its Overlay on, something selected --
// the unselected show under Quick Mask's red.
bool Shell::refine_overlay() const {
    return m_rail.tool() == "refine" && m_strip.refine().overlay && m_doc.selection_ptr() != nullptr;
}

void Shell::load_demo() {
    namespace P = pixel;
    core::Document::Loading load(m_doc);
    const int W = 1200, H = 1600;
    m_doc.set_image({.width = W, .height = H, .format = P::Format{P::Depth::U8, P::Mode::RGB, true}});
    const auto put = [this](core::Kind kind, const char* name, const std::string& parent, double pos,
                            core::Blend blend = core::Blend::Normal, int opacity = 100,
                            std::shared_ptr<const P::TileMap> px = nullptr) {
        const auto id = m_doc.fresh_id();
        core::Layer l;
        l.kind = kind;
        l.name = name;
        l.parent = parent;
        l.pos = pos;
        l.blend = kind == core::Kind::Group ? core::Blend::PassThrough : blend;
        l.opacity = opacity;
        l.pixels = std::move(px);
        m_doc.put(id, l);
        return id;
    };
    // Pixels for the demo: shapes painted per pixel (a load, not a step).
    const auto map = [this](const P::Rect& r, std::function<P::Px(int, int, const P::Px&)> f) {
        return std::make_shared<const P::TileMap>(m_doc.blank().paint(r, std::move(f)));
    };
    const auto disc = [](double cx, double cy, double rx, double ry, P::Px c) {
        return [=](int x, int y, const P::Px& old) {
            const double dx = (x + 0.5 - cx) / rx, dy = (y + 0.5 - cy) / ry;
            const double d = std::sqrt(dx * dx + dy * dy);
            const float a = static_cast<float>(std::clamp((1.0 - d) * rx, 0.0, 1.0));   // 1 px soft edge
            if (a <= 0) return old;
            P::Px p = c;
            p.c[3] = std::max(old.c[3], a);
            return p;
        };
    };
    using core::Kind;
    const auto bg = put(Kind::Pixel, "Background", "", 1, core::Blend::Normal, 100,
                        std::make_shared<const P::TileMap>(m_doc.blank().fill({0, 0, W, H}, P::rgba(0.13f, 0.23f, 0.23f))));
    put(Kind::Pixel, "Vignette", "", 2, core::Blend::Multiply, 40, map({0, 0, W, H}, [=](int x, int y, const P::Px&) {
        const double dx = (x - W / 2.0) / (W / 2.0), dy = (y - H / 2.0) / (H / 2.0);
        const double d = std::sqrt(dx * dx + dy * dy);
        return P::rgba(0, 0, 0, static_cast<float>(std::clamp((d - 0.55) / 0.6, 0.0, 1.0)));
    }));
    const auto portrait = put(Kind::Group, "Portrait", "", 3);
    put(Kind::Pixel, "Skin", portrait, 1, core::Blend::Normal, 100,
        map({200, 220, 800, 800}, disc(600, 620, 390, 390, P::rgba(0.79f, 0.64f, 0.26f))));
    {
        auto face = m_doc.blank().paint({440, 470, 320, 380}, disc(600, 660, 150, 185, P::rgba(0.09f, 0.16f, 0.15f)));
        face = face.paint({260, 900, 680, 700}, disc(600, 1400, 330, 480, P::rgba(0.09f, 0.16f, 0.15f)));
        put(Kind::Pixel, "Face", portrait, 2, core::Blend::Normal, 100, std::make_shared<const P::TileMap>(std::move(face)));
    }
    put(Kind::Pixel, "Retouch", portrait, 3, core::Blend::SoftLight, 70,
        map({0, 300, W, 700}, [](int x, int y, const P::Px&) {
            const double t = std::abs((x - y * 0.6) - 150.0) / 260.0;   // a soft diagonal band
            return P::rgba(1, 0.95f, 0.85f, static_cast<float>(std::clamp(1.0 - t, 0.0, 1.0) * 0.8));
        }));
    put(Kind::Pixel, "Hair", portrait, 4, core::Blend::Normal, 100,
        map({420, 440, 360, 240}, [](int x, int y, const P::Px& old) {
            const double dx = x - 600.0, dy = y - 650.0, d = std::sqrt(dx * dx + dy * dy);
            if (dy > -20 || d < 165 || d > 205) return old;          // an arc over the head
            return P::rgba(0.55f, 0.40f, 0.16f, 1);
        }));
    put(Kind::Pixel, "Title", "", 4, core::Blend::Normal, 100,
        map({220, 110, 760, 70}, [](int, int, const P::Px&) { return P::rgba(0.94f, 0.87f, 0.66f, 1); }));
    auto b = *m_doc.get(bg);
    b.locks = core::lock::All;
    m_doc.put(bg, b);
    m_layers.select({portrait}, false);
}

// The Layers selection moved, or the panel repainted: everything that follows
// the selection follows it.
void Shell::on_selection() {
    m_properties.show(m_layers.selection());
    update_paths();   // s035: a picked path layer is the path on the canvas
    update_layer_actions();
    update_summary();
}

void Shell::update_layer_actions() {
    const bool any = m_layers.has_selection();
    const auto on = [this](const char* name, bool enabled) {
        if (auto it = m_layer_acts.find(name); it != m_layer_acts.end()) it->second->set_enabled(enabled);
    };
    // s009: with no image, the verbs that need one grey.
    const bool img = has_image();
    on("guides-clear", img && !m_doc.image().guides.empty());   // s032
    for (const char* a : {"layer-new", "layer-new-group", "save", "save-as", "export", "image-size", "canvas-size", "reveal-all", "select-all", "quick-mask", "select-save", "select-range", "refine-edges",
                          "free-transform", "flip-h", "flip-v", "rotate-cw", "rotate-ccw", "rotate-180",
                          "canvas-cw", "canvas-ccw", "canvas-180", "canvas-flip-h", "canvas-flip-v", "canvas-rotate"})
        on(a, img);
    // s012: the mask verbs follow the primary layer's mask.
    {
        const auto pl = m_doc.get(m_layers.primary());
        const bool pixel_layer = pl && pl->kind == core::Kind::Pixel;
        const bool maskable = pl != std::nullopt;   // s013: adjustment layers; s024: groups too
        const bool masked = maskable && pl->mask;
        for (const char* a : {"mask-add", "mask-reveal-all", "mask-hide-all", "mask-reveal-sel", "mask-hide-sel"}) on(a, maskable && !masked);
        for (const char* a : {"mask-delete", "mask-toggle", "mask-invert"}) on(a, masked);
        on("mask-apply", masked && pixel_layer);
        on("mask-edit", masked && pixel_layer);
        // s013: Image > Adjustments wants a pixel layer; new adjustment layers, an image.
        for (const auto k : core::adjust::kinds()) {
            const std::string key(core::adjust::key(k));
            on(("adjust-apply-" + key).c_str(), img && pixel_layer && pl->pixels);
            on(("adjust-new-" + key).c_str(), img);
        for (const auto fk : core::filter::kinds())   // s030
            on(("filter-layer-" + std::string(core::filter::key(fk))).c_str(), img && m_doc.image().format.mode == pixel::Mode::RGB);
        }
        // s014: the Filter menu wants a pixel layer with pixels (Last Filter: one run before).
        const bool filterable = img && pixel_layer && pl->pixels;
        for (const auto k : core::filter::kinds()) on(("filter-" + std::string(core::filter::key(k))).c_str(), filterable);
        on("filter-last", filterable && m_last_filter);
        // s019: a pixel layer takes styles; Paste wants something copied.
        // s024: groups take styles too (Rasterize: pixel layers only).
        const bool styleable = pixel_layer || (pl && pl->kind == core::Kind::Group);
        for (const char* v : {"style-drop-shadow", "style-outer-glow", "style-inner-shadow", "style-inner-glow", "style-satin", "style-pattern-overlay", "style-gradient-overlay", "style-bevel-emboss", "style-colour-overlay", "style-stroke", "style-copy", "style-clear"})
            on(v, img && styleable);
        on("style-rasterize", img && pixel_layer);
        on("style-paste", img && styleable && m_style_clip);
        on("filter-last-dialog", filterable && m_last_filter);
    }
    on("layer-duplicate", any);
    on("layer-group", any);
    on("layer-ungroup", m_layers.primary_is_group());
    on("layer-delete", any);
    on("layer-raise", any);
    on("layer-lower", any);
    on("layer-clip", any);
    on("layer-rename", any);
}

// Undo / Redo say what they would do, and grey when there is nothing.
void Shell::update_undo_ui() {
    if (m_act_undo) m_act_undo->set_enabled(m_journal.can_undo());
    if (m_act_redo) m_act_redo->set_enabled(m_journal.can_redo());
    const std::string u = m_journal.undo_label(), r = m_journal.redo_label();
    m_undo_button.set_tooltip_text(u.empty() ? "Nothing to undo" : "Undo " + u + "  (Ctrl+Z)");
    m_redo_button.set_tooltip_text(r.empty() ? "Nothing to redo" : "Redo " + r + "  (Ctrl+Shift+Z)");
    if (m_edit_menu) {
        m_edit_menu->remove_all();
        m_edit_menu->append(u.empty() ? "Undo" : "Undo " + u, "win.undo");
        m_edit_menu->append(r.empty() ? "Redo" : "Redo " + r, "win.redo");
    }
    // s020: Edit > Fade follows the last step too.
    const auto fade = core::fade::source(m_journal, m_doc);
    if (m_act_fade) m_act_fade->set_enabled(fade.has_value());
    if (m_edit_menu) m_edit_menu->append(fade ? "Fade " + fade->what + "…" : "Fade…", "win.fade");
}

// The document in a line: the title's subtitle and the status line.
void Shell::update_summary() {   // s012: says when the mask is what's painted
    if (!has_image()) {   // s009: launched with nothing open yet
        if (m_title) adw_window_title_set_subtitle(m_title, "");
        m_status.set_summary("No image  ·  New (Ctrl+N), Open (Ctrl+O), or Open Demo in the main menu");
        return;
    }
    std::size_t layers = 0, groups = 0;
    for (const auto& id : m_doc.walk()) (m_doc.is_group(id) ? groups : layers)++;
    std::string s = std::to_string(layers) + (layers == 1 ? " layer" : " layers");
    if (groups) s += ", " + std::to_string(groups) + (groups == 1 ? " group" : " groups");
    if (m_title) adw_window_title_set_subtitle(m_title, s.c_str());
    const auto n = m_layers.selection().size();
    const auto& im = m_doc.image();
    const char* mode = im.format.mode == pixel::Mode::RGB ? "RGB" : im.format.mode == pixel::Mode::CMYK ? "CMYK"
                     : im.format.mode == pixel::Mode::Lab ? "Lab" : "Gray";
    const char* depth = im.format.depth == pixel::Depth::U8 ? "8-bit" : im.format.depth == pixel::Depth::U16 ? "16-bit"
                      : "32-bit float";
    m_status.set_summary(std::to_string(im.width) + " × " + std::to_string(im.height) + " px  ·  " + mode + " " +
                         depth + "  ·  " + s + (n ? "  ·  " + std::to_string(n) + " selected" : std::string{}) +
                         (m_layers.mask_target().empty() ? std::string{} : "  ·  painting the MASK (white shows, black hides)"));
}

// A toast. With `offer_undo`, its button is Undo (the step just taken).
void Shell::toast(const std::string& text, bool offer_undo) {
    if (!m_toasts) return;
    AdwToast* t = adw_toast_new(text.c_str());
    adw_toast_set_timeout(t, offer_undo ? 4 : 2);
    if (offer_undo) {
        adw_toast_set_button_label(t, "Undo");
        adw_toast_set_action_name(t, "win.undo");
    }
    adw_toast_overlay_add_toast(m_toasts, t);   // takes the toast
}

// ── Jobs: submit, follow, land ──────────────────────────────────────────────
core::Jobs::Id Shell::start_job(const std::string& label, core::Jobs::Work work) {
    const auto id = m_jobs.submit(label, std::move(work));
    m_job_ids.push_back(id);
    if (auto lg = log::get(log::Area::Shell)) lg->info("job: {}", label);
    if (!m_job_poll.connected())
        m_job_poll = Glib::signal_timeout().connect(sigc::mem_fun(*this, &Shell::poll_jobs), 100);
    return id;
}

void Shell::on_jobs_wake() {  // the UI thread, woken by a worker
    const auto landed = m_jobs.pump();
    if (landed)
        if (auto lg = log::get(log::Area::Shell)) lg->info("jobs: {} landed", landed);
    poll_jobs();
}

bool Shell::poll_jobs() {
    using S = core::Jobs::State;
    std::erase_if(m_job_ids, [this](core::Jobs::Id id) {
        const auto st = m_jobs.state(id);
        if (st == S::Failed) toast(m_jobs.label(id) + " failed: " + m_jobs.error(id));
        return st == S::Landed || st == S::Cancelled || st == S::Failed;
    });
    // s014 m4: OK's filter job cancelled or failed -- its preview goes too.
    if (m_filter_landing && (m_jobs.state(m_filter_landing) == S::Cancelled || m_jobs.state(m_filter_landing) == S::Failed)) {
        m_filter_landing = 0;
        if (m_preview.erase(m_filter_target)) queue_render();
    }
    if (m_job_ids.empty()) {
        m_status.hide_job();
        if (m_job_poll.connected()) m_job_poll.disconnect();
        return false;
    }
    m_status.show_job(m_jobs.label(m_job_ids.front()), m_jobs.progress(m_job_ids.front()));
    return true;
}

bool Shell::focus_is_text() const {
    for (const Gtk::Widget* w = get_focus(); w; w = w->get_parent())
        if (dynamic_cast<const Gtk::TextView*>(w) || dynamic_cast<const Gtk::Editable*>(w)) return true;
    return false;
}

// The document changed: composite the tiles it changed (core::changed_tiles
// against what the canvas was made from), on workers, once per idle. A change
// while batches run cancels them all (their landings are dropped) and sends
// every tile still owed again, nearest the view's centre first; a batch that
// lands is current. Landings hand the canvas tiles: not a step.
void Shell::queue_render() {
    if (m_render_queued) return;
    m_render_queued = true;
    Glib::signal_idle().connect_once([this] {
        m_render_queued = false;
        update_rulers();   // s031: the zero point, the resolution may have changed
        update_guides();   // s032
        update_paths();    // s035
        const core::Image im = m_doc.image();
        auto layers = m_doc.snapshot();
        // s011: a transform's preview shows in place of the real pixels.
        for (const auto& [id, px] : m_preview)
            if (auto it = layers.find(id); it != layers.end()) {
                it->second.pixels = px;
                it->second.filters.clear();   // s018: a filter preview has the live stack in it already
            }
        // s013: Image > Adjustments' preview -- the adjustment as a layer
        // clipped to its target, just above it, through the selection.
        if (m_adjust_preview && !core::adjust::identity(*m_adjust_preview))
            if (const auto it = layers.find(m_adjust_target); it != layers.end()) {
                const auto& t = it->second;
                double next = t.pos + 1;
                for (const auto& [oid, o] : layers)
                    if (o.parent == t.parent && o.pos > t.pos) next = std::min(next, o.pos);
                core::Layer p;
                p.kind = core::Kind::Adjustment;
                p.name = "Preview";
                p.parent = t.parent;
                p.pos = (t.pos + next) / 2;
                p.clipped = true;
                p.adjust = *m_adjust_preview;
                if (const auto* sel = m_doc.selection())
                    p.mask = std::make_shared<const pixel::TileMap>(
                        core::select::inverted(sel, m_doc.mask_format(), im.width, im.height));
                layers["@adjust-preview"] = std::move(p);
            }
        const bool fresh = im.width != m_shown_image.width || im.height != m_shown_image.height ||
                           !(im.format == m_shown_image.format) || m_dirty.cols == 0;
        if (fresh) {
            m_canvas.set_document(im.width, im.height);
            m_dirty = core::tiles_of(im);
            m_dirty.add_all();
        } else {
            m_dirty.merge(core::changed_tiles(m_shown_image, m_shown, im, layers));
        }
        // s016: Quick Mask's red follows the selection -- on / off redraws all;
        // a painted selection redraws the tiles whose mask changed.
        // s024: Color Range's preview shows the same way.
        // s039 m3: the Refine brush's overlay -- the selection, the same way.
        const bool ref_ov = refine_overlay();
        const bool qm_flag = m_quick_mask || m_range_preview || ref_ov;
        const auto qm = m_range_preview ? m_range_preview : (m_quick_mask || ref_ov) ? m_doc.selection_ptr() : nullptr;
        if (qm_flag != m_shown_qm_on) m_dirty.add_all();
        else if (qm_flag && qm != m_shown_qm) {
            if (!qm || !m_shown_qm) m_dirty.add_all();
            else
                for (int row = 0; row < std::min(m_dirty.rows, qm->rows()); ++row)
                    for (int col = 0; col < std::min(m_dirty.cols, qm->cols()); ++col)
                        if (!qm->same_tile(*m_shown_qm, col, row)) m_dirty.add(col, row);
        }
        m_shown_qm = qm;
        m_shown_qm_on = qm_flag;
        m_shown = layers;
        m_shown_image = im;
        for (const auto id : m_renders) m_jobs.cancel(id);
        m_renders.clear();
        if (im.width <= 0 || im.height <= 0 || m_dirty.count() == 0) return;

        // The tiles owed, the view's centre first.
        double cx = 0, cy = 0;
        m_canvas.view_centre(cx, cy);
        struct T { int col, row; double d; };
        std::vector<T> owed;
        for (int row = 0; row < m_dirty.rows; ++row)
            for (int col = 0; col < m_dirty.cols; ++col)
                if (m_dirty.has(col, row)) {
                    const double dx = (col + 0.5) * pixel::kTile - cx, dy = (row + 0.5) * pixel::kTile - cy;
                    owed.push_back({col, row, dx * dx + dy * dy});
                }
        std::sort(owed.begin(), owed.end(), [](const T& a, const T& b) { return a.d < b.d; });
        auto shared = std::make_shared<const core::Layers>(std::move(layers));
        constexpr std::size_t kBatch = 12;
        for (std::size_t i = 0; i < owed.size(); i += kBatch) {
            std::vector<std::pair<int, int>> batch;
            for (std::size_t k = i; k < std::min(owed.size(), i + kBatch); ++k) batch.emplace_back(owed[k].col, owed[k].row);
            m_renders.push_back(m_jobs.submit("Composite", [this, im, shared, batch, view = m_mask_view, qm, qm_on = qm_flag, chan = m_channel_view](core::Jobs::Token& t) -> core::Jobs::Landing {
                auto out = std::make_shared<std::vector<CanvasView::Tile>>();
                for (const auto& [col, row] : batch) {
                    if (t.cancelled()) return [] {};
                    auto px = std::make_shared<std::vector<std::uint32_t>>();
                    CanvasView::Tile tile;
                    tile.col = col;
                    tile.row = row;
                    if (!view.empty() && shared->count(view) && shared->at(view).mask)   // s012: the mask itself
                        core::mask_tile_argb(*shared->at(view).mask, col, row, *px, tile.w, tile.h);
                    else
                        core::composite_tile_argb(im, *shared, col, row, *px, tile.w, tile.h);
                    // s026: one colour channel alone, as grey.
                    if (chan >= 0 && view.empty())
                        for (auto& v : *px) {
                            const float r = ((v >> 16) & 0xff) / 255.f, g = ((v >> 8) & 0xff) / 255.f, b = (v & 0xff) / 255.f;
                            const auto q = static_cast<std::uint32_t>(std::lround(std::clamp(core::select::channel_grey(im.format.mode, chan, r, g, b), 0.f, 1.f) * 255.f));
                            v = (v & 0xff000000u) | (q << 16) | (q << 8) | q;
                        }
                    // s016: Quick Mask -- the unselected under a half-strong red.
                    if (qm_on && qm) {
                        for (int y = 0; y < tile.h; ++y)
                            for (int x = 0; x < tile.w; ++x) {
                                const float a = 0.5f * (1.f - core::select::coverage(*qm, col * pixel::kTile + x, row * pixel::kTile + y));
                                if (a <= 0.f) continue;
                                auto& v = (*px)[static_cast<std::size_t>(y) * static_cast<std::size_t>(tile.w) + static_cast<std::size_t>(x)];
                                const auto mix = [a](std::uint32_t c, std::uint32_t to) { return static_cast<std::uint32_t>(std::lround(c * (1 - a) + to * a)); };
                                v = (mix(v >> 24, 255) << 24) | (mix((v >> 16) & 0xff, 255) << 16) | (mix((v >> 8) & 0xff, 0) << 8) | mix(v & 0xff, 0);
                            }
                    }
                    // s010 m4: its mips, here on the worker.
                    {
                        const std::vector<std::uint32_t>* from = px.get();
                        int fw = tile.w, fh = tile.h;
                        for (int k = 0; k < CanvasView::kMips && (fw > 1 || fh > 1); ++k) {
                            auto m = std::make_shared<std::vector<std::uint32_t>>();
                            int mw = 0, mh = 0;
                            core::argb_half(*from, fw, fh, *m, mw, mh);
                            from = m.get();
                            fw = mw;
                            fh = mh;
                            tile.mips.push_back({mw, mh, std::move(m)});
                        }
                    }
                    tile.argb = std::move(px);
                    out->push_back(std::move(tile));
                }
                return [this, out] {
                    m_canvas.set_tiles(*out);
                    for (const auto& tl : *out) {
                        if (m_dirty.all) {   // "all" becomes a real set as tiles land
                            m_dirty.all = false;
                            std::fill(m_dirty.on.begin(), m_dirty.on.end(), 1);
                        }
                        m_dirty.on[static_cast<std::size_t>(tl.row) * static_cast<std::size_t>(m_dirty.cols) +
                                   static_cast<std::size_t>(tl.col)] = 0;
                    }
                };
            }));
        }
    });
}

}  // namespace klimt
