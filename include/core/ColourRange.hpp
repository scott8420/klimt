#pragma once
// core::select -- COLOR RANGE (s024, WEAKNESSES 24): a selection of every
// pixel whose colour is near the sampled ones -- Photoshop's Select > Color
// Range. GTK-free. docs/selection.md.
//
//   * the colours are the picture AS SEEN (the composite), sRGB 0..1;
//   * FUZZINESS (0..200, Photoshop's scale, in 0..255 units): a pixel within
//     half of it of a sample is wholly selected, fading to none at the full
//     distance (a soft-edged selection, as Photoshop's);
//   * several samples: the nearest counts; INVERT selects the rest;
//   * transparent pixels are never selected.
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/Selection.hpp"

#include <array>
#include <functional>
#include <optional>
#include <vector>

namespace klimt::core::select {

struct ColourRange {
    std::vector<std::array<float, 3>> samples;   // sRGB 0..1
    int  fuzziness = 40;                           // 0..200
    bool invert = false;
    bool operator==(const ColourRange&) const = default;
};

// How much a colour (straight sRGB 0..1) is selected, 0..1.
float colour_weight(const ColourRange& r, float red, float green, float blue);
// The mask (the selection's format) over the composite; nullopt if stopped.
std::optional<pixel::TileMap> colour_range_mask(const Image& im, const Layers& layers, const ColourRange& r,
                                                const pixel::Format& mf, const std::function<bool()>& stop = {});
// The verb: the selection becomes / gains / loses that range. ONE step, "Color Range".
bool colour_range(DocJournal& j, Document& d, const ColourRange& r, Op op = Op::Replace);

}  // namespace klimt::core::select
