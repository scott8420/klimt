#pragma once
// core::select -- THE COLOUR CHANNELS (s026, WEAKNESSES 24): the picture's
// own channels as grey -- Red / Green / Blue (Gray; Cyan / Magenta / Yellow /
// Black) -- to look at one alone, to load as a selection, to copy into a
// saved channel and push (Photoshop's channel masking: pick the channel with
// the most contrast -- the sky's blue against hair -- and make it a mask).
// GTK-free. docs/selection.md.
//
//   * from the picture AS SEEN (the composite), over white where it is
//     transparent;
//   * the grey is the channel's value: white = full red ... ; a CMYK plate
//     shows its INK as dark (white = no ink), as Photoshop's do;
//   * CMYK plates are worked out from the composite's RGB by the plain,
//     unmanaged formula (K = 1 - max) until colour management lands
//     (WEAKNESSES 14); Lab shows its Lightness only.
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/Selection.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::select {

std::vector<std::string> colour_channels(const pixel::Format& f);   // their names, in order
// Channel k's grey (0..1) for a straight sRGB colour.
float channel_grey(pixel::Mode m, int k, float r, float g, float b);
// Channel k as a mask (the selection's format): white = selected.
std::optional<pixel::TileMap> colour_channel_mask(const Image& im, const Layers& layers, int k, const pixel::Format& mf,
                                                  const std::function<bool()>& stop = {});
// The verbs. Load: channel k into the selection by `op` -- ONE step ("Load
// Red Channel"). Duplicate: channel k as a new saved channel ("Red copy").
bool load_colour_channel(DocJournal& j, Document& d, int k, Op op = Op::Replace);
Document::Id duplicate_colour_channel(DocJournal& j, Document& d, int k);

}  // namespace klimt::core::select
