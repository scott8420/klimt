#pragma once
// core::Gradient -- the Gradient tool (s012 m4). GTK-free; docs/paint.md.
//
// Linear (along the dragged line) or radial (out from its start), from one
// colour to another (or to transparent), laid over the target -- a layer's
// pixels or its mask -- through the selection, at an opacity. 8-bit and
// 16-bit are dithered (no banding). One step.
#include "core/Document.hpp"
#include "core/Selection.hpp"

#include <functional>
#include <optional>

namespace klimt::core::gradient {

enum class Shape { Linear, Radial };

struct Spec {
    Shape      shape = Shape::Linear;
    select::Pt a, b;                 // the dragged line, image coords
    pixel::Px  from, to;             // straight, the target's format (alpha used when it has one)
    double     opacity = 1;          // 0..1
    bool       alpha_lock = false;   // colour only, where there is something
};

// Where along the gradient (0..1) the pixel centre (x + .5, y + .5) falls.
double position(const Spec& s, double x, double y);

// `target` with the gradient laid over it, through `mask` (nullptr: all).
// nullopt when cancelled.
std::optional<pixel::TileMap> over(const pixel::TileMap& target, const pixel::TileMap* mask, const Spec& s,
                                   const std::function<bool()>& cancelled = {});

// The verb: on layer `id`'s pixels (or, on_mask, its mask -- `from` / `to`
// then carry the hide amount in channel 0), through the document's
// selection, as ONE step "Gradient". Refused as the Brush is (a group, the
// locks); on a mask, only All.
bool apply(DocJournal& j, Document& d, const Document::Id& id, Spec s, bool on_mask = false);

}  // namespace klimt::core::gradient
