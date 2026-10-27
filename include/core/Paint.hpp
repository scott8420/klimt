#pragma once
// core::Paint -- the Brush and the Eraser (s008 m4). GTK-free; docs/paint.md.
//
// "A modest brush -- painting is Krita's" (the tool note): round, size,
// hardness, opacity; the foreground colour; through the selection; the
// locks bite. One stroke = ONE step.
//
// THE MODEL. A stroke keeps a COVERAGE map (float, the tile grid): each dab
// raises coverage to max(coverage, dab), so a stroke never builds past its
// opacity where it crosses itself (Photoshop's opacity, not its flow). The
// layer during the stroke is always   original  (+)  colour at
// coverage x opacity x selection   -- recomputed from the ORIGINAL pixels,
// for the tiles the new dabs touched. So any moment of the stroke is exact,
// and the work is pure: a worker computes the next layer map from (original,
// current, coverage, dabs); the UI only lands it inside the stroke's gesture.
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/Selection.hpp"

#include <vector>

namespace klimt::core::paint {

struct Brush {
    double     size = 30;          // diameter, image pixels (1 .. 2500)
    double     hardness = 0.8;     // 0 soft .. 1 hard edge
    double     opacity = 1;        // 0 .. 1, the stroke's ceiling
    bool       erase = false;      // the Eraser: alpha down (no alpha: paints `background`)
    pixel::Px  colour;             // straight, the document's format (alpha ignored)
    pixel::Px  background;         // what the Eraser paints on a layer with no alpha
    bool       alpha_lock = false; // colour only, where there is something
    double spacing() const { return std::max(0.5, size * 0.25); }
};

// The dab centres from `from` to `to`, `spacing` apart, carrying the leftover
// distance in `carry` across calls (so a slow drag spaces dabs like a fast one).
// The first call of a stroke: carry = spacing, so it starts with a dab at `from`.
std::vector<select::Pt> dabs_along(select::Pt from, select::Pt to, double spacing, double& carry);

// The dab's strength at distance d from its centre (radius r = size / 2).
float dab_alpha(double d, const Brush& b);

// Coverage += the dabs (by max). `touched` (on the coverage's grid) gains
// every tile a dab reached. Coverage is F32 Gray, no alpha, at the image size.
pixel::TileMap add_dabs(const pixel::TileMap& cov, const Brush& b, const std::vector<select::Pt>& dabs, TileSet& touched);
pixel::TileMap new_coverage(int w, int h);

// The layer as the stroke has it: `current` with each `touched` tile
// recomputed from `original`, `cov`, the selection `mask` and the brush.
pixel::TileMap apply(const pixel::TileMap& original, const pixel::TileMap& current, const pixel::TileMap& cov,
                     const pixel::TileMap* mask, const Brush& b, const TileSet& touched);

// May this layer be painted? "" yes, else why not (for a toast).
std::string refusal(const Document& d, const Document::Id& id, const Brush& b);

// A whole stroke, synchronously, as ONE step (the verb table; tests). The
// app does the same in pieces on workers (Shell_paint.cpp).
bool stroke(DocJournal& j, Document& d, const Document::Id& id, const std::vector<select::Pt>& path, Brush b);

}  // namespace klimt::core::paint
