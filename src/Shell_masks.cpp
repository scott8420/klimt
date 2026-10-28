#include "Shell.hpp"
#include "Log.hpp"
#include "core/Composite.hpp"
#include "core/Selection.hpp"
#include "core/Gradient.hpp"

// Shell_masks.cpp -- LAYER MASKS (s012 m3). The Layer > Layer Mask menu, the
// panel's Add Mask button, and Alt-click's view of the mask. Painting the
// mask is the Brush / Eraser / Fill with the mask as their target
// (Shell_paint.cpp, Shell_select.cpp): white shows, black hides.

namespace klimt {

namespace L = core::layers;

// What a colour paints into a mask (stored as what it HIDES): white 0, black 1.
float Shell::mask_hide_of(ToolRail::RGB c) const {
    return std::clamp(1.f - (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b), 0.f, 1.f);
}

void Shell::on_mask_add(int from) {
    const auto id = m_layers.primary();
    const auto l = m_doc.get(id);
    if (!l) { toast("Layer Mask: select a layer first"); return; }
    if (l->kind == core::Kind::Group) { toast("Layer Mask: “" + l->name + "” is a group -- masks are on layers (for now)"); return; }
    if (l->mask) { toast("Layer Mask: “" + l->name + "” has one already"); return; }
    if (m_doc.locked_all(id)) { toast("Layer Mask: “" + l->name + "” is locked"); return; }
    auto f = static_cast<L::MaskFrom>(from);
    if (from < 0) f = m_doc.selection() ? L::MaskFrom::RevealSelection : L::MaskFrom::RevealAll;   // the panel's button
    if ((f == L::MaskFrom::RevealSelection || f == L::MaskFrom::HideSelection) && !m_doc.selection()) {
        toast("Layer Mask: nothing is selected");
        return;
    }
    if (L::add_mask(m_journal, m_doc, id, f)) {
        m_layers.set_mask_target(id);   // Photoshop's way: the new mask is what's painted next
        toast("Painting the mask of “" + l->name + "” -- white shows, black hides; click the layer's icon for its pixels");
    }
}

void Shell::on_mask_verb(const std::string& verb) {
    const auto id = m_layers.primary();
    const auto l = m_doc.get(id);
    if (!l || !l->mask) { toast("Layer Mask: the selected layer has no mask"); return; }
    if (verb == "delete") L::delete_mask(m_journal, m_doc, id);
    else if (verb == "toggle") L::set_mask_on(m_journal, m_doc, id, !l->mask_on);
    else if (verb == "invert") L::invert_mask(m_journal, m_doc, id);
    else if (verb == "edit") m_layers.set_mask_target(m_layers.mask_target() == id ? std::string{} : id);
    else if (verb == "apply") {
        if (!m_doc.image().format.alpha) { toast("Apply Layer Mask: this image has no transparency to apply it to"); return; }
        if (l->locks & (core::lock::Pixels | core::lock::Alpha)) { toast("Apply Layer Mask: “" + l->name + "” is locked"); return; }
        L::apply_mask(m_journal, m_doc, id);
    }
}

// Alt-click a mask chip: the canvas shows that mask (white shows, black
// hides); again, or another layer's, and it goes back.
void Shell::on_mask_view(const std::string& id) {
    m_mask_view = m_mask_view == id ? std::string{} : id;
    m_layers.set_mask_viewing(m_mask_view);
    m_dirty = {};   // everything again, the other way
    m_shown = {};
    queue_render();
    if (!m_mask_view.empty()) toast("Showing the mask -- Alt-click it again to see the image");
}

// s012 m4: the Gradient tool -- on the layer's pixels, or its mask when the
// mask is what's painted; through the selection; a job, ONE step.
void Shell::on_gradient(core::select::Pt a, core::select::Pt b) {
    namespace G = core::gradient;
    if (m_xform.active || m_stroke.active || m_move.active) return;
    const auto id = m_layers.primary();
    const auto l = m_doc.get(id);
    if (!l) { toast("Gradient: select a layer first"); return; }
    const bool on_mask = l->mask && m_layers.mask_target() == id;
    if (!on_mask && l->kind != core::Kind::Pixel) { toast("Gradient: " + L::not_pixels(m_doc, id)); return; }
    if (m_doc.locked_all(id) || (!on_mask && (l->locks & core::lock::Pixels))) { toast("Gradient: “" + l->name + "” is locked"); return; }
    const auto o = m_strip.gradient();
    auto c0 = m_rail.foreground(), c1 = m_rail.background();
    if (o.reverse && !o.to_transparent) std::swap(c0, c1);
    G::Spec s;
    s.shape = m_strip.variant("gradient") == 1 ? G::Shape::Radial : G::Shape::Linear;
    s.a = a;
    s.b = b;
    s.opacity = o.opacity / 100.0;
    const auto fmt = m_doc.image().format;
    if (on_mask) {
        s.from.c[0] = mask_hide_of(c0);
        s.to.c[0] = o.to_transparent ? s.from.c[0] : mask_hide_of(c1);
    } else {
        s.from = core::from_srgb(fmt, c0.r, c0.g, c0.b);
        s.to = o.to_transparent ? s.from : core::from_srgb(fmt, c1.r, c1.g, c1.b);
        if (o.to_transparent && fmt.alpha) s.to.c[static_cast<std::size_t>(fmt.channels() - 1)] = 0.f;
        if (o.reverse && o.to_transparent) std::swap(s.from, s.to);
        s.alpha_lock = (l->locks & core::lock::Alpha) != 0;
    }
    const auto before = on_mask ? l->mask : l->pixels;
    const auto sel = m_doc.selection_ptr();
    const auto blank = m_doc.blank();
    const std::string label = on_mask ? "Gradient on Mask" : "Gradient";
    start_job(label, [this, id, s, on_mask, before, sel, blank, label](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto out = G::over(before ? *before : blank, sel.get(), s, [&t] { return t.cancelled(); });
        if (!out) return [] {};
        auto res = std::make_shared<pixel::TileMap>(std::move(*out));
        return [this, id, on_mask, before, sel, res, label] {
            const bool ok = m_doc.selection_ptr() == sel &&
                (on_mask ? core::layers::set_mask_if(m_journal, m_doc, id, before, std::move(*res), label)
                         : core::layers::set_pixels_if(m_journal, m_doc, id, before, std::move(*res), label));
            if (!ok) toast(label + ": the layer changed meanwhile -- nothing done");
        };
    });
}

}  // namespace klimt
