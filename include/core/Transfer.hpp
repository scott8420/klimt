#pragma once
// core::transfer -- LAYERS FROM ONE DOCUMENT INTO ANOTHER (s022): a layer
// dragged onto another tab, Layer > Duplicate To, an image file dropped onto
// the canvas. GTK-free. docs/tabs.md.
//
//   take()     the layers (each selected top with everything inside it) as
//              values, with the image they came from -- a snapshot, cheap
//              (maps of shared pixels);
//   refusal()  "" when they can go into an image of that format, else why
//              not (CMYK / Lab across modes; into an image without alpha);
//   paste()    fitted to the target -- every map converted to its format
//              (depth; RGB <-> Gray), the dragged CONTENT centred on its
//              canvas (what falls beyond is kept beyond) -- then ONE step,
//              fresh ids, above `above`.
#include "core/Document.hpp"

#include <string>
#include <utility>
#include <vector>

namespace klimt::core::transfer {

struct Bundle {
    Image image;                                       // the source image (size, format)
    std::vector<std::pair<Document::Id, Layer>> layers;   // parents before children
    std::vector<Document::Id> roots;                   // the tops, panel order
    std::string name;                                  // what it's called in a label
};

Bundle take(const Document& d, const std::vector<Document::Id>& ids);
// A whole flat image (a file dropped on the canvas) as one pixel layer.
Bundle of_image(const Image& im, std::shared_ptr<const pixel::TileMap> pixels, const std::string& name);
std::string refusal(const Bundle& b, const Image& into);
// One map in another format (same size): depth re-encoded, RGB <-> Gray.
pixel::TileMap converted(const pixel::TileMap& m, const pixel::Format& to);

}  // namespace klimt::core::transfer

namespace klimt::core::layers {
std::vector<Id> paste(DocJournal& j, Document& d, const transfer::Bundle& b, const Id& above = {});
}  // namespace klimt::core::layers
