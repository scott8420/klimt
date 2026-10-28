#pragma once
// core::Adjust -- ADJUSTMENTS (s013). docs/adjustments.md.
//
// One value type says what an adjustment does: Levels, Curves, Hue /
// Saturation or Brightness / Contrast, with its settings. It lives in two
// places:
//
//   * an ADJUSTMENT LAYER (Kind::Adjustment) holds one in Layer::adjust; the
//     compositor applies it, live, to everything below it in its group (Pass
//     Through: below it in the document), through its mask, its opacity, its
//     blend mode and its clip -- the pixels underneath never change;
//   * Image > Adjustments applies one to a pixel layer's pixels for good
//     (through the selection), one step.
//
// Every adjustment is a function of a pixel's own colour alone (no
// neighbours), so a tile adjusts by itself and the compositor stays per tile.
// The maths works on encoded RGB 0..1, the way Photoshop's do on an RGB
// document; alpha is never touched.
#include "pixel/Pixel.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace klimt::core {

enum class AdjustKind : std::uint8_t { None, Levels, Curves, HueSat, BrightCon };

// Levels on one channel: input black / white points and gamma (the middle
// slider, 0.1 .. 9.99, 1 = none), output black / white. All 0..1.
struct LevelsCh {
    double in_black = 0, in_white = 1, gamma = 1, out_black = 0, out_white = 1;
    bool operator==(const LevelsCh&) const = default;
};

struct CurvePt {
    double x = 0, y = 0;   // input, output; 0..1
    bool operator==(const CurvePt&) const = default;
};

// Channel 0 of `levels` and `curves` is the composite (RGB); 1..3 are R, G,
// B. A channel's own setting applies first, then the composite's.
inline constexpr int kAdjustChannels = 4;

struct Adjust {
    AdjustKind kind = AdjustKind::None;
    std::array<LevelsCh, kAdjustChannels> levels{};
    // Points by x (the ends included); fewer than two: a straight line.
    std::array<std::vector<CurvePt>, kAdjustChannels> curves{};
    // Hue / Saturation: hue -180..180 degrees (Colorize: the hue, 0..360),
    // saturation -100..100 (Colorize: 0..100), lightness -100..100.
    int  hue = 0, saturation = 0, lightness = 0;
    bool colorize = false;
    // Brightness -150..150, contrast -50..100 (Photoshop's ranges; the
    // modern behaviour -- black and white stay put).
    int  brightness = 0, contrast = 0;
    bool operator==(const Adjust&) const = default;
};

namespace adjust {
std::string_view label(AdjustKind k);   // "Hue/Saturation"
std::string_view key(AdjustKind k);     // "hue-saturation" (files, scripts)
std::optional<AdjustKind> from_key(std::string_view key);
const std::vector<AdjustKind>& kinds(); // menu order

// What a new one starts as: does nothing (Curves: the diagonal).
Adjust fresh(AdjustKind k);
// Every value inside its range, curve points sorted and inside 0..1.
Adjust normalised(Adjust a);
// Does it change nothing at all?
bool identity(const Adjust& a);
// "" when `a` is well formed (a file is trusted only after this).
std::string refusal(const Adjust& a);

// The pieces, for the editors and the tests.
double levels_map(const LevelsCh& c, double v);
// The curve through `pts` (a natural cubic spline, flat past the end points,
// clamped to 0..1) sampled n times at x = i / (n - 1).
std::vector<double> curve_table(const std::vector<CurvePt>& pts, int n);
// The adjustment on one straight (not premultiplied) colour, in place.
void apply_rgb(const Adjust& a, float& r, float& g, float& b);

// The compiled form: lookup tables made once, then run over many pixels.
struct Compiled {
    AdjustKind kind = AdjustKind::None;
    std::array<std::vector<float>, 3> lut;   // Levels, Curves, Brightness / Contrast
    Adjust a;                                // Hue / Saturation runs per pixel
};
// Cached: the compositor asks for the same few per tile.
std::shared_ptr<const Compiled> compile(const Adjust& a);
// PREMULTIPLIED RGBA, n pixels, in place; alpha unchanged.
void run(const Compiled& c, float* rgba, std::size_t n);
// Straight colour through a compiled form.
void run_rgb(const Compiled& c, float& r, float& g, float& b);

// A pixel layer's pixels with `a` applied (Image > Adjustments): straight
// colour, through the selection's coverage (nullptr: everywhere), alpha as it
// was. RGB and Gray maps (Gray: the composite channel on the gray); nullopt
// for CMYK / Lab, or when cancelled.
std::optional<pixel::TileMap> applied(const pixel::TileMap& src, const Adjust& a, const pixel::TileMap* sel,
                                      const std::function<bool()>& cancelled = {});

// 256 bins per channel (0 = luminosity, 1..3 = R, G, B) of Cairo ARGB32
// pixels (premultiplied; transparent pixels are left out) -- what Levels
// draws behind its sliders.
using Histogram = std::array<std::array<std::uint32_t, 256>, 4>;
Histogram histogram(const std::vector<std::uint32_t>& argb);
}  // namespace adjust

}  // namespace klimt::core
