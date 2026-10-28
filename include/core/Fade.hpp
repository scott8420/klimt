#pragma once
// core::fade -- EDIT > FADE (s020). The last step's pixels mixed back toward
// what it began from: the step's result as a layer at an opacity, in a blend
// mode, over the layer as it was. Photoshop's Fade (after a filter, an
// adjustment, a stroke, a fill), as its OWN step ("Fade Gaussian Blur"): undo
// takes the fade off and leaves the filter. docs/fade.md.
//
//   * WHAT can be faded: the last step DONE, when it changed one pixel
//     layer's pixels and nothing else of it, and the layer still holds that
//     result. Not a fade of a fade.
//   * Normal mixes straight (an erase faded half-way is half erased); the
//     other modes are the compositor's (W3C, premultiplied) at the opacity --
//     Dissolve dithers. Non-RGB documents: Normal only.
//   * Only the tiles the step changed are worked; the rest stay shared.
#include "core/Document.hpp"
#include "pixel/Blend.hpp"
#include "pixel/Pixel.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace klimt::core::fade {

using MapPtr = std::shared_ptr<const pixel::TileMap>;

struct Source {
    Document::Id id;          // the layer
    std::string  what;        // the step's label ("Gaussian Blur") -- "Fade <what>"
    MapPtr       before;      // its pixels before the step (nullptr: none)
    MapPtr       after;       // its pixels now (the step's result)
};
// What Fade would fade, or nothing.
std::optional<Source> source(const DocJournal& j, const Document& d);
// The mode works on this format? (Normal always; the others in RGB.)
bool supports(const pixel::Format& f, pixel::Blend mode);
// before -> after, faded: `after` at `opacity` (0..1) in `mode` over
// `before`. nullopt when `stop` says so (a cancelled job). `before` may be
// nullptr (the layer had no pixels).
std::optional<pixel::TileMap> faded(const MapPtr& before, const pixel::TileMap& after, double opacity,
                                    pixel::Blend mode, const std::function<bool()>& stop = {});

}  // namespace klimt::core::fade

namespace klimt::core::layers {
// The verb, done here and now (the selftest's; the Shell runs faded() on a
// worker and lands with set_pixels_if): `id`'s pixels (still `expect`) faded
// toward `before`. ONE step, "Fade <what>".
bool fade(DocJournal& j, Document& d, const Id& id, const fade::MapPtr& before, const fade::MapPtr& expect,
          double opacity, pixel::Blend mode, const std::string& what);
}  // namespace klimt::core::layers
