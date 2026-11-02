#pragma once
// core::subject -- SELECT > SUBJECT (s086; REQUIREMENTS "Added"; docs/ai.md).
// The thing that stands out in the picture, as a selection, found by a local
// model (libs/seg) -- nothing leaves the machine.
//
// Scott: a single click gives too little context; a rough Lasso says "the
// item is in here". So with a selection already made, the model looks only
// inside its box (padded a tenth, so it sees the edges), and what it finds is
// cut to the selection; with none, the whole picture. The model's own soft
// edge is kept (Refine Edges takes it further). GTK-free; pure; on a worker.
#include "pixel/Pixel.hpp"
#include "seg/Seg.hpp"

#include <functional>
#include <optional>
#include <string>

namespace klimt::core::subject {

// The box the model looks at: the selection's bounds padded by a tenth of
// their larger side (at least 16 px), kept on the picture; the whole picture
// with no selection (or an empty one).
pixel::Rect look_box(const pixel::TileMap* within, int width, int height);

// `picture`: what the eye sees (any RGB format). `within`: the selection's
// coverage (nullptr: none). Back: the selection, `mask_format` at the
// picture's size; nullopt with `error` ("cancelled" when it was).
std::optional<pixel::TileMap> mask(const pixel::TileMap& picture, const pixel::TileMap* within, const pixel::Format& mask_format,
                                   const seg::Model& model, const std::string& model_path, std::string& error,
                                   const std::function<bool()>& cancelled = {});

// s089: OBJECT SELECT -- the one thing inside `box` (dragged round it): the
// subject model looks at the box padded, and what it finds is cut to the
// box grown a little (a box drawn tight still keeps the edge). nullopt with
// `error` ("nothing stands out there" when the box holds no one thing).
std::optional<pixel::TileMap> object(const pixel::TileMap& picture, const pixel::Rect& box, const pixel::Format& mask_format,
                                     const seg::Model& model, const std::string& model_path, std::string& error,
                                     const std::function<bool()>& cancelled = {});

// s087: LAYER > REMOVE BACKGROUND -- the layer mask (how much it HIDES, in
// `mask_format`): the subject as above, its edge then taken from the
// picture's own colours within `edge` px (core/Matte's soft edge -- hair and
// fur the model only half sees); 0: the model's edge as it is.
std::optional<pixel::TileMap> background_mask(const pixel::TileMap& picture, const pixel::TileMap* within,
                                              const pixel::Format& mask_format, const seg::Model& model,
                                              const std::string& model_path, double edge, std::string& error,
                                              const std::function<bool()>& cancelled = {});
// The edge Remove Background uses on a picture this size: about 0.4 % of its
// larger side, 2 .. 24 px.
double edge_for(int width, int height);

}  // namespace klimt::core::subject
