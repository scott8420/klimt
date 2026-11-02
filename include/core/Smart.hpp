#pragma once
// core::smart -- SMART LAYERS (s082; REQUIREMENTS "Added" 8; docs/smart.md).
//
// A placed picture that stays whole: the layer (Kind::Smart) keeps the
// picture at its own size (smart::Smart, core/SmartSpec.hpp) and a map onto
// the canvas, and its pixels are always drawn fresh from them. Scale it to a
// tenth and back -- nothing is lost; turn it, skew it, put it in perspective
// -- each a new map, drawn from the picture once. Its LIVE FILTERS run on
// what it draws (they are the "smart filters"), its mask, styles, opacity
// and blend are a layer's.
//
// EMBEDDED: the .klimt carries the picture. LINKED: it remembers the file
// (path, size, last change) and Update reads it again when it has changed;
// the .klimt carries the picture too, so a file moved away still opens.
//
// Painting, Liquify and Warp take pixels: they're refused on a smart layer
// (Layer > Smart Layer > Rasterize first). GTK-free; rendering is pure.
#include "core/Document.hpp"
#include "core/Transform.hpp"
#include "formats/Formats.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace klimt::core::smart {

xform::Projective map_of(const Smart& s);
Smart with_map(const Smart& s, const xform::Projective& m);
// The picture drawn on a canvas of the image's size and format (nullptr:
// nothing lands on it). A map that only moves pixels whole (or flips / turns
// them a quarter) copies them exactly. nullopt: cancelled. Pure.
std::optional<std::shared_ptr<const pixel::TileMap>> rendered(const Image& im, const Smart& s,
                                                              const std::function<bool()>& cancelled = {});
// Where the picture's corners land (tl, tr, br, bl), and how much it is
// scaled (the map's mean linear scale at its centre; 1 = 100 %).
xform::Quad corners(const Smart& s);
double scale_of(const Smart& s);

// "" when a picture of format `f` can be placed into `into`, else why not.
std::string refusal(const pixel::Format& f, const Image& into);

// Reading a file to place or update: what the eye sees in it (a layered
// file flattened), its resolution, and the file's size and last change.
struct Loaded {
    pixel::TileMap pixels;
    double         dpi = 72;
    std::int64_t   stamp = 0;
    std::uint64_t  bytes = 0;
    std::string    name;   // the file's name
};
std::optional<Loaded> load(const std::string& path, std::string& error, const formats::Progress& p = {});
// A file's change stamp now: {exists, stamp, bytes}.
struct Stamp { bool exists = false; std::int64_t stamp = 0; std::uint64_t bytes = 0; };
Stamp stamp_of(const std::string& path);
// A linked picture's state: Embedded; Current; Modified (the file changed
// since it was read); Missing (no file there).
enum class Link { Embedded, Current, Modified, Missing };
Link status(const Smart& s);
const char* link_label(Link l);   // "embedded", "linked", "linked -- the file has changed", "linked -- the file is missing"

}  // namespace klimt::core::smart

namespace klimt::core::layers {

// File > Place: `picture` (any format refusal() allows) as a new smart layer
// above `above`, centred on the canvas -- at 100 %, or scaled down to fit
// when it is bigger. `link` "" embeds; else the file it came from, with
// its stamp. ONE step "Place “name”". "" refused.
Id place_smart(DocJournal& j, Document& d, const pixel::TileMap& picture, const std::string& name, double dpi,
               const std::string& link = {}, std::int64_t stamp = 0, std::uint64_t bytes = 0, const Id& above = {});
// Layer > Smart Layer > Convert to Smart Layer: a pixel layer's pixels (cut
// to what it holds) become its picture, the map placing them where they
// were -- the look unchanged. ONE step.
bool to_smart(DocJournal& j, Document& d, const Id& id);
// > Rasterize: the smart layer becomes a pixel layer of what it draws. ONE step.
bool rasterize_smart(DocJournal& j, Document& d, const Id& id);
// > Replace Contents / Update Modified Content: a new picture in the same
// place (centred where the old one was, the same map otherwise). `label`
// names the step. Refused: not a smart layer, refusal(), no change.
bool replace_smart(DocJournal& j, Document& d, const Id& id, const pixel::TileMap& picture, const std::string& name,
                   double dpi, const std::string& link, std::int64_t stamp, std::uint64_t bytes, const std::string& label);
// > Embed Linked: forget the file, keep the picture. ONE step.
bool embed_smart(DocJournal& j, Document& d, const Id& id);
// The map set (Properties' scale, tests): ONE step named `label`.
bool set_smart_map(DocJournal& j, Document& d, const Id& id, const xform::Projective& m, const std::string& label);

// A smart layer "L6" at the top holding `picture` at map `m`, LOADED (a verb's setup; tests).
void load_smart_layer(Document& d, const pixel::TileMap& picture, const xform::Projective& m);

}  // namespace klimt::core::layers
