#pragma once
// core::fill -- EDIT > CONTENT-AWARE FILL (s088; REQUIREMENTS "Added": LaMa
// fill; docs/ai.md). The selection filled from what's around it by LaMa
// (big-mask inpainting, libs/seg) -- on this machine.
//
// LaMa sees a 512 square. The model looks at the hole's box grown by its own
// size each way (at least 32 px) -- the context it borrows from -- scaled so
// the longer side is 512 (up for a small hole, down for a big one; edges
// repeated to fill the square), and what it paints comes back to the
// picture's size. Only the hole is taken from it, its rim feathered a couple
// of pixels so no seam shows. GTK-free; pure; on a worker.
#include "pixel/Pixel.hpp"
#include "seg/Seg.hpp"

#include <functional>
#include <optional>
#include <string>

namespace klimt::core::fill {

// The context LaMa sees for a hole whose bounds are `hole`, kept on the picture.
pixel::Rect context_box(const pixel::Rect& hole, int width, int height);

// `picture`: what the eye sees (RGB, any depth); `hole`: the selection's
// coverage, the picture's size. Back: a LAYER's pixels in `out_format` --
// the fill inside the hole (feathered at its rim), transparent elsewhere;
// nullopt with `error` ("nothing is selected", "cancelled" ...).
std::optional<pixel::TileMap> content_aware(const pixel::TileMap& picture, const pixel::TileMap& hole, const pixel::Format& out_format,
                                            const seg::Model& model, const std::string& model_path, std::string& error,
                                            const std::function<bool()>& cancelled = {});

}  // namespace klimt::core::fill
