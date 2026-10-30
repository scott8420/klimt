// core -- ADJUSTMENT LAYER verbs (s013); s014: apply_filter. docs/adjustments.md. (add_adjustment
// is in Document.cpp, beside add_layer.)
#include "core/Document.hpp"
#include "core/Selection.hpp"

namespace klimt::core::layers {

namespace {
// The layer's new value with `a`, or nothing when refused / unchanged.
std::optional<Layer> adjusted(const Document& d, const Id& id, const Adjust& a) {
    const auto l = d.get(id);
    if (!l || l->kind != Kind::Adjustment || d.locked_all(id) || a.kind != l->adjust.kind) return std::nullopt;
    Layer v = *l;
    v.adjust = adjust::normalised(a);
    if (v.adjust == l->adjust) return std::nullopt;
    return v;
}
std::string label_of(const Document& d, const Id& id) {
    const auto l = d.get(id);
    return std::string(adjust::label(l->adjust.kind)) + " of “" + l->name + "”";
}
}  // namespace

bool set_adjust(DocJournal& j, Document& d, const Id& id, const Adjust& a) {
    const auto v = adjusted(d, id, a);
    if (!v) return false;
    return j.run(label_of(d, id), {id}, [&] { d.put(id, *v); }, "adjust:" + id);
}

void adjust_begin(DocJournal& j, Document& d, const Id& id) {
    const auto l = d.get(id);
    if (l && l->kind == Kind::Adjustment && !d.locked_all(id)) j.begin(label_of(d, id), {id});
}

void adjust_to(DocJournal& j, Document& d, const Id& id, const Adjust& a) {
    if (!j.in_gesture()) return;   // refused at begin
    const auto v = adjusted(d, id, a);
    if (!v) return;
    j.run("Adjust", {id}, [&] { d.put(id, *v); });   // inside the gesture: just writes
}

bool adjust_end(DocJournal& j) { return j.commit(); }

bool apply_adjust(DocJournal& j, Document& d, const Id& id, const Adjust& a) {
    const auto l = d.get(id);
    if (!l || l->kind != Kind::Pixel || d.locked_all(id) || (l->locks & lock::Pixels) || !l->pixels) return false;
    const auto n = adjust::normalised(a);
    if (adjust::identity(n)) return false;
    auto out = adjust::applied(*l->pixels, n, d.selection());
    if (!out) return false;
    return set_pixels_if(j, d, id, l->pixels, std::move(*out), std::string(adjust::label(n.kind)));
}

bool apply_filter(DocJournal& j, Document& d, const Id& id, const Filter& f) {
    const auto l = d.get(id);
    if (!l || l->kind != Kind::Pixel || d.locked_all(id) || (l->locks & lock::Pixels) || !l->pixels) return false;
    const auto n = filter::normalised(f);
    if (filter::identity(n)) return false;
    auto out = filter::filtered(*l->pixels, n, d.selection(), (l->locks & lock::Alpha) != 0, nullptr, {}, {},
                                filter::wants_mask(n) ? l->mask.get() : nullptr);   // s039: the depth map
    if (!out) return false;
    return set_pixels_if(j, d, id, l->pixels, std::move(*out), std::string(filter::label(n.kind)));
}

}  // namespace klimt::core::layers
