#include "Shell.hpp"
#include "Log.hpp"
#include "core/Selection.hpp"
#include "core/Composite.hpp"
#include "core/File.hpp"
#include "NumberDialog.hpp"

#include <glibmm/main.h>

// Shell_select.cpp -- SELECTION (s008). The Select tool's shapes, the Select
// menu, Fill / Delete through the selection, and the marching ants. The mask
// work is a JOB (from a snapshot; the landing goes through select::set_if or
// layers::set_pixels_if, refused if the document moved meanwhile); the
// landing is the step.

namespace klimt {

namespace S = core::select;

// A marquee or lasso finished on the canvas.
void Shell::on_shape(const CanvasView::Shape& shape) {
    using K = CanvasView::Shape::Kind;
    if (shape.kind == K::None) {
        // A click with the Select tool: Deselect (Photoshop) -- a plain click
        // only; a modified click leaves the selection be.
        if (shape.op == S::Op::Replace && m_doc.selection()) S::none(m_journal, m_doc);
        return;
    }
    const auto im = m_doc.image();
    const auto mf = m_doc.mask_format();
    const auto old = m_doc.selection_ptr();
    if (shape.kind == K::Point) {
        // The Magic Wand: the pixels like the clicked one, in the selected
        // layer -- or in what you see, with "All layers".
        const auto w = m_strip.wand();
        const auto at = shape.pts.front();
        const int x = static_cast<int>(std::floor(at.x)), y = static_cast<int>(std::floor(at.y));
        if (x < 0 || y < 0 || x >= im.width || y >= im.height) return;
        const auto l = m_doc.get(m_layers.primary());
        if (!w.all_layers && (!l || !core::draws_pixels(l->kind))) { toast("Magic Wand: pick a layer (or tick All layers)"); return; }   // s046: type reads as pixels
        const auto layers = m_doc.snapshot();
        const auto src = w.all_layers ? nullptr : l->pixels;
        const auto blank = m_doc.blank();
        const std::string label = shape.op != S::Op::Replace ? S::op_name(shape.op) : "Magic Wand";
        start_job(label, [this, w, x, y, layers, src, blank, im, mf, old, op = shape.op, label](core::Jobs::Token& t)
                             -> core::Jobs::Landing {
            const auto cancel = [&t] { return t.cancelled(); };
            std::optional<pixel::TileMap> from;
            if (w.all_layers) {
                formats::Progress p;
                p.cancelled = cancel;
                from = core::file::flatten(im, layers, p);
            } else {
                from = src ? *src : blank;
            }
            if (!from) return [] {};
            auto m = S::wand_mask(*from, mf, x, y, static_cast<float>(w.tolerance) / 255.f, w.contiguous, cancel);
            if (!m) return [] {};
            auto next = std::make_shared<pixel::TileMap>(S::combine(old.get(), *m, op));
            return [this, old, next, label] { S::set_if(m_journal, m_doc, old, std::move(*next), label); };
        });
        return;
    }
    const std::string label = shape.op != S::Op::Replace ? S::op_name(shape.op)
                            : shape.kind == K::Rect ? "Rectangle Select" : shape.kind == K::Ellipse ? "Ellipse Select" : "Lasso";
    start_job(label, [this, shape, im, mf, old, label](core::Jobs::Token& t) -> core::Jobs::Landing {
        pixel::TileMap m;
        const auto& p = shape.pts;
        if (shape.kind == K::Lasso) {
            m = S::polygon_mask(mf, im.width, im.height, p);
        } else {
            const double x0 = std::min(p.front().x, p.back().x), x1 = std::max(p.front().x, p.back().x);
            const double y0 = std::min(p.front().y, p.back().y), y1 = std::max(p.front().y, p.back().y);
            if (shape.kind == K::Rect) {
                // The marquee snaps to whole pixels (hard edges, as Photoshop).
                const int ix0 = static_cast<int>(std::lround(x0)), iy0 = static_cast<int>(std::lround(y0));
                const int ix1 = static_cast<int>(std::lround(x1)), iy1 = static_cast<int>(std::lround(y1));
                m = S::rect_mask(mf, im.width, im.height, {ix0, iy0, ix1 - ix0, iy1 - iy0});
            } else {
                m = S::ellipse_mask(mf, im.width, im.height, x0, y0, x1, y1);
            }
        }
        if (t.cancelled()) return [] {};
        auto next = std::make_shared<pixel::TileMap>(S::combine(old.get(), m, shape.op));
        return [this, old, next, label] {
            if (!S::set_if(m_journal, m_doc, old, std::move(*next), label) && m_doc.selection_ptr() != old)
                toast(label + ": the selection changed meanwhile -- nothing done");
        };
    });
}

void Shell::on_select_all() { S::all(m_journal, m_doc); }
void Shell::on_deselect() { S::none(m_journal, m_doc); }

void Shell::on_select_invert() {
    const auto im = m_doc.image();
    const auto mf = m_doc.mask_format();
    const auto old = m_doc.selection_ptr();
    start_job("Invert Selection", [this, im, mf, old](core::Jobs::Token&) -> core::Jobs::Landing {
        auto next = std::make_shared<pixel::TileMap>(S::inverted(old.get(), mf, im.width, im.height));
        return [this, old, next] { S::set_if(m_journal, m_doc, old, std::move(*next), "Invert Selection"); };
    });
}

// Fill / Delete through the selection, on the primary layer: a job from a
// snapshot of the layer's pixels and the selection; the landing is the step.
void Shell::run_pixel_job(const std::string& verb, bool fill, ToolRail::RGB colour) {
    const auto id = m_layers.primary();
    const auto l = m_doc.get(id);
    if (!l) { toast(verb + ": select a layer first"); return; }
    // s012: the MASK is the target: fill it with the colour's grey (white
    // shows, black hides); Delete fills with the background colour.
    if (l->mask && m_layers.mask_target() == id) {
        if (m_doc.locked_all(id)) { toast(verb + ": “" + l->name + "” is locked"); return; }
        if (!fill && !m_doc.selection()) { toast("Delete: nothing is selected -- Select All (Ctrl+A) first"); return; }
        const auto before = l->mask;
        const auto sel = m_doc.selection_ptr();
        pixel::Px hide;
        hide.c[0] = mask_hide_of(colour);
        const std::string label = (fill ? "Fill Mask of “" : "Delete in Mask of “") + l->name + "”";
        start_job(label, [this, id, before, sel, hide, label](core::Jobs::Token& t) -> core::Jobs::Landing {
            auto after = std::make_shared<pixel::TileMap>(S::filled(*before, sel.get(), hide, false));
            if (t.cancelled()) return [] {};
            return [this, id, before, sel, after, label] {
                if (m_doc.selection_ptr() != sel || !core::layers::set_mask_if(m_journal, m_doc, id, before, std::move(*after), label))
                    toast(label + ": the mask changed meanwhile -- nothing done");
            };
        });
        return;
    }
    if (l->kind != core::Kind::Pixel) { toast(verb + ": " + core::layers::not_pixels(m_doc, id)); return; }
    if (m_doc.locked_all(id) || (l->locks & core::lock::Pixels)) { toast(verb + ": “" + l->name + "” is locked"); return; }
    const bool alpha_lock = (l->locks & core::lock::Alpha) != 0;
    if (!fill && alpha_lock && m_doc.image().format.alpha) {
        toast("Delete: “" + l->name + "” has its transparent pixels locked");
        return;
    }
    if (!fill && !m_doc.selection()) { toast("Delete: nothing is selected -- Select All (Ctrl+A) first"); return; }
    const auto before = l->pixels;
    const auto mask = m_doc.selection_ptr();
    const auto blank = m_doc.blank();
    const pixel::Px px = core::from_srgb(m_doc.image().format, colour.r, colour.g, colour.b);
    const std::string label = (fill ? "Fill “" : "Delete in “") + l->name + "”";
    start_job(label, [this, id, before, mask, blank, px, fill, alpha_lock, label](core::Jobs::Token& t) -> core::Jobs::Landing {
        const pixel::TileMap& src = before ? *before : blank;
        auto after = std::make_shared<pixel::TileMap>(fill ? S::filled(src, mask.get(), px, alpha_lock)
                                                           : S::cleared(src, mask.get(), px));
        if (t.cancelled()) return [] {};
        return [this, id, before, mask, after, label] {
            if (m_doc.selection_ptr() != mask ||
                !core::layers::set_pixels_if(m_journal, m_doc, id, before, std::move(*after), label)) {
                if (m_doc.get(id) && m_doc.get(id)->pixels != before) toast(label + ": the layer changed meanwhile -- nothing done");
            }
        };
    });
}

void Shell::on_fill(bool foreground) {
    run_pixel_job("Fill", true, foreground ? m_rail.foreground() : m_rail.background());
}

void Shell::on_delete_selection() { run_pixel_job("Delete", false, m_rail.background()); }

// The marching ants follow the selection: when its mask changes, its
// outline is traced on a worker (newest wins) and handed to the canvas.
void Shell::queue_ants() {
    if (m_ants_queued) return;
    m_ants_queued = true;
    Glib::signal_idle().connect_once([this] {
        m_ants_queued = false;
        if (m_xform.active) return;   // s011: no ants while a transform's box is up
        if (m_quick_mask) { m_canvas.set_outline(nullptr); m_ants_known = false; return; }   // s016: the red shows it
        const auto sel = m_doc.selection_ptr();
        const int level = m_canvas.ants_level();   // s049: traced at the view's zoom
        if (m_ants_known && sel == m_ants_for && level == m_ants_level) return;
        const bool same_mask = m_ants_known && sel == m_ants_for;
        m_ants_known = true;
        m_ants_for = sel;
        m_ants_level = level;
        if (m_ants_job) m_jobs.cancel(m_ants_job);
        m_ants_job = 0;
        // The old ants go NOW: until the new outline lands they would show a
        // selection that no longer is (seen under ASan: an undone Wand's
        // outline stood ~1.5 s over the selection undo had put back). s049:
        // a new zoom level of the SAME mask keeps them -- they are still true.
        if (!same_mask) m_canvas.set_outline(nullptr);
        if (!sel) { update_select_actions(); return; }
        m_ants_job = m_jobs.submit("Outline", [this, sel, level](core::Jobs::Token& t) -> core::Jobs::Landing {
            auto a = S::ants(*sel, level, [&t] { return t.cancelled(); });
            if (!a || t.cancelled()) return [] {};
            auto ants = std::make_shared<const S::Ants>(std::move(*a));
            return [this, ants] { m_canvas.set_outline(ants); };
        });
        update_select_actions();
    });
}

void Shell::on_feather() {
    if (!m_doc.selection()) { toast("Feather: nothing is selected"); return; }
    if (!m_feather) {
        m_feather = std::make_unique<NumberDialog>(*this, "shell.feather", "Feather Selection", "Radius", "px",
                                                   0.5, 250, 5, 1);
        m_feather->signal_ok().connect([this](double r) {
            const auto old = m_doc.selection_ptr();
            if (!old) return;
            char label[48];
            std::snprintf(label, sizeof label, "Feather %g px", r);
            start_job(label, [this, old, r, label = std::string(label)](core::Jobs::Token& t) -> core::Jobs::Landing {
                auto f = S::feathered(*old, r, [&t] { return t.cancelled(); });
                if (!f) return [] {};
                auto next = std::make_shared<pixel::TileMap>(std::move(*f));
                return [this, old, next, label] { S::set_if(m_journal, m_doc, old, std::move(*next), label); };
            });
        });
    }
    m_feather->ask();
}

// The Eyedropper: what you see at (x, y) -- a job (it composites that tile).
void Shell::on_pick(int x, int y, bool background) {
    const auto im = m_doc.image();
    auto layers = m_doc.snapshot();
    const int radius = m_strip.variant("eyedropper");   // Point, 3 x 3, 5 x 5
    m_jobs.submit("Eyedropper", [this, im, layers = std::move(layers), x, y, radius, background](core::Jobs::Token&)
                                    -> core::Jobs::Landing {
        const auto s = core::sample_composite(im, layers, x, y, radius);
        return [this, s, background] {
            if (s.a <= 0.f) { toast("Eyedropper: nothing there (transparent)"); return; }
            const ToolRail::RGB c{s.r, s.g, s.b};
            if (background) m_rail.set_background(c); else m_rail.set_foreground(c);
        };
    });
}

// s011 m4: Select > Modify -- the number, then the mask work as a job.
void Shell::on_modify(S::Modify k) {
    if (!m_doc.selection()) { toast(std::string(S::modify_name(k)) + ": nothing is selected"); return; }
    auto& dlg = m_modify_dialogs[static_cast<int>(k)];
    if (!dlg) {
        const char* label = k == S::Modify::Border ? "Width" : k == S::Modify::Smooth ? "Sample radius"
                          : k == S::Modify::Expand ? "Expand by" : "Contract by";
        const double hi = k == S::Modify::Smooth ? 100 : k == S::Modify::Border ? 200 : 500;
        const std::string name = std::string("shell.modify_") + S::modify_name(k);
        dlg = std::make_unique<NumberDialog>(*this, name, std::string(S::modify_name(k)) + " Selection", label, "px", 1, hi,
                                             k == S::Modify::Smooth ? 2 : 8, 0);
        dlg->signal_ok().connect([this, k](double r) {
            const auto old = m_doc.selection_ptr();
            if (!old) return;
            char text[48];
            std::snprintf(text, sizeof text, "%s %g px", S::modify_name(k), r);
            const std::string label = text;
            start_job(label, [this, old, k, r, label](core::Jobs::Token& t) -> core::Jobs::Landing {
                auto f = S::modified(*old, k, r, [&t] { return t.cancelled(); });
                if (!f) return [] {};
                auto next = std::make_shared<pixel::TileMap>(std::move(*f));
                return [this, old, next, label] {
                    if (!S::set_if(m_journal, m_doc, old, std::move(*next), label) && m_doc.selection_ptr() != old)
                        toast(label + ": the selection changed meanwhile -- nothing done");
                };
            });
        });
    }
    dlg->ask();
}

void Shell::update_select_actions() {
    const bool any = m_doc.selection() != nullptr;
    if (auto it = m_layer_acts.find("deselect"); it != m_layer_acts.end()) it->second->set_enabled(any);
    if (auto it = m_layer_acts.find("select-feather"); it != m_layer_acts.end()) it->second->set_enabled(any);
    if (auto it = m_layer_acts.find("crop-selection"); it != m_layer_acts.end()) it->second->set_enabled(any);
    for (const char* a : {"select-expand", "select-contract", "select-border", "select-smooth"})
        if (auto it = m_layer_acts.find(a); it != m_layer_acts.end()) it->second->set_enabled(any);
}

}  // namespace klimt
