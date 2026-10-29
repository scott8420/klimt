#pragma once
// core::pattern -- BUILT-IN PATTERNS (s028 m4), for the Pattern Overlay layer
// style. GTK-free, procedural: a pattern is a function of the image pixel, so
// a file keeps only which one (and its scale / offset) -- nothing to embed,
// nothing to lose. Every pattern tiles seamlessly at its own period.
//
// Greys. The geometric ones (Checker, Stripes, Dots, Grid, Bricks, Hatch)
// are dark on white -- they read as they are in Normal and Multiply. The
// textures (Paper, Canvas) sit around a middle grey, so in Overlay / Soft
// Light they texture what's under them without lightening or darkening it.
#include "pixel/Pixel.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::pattern {

struct Info {
    const char* key;    // in the file
    const char* name;   // in the UI
    int period;         // px at 100 %: where it repeats
};
// The built-in patterns, in the order the UI lists them (the index is the id).
const std::vector<Info>& builtin();
int index_of(const std::string& key);   // -1 when unknown

// The pattern's grey (0 black .. 1 white) at the image point (x, y) -- the
// pixel's centre is (x + 0.5, y + 0.5) -- at `scale` (1 = 100 %), moved by
// (ox, oy) px. Box-filtered over the pixel when the pattern is small enough
// to alias.
float grey(int id, double x, double y, double scale, double ox, double oy);

// ── s033: YOUR patterns ─────────────────────────────────────────────────────
// Pixels: straight RGB + alpha, 8-bit, at most kMaxSide a side.
inline constexpr int kMaxSide = 1024;
// A user pattern's colour (straight sRGB 0..1, and alpha) at the image point
// (x, y): tiled, at `scale`, moved by (ox, oy); box-filtered below 100 %.
void sample(const pixel::TileMap& img, double x, double y, double scale, double ox, double oy, float out[4]);

// THE LIBRARY: the app's patterns, one PNG each in a folder (the UI's
// ~/.config/klimt/patterns), named by their file. Loaded when the folder is
// set; add / remove write the folder. GTK-free.
struct UserPattern {
    std::string name;
    std::shared_ptr<const pixel::TileMap> image;
};
void set_library_dir(const std::string& dir);
const std::vector<UserPattern>& library();
// Added under `want` (trimmed; "Pattern" when blank; a name taken gets " 2"
// ...); the name used, or "" with `error`.
std::string add_to_library(const pixel::TileMap& img, const std::string& want, std::string& error);
bool remove_from_library(const std::string& name);
// The library's pattern by name; nullptr: none.
std::shared_ptr<const pixel::TileMap> find(const std::string& name);
}  // namespace klimt::core::pattern

#include "core/Composite.hpp"
namespace klimt::core::pattern {
// Edit > Define Pattern: what the eye sees inside `r` (the picture's pixels,
// every visible layer -- flattened, straight RGB + alpha, 8-bit). nullopt
// with `why`: an empty rectangle, one larger than kMaxSide a side, or
// nothing there.
std::optional<pixel::TileMap> define(const Image& im, const Layers& layers, pixel::Rect r, std::string& why);

}  // namespace klimt::core::pattern
