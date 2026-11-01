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

// s061 (WEAKNESSES 34): Invert, Posterize, Threshold, Exposure -- appended,
// so the numbers already in use keep their meaning.
// s062: Vibrance, Black & White, Photo Filter -- appended the same way.
enum class AdjustKind : std::uint8_t { None, Levels, Curves, HueSat, BrightCon, Invert, Posterize, Threshold, Exposure,
                                       Vibrance, BlackWhite, PhotoFilter,
                                       // s063: the rest of WEAKNESSES 34.
                                       ColourBalance, ChannelMixer, SelectiveColour, GradientMap };

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

// s064: one of Hue/Saturation's six colour ranges (Reds .. Magentas): its
// hues in degrees 0..359 -- where the fall-off starts, the band (full
// strength) starts and ends, the fall-off ends, going round the wheel -- and
// what it adds to the master's hue, saturation, lightness there.
struct HueRange {
    std::array<int, 4> band{};
    int hue = 0, saturation = 0, lightness = 0;
    bool operator==(const HueRange&) const = default;
};
inline constexpr std::array<std::array<int, 4>, 6> kHueBands{{   // Photoshop's
    {315, 345, 15, 45}, {15, 45, 75, 105}, {75, 105, 135, 165}, {135, 165, 195, 225}, {195, 225, 255, 285}, {255, 285, 315, 345}}};

// s063: one colour stop of a Gradient Map.
struct GradStop {
    double at = 0;                  // where along the gradient, 0..1
    double mid = 0.5;               // between the stop before and this one, where
                                    // the two mix half and half (0.05..0.95)
    std::array<double, 3> rgb{};    // sRGB 0..1
    bool operator==(const GradStop&) const = default;
};
inline constexpr std::size_t kGradStops = 32;   // the most a Gradient Map holds

struct Adjust {
    AdjustKind kind = AdjustKind::None;
    std::array<LevelsCh, kAdjustChannels> levels{};
    // Points by x (the ends included); fewer than two: a straight line.
    std::array<std::vector<CurvePt>, kAdjustChannels> curves{};
    // Hue / Saturation: hue -180..180 degrees (Colorize: the hue, 0..360),
    // saturation -100..100 (Colorize: 0..100), lightness -100..100.
    int  hue = 0, saturation = 0, lightness = 0;
    bool colorize = false;
    // s064: the colour ranges (Colorize leaves them be).
    std::array<HueRange, 6> ranges{{{kHueBands[0]}, {kHueBands[1]}, {kHueBands[2]}, {kHueBands[3]}, {kHueBands[4]}, {kHueBands[5]}}};
    // Brightness -150..150, contrast -50..100 (Photoshop's ranges; the
    // modern behaviour -- black and white stay put).
    int  brightness = 0, contrast = 0;
    // s061: Posterize -- levels per channel, 2..255 (Photoshop's 4 to start).
    int  posterize = 4;
    // s061: Threshold -- the level, 1..255: luminance at or above it is
    // white, below it black (Photoshop's 128 to start).
    int  threshold = 128;
    // s061: Exposure, Photoshop's, worked in linear light (sRGB decoded):
    // exposure -20..20 stops, offset -0.5..0.5, then gamma 0.01..9.99.
    double exposure = 0, offset = 0, exp_gamma = 1;
    // s062: Vibrance -100..100 (Saturation is `saturation` above, -100..100):
    // vibrance lifts the dull colours most and leaves the strong ones be.
    int  vibrance = 0;
    // s062: Black & White -- how light each hue turns, percent -200..300:
    // reds, yellows, greens, cyans, blues, magentas (Photoshop's defaults);
    // Tint: the grey coloured, hue 0..359, saturation 0..100.
    std::array<int, 6> bw{40, 60, 40, 60, 20, 80};
    bool bw_tint = false;
    int  tint_hue = 42, tint_sat = 20;
    // s062: Photo Filter -- the filter's colour (sRGB 0..1; Photoshop's
    // Warming Filter (85) to start), density 1..100 %, Preserve Luminosity.
    std::array<double, 3> filter{236 / 255.0, 138 / 255.0, 0.0};
    int  density = 25;
    // Photo Filter's Preserve Luminosity; s063: Colour Balance's too.
    bool keep_luma = true;
    // s063: Colour Balance -- per tone (shadows, midtones, highlights) the
    // cyan-red, magenta-green, yellow-blue shifts, -100..100.
    std::array<std::array<int, 3>, 3> balance{};
    // s063: Channel Mixer -- each output (red, green, blue): how much of the
    // red, green and blue goes into it, percent -200..200, and a constant
    // -200..200 (100: white added). Monochrome: one grey, from `mono`.
    std::array<std::array<int, 4>, 3> mixer{{{100, 0, 0, 0}, {0, 100, 0, 0}, {0, 0, 100, 0}}};
    std::array<int, 4> mono{40, 40, 20, 0};
    bool monochrome = false;
    // s063: Selective Colour -- for each colour (reds, yellows, greens,
    // cyans, blues, magentas, whites, neutrals, blacks) the cyan, magenta,
    // yellow and black added or taken, percent -100..100; Relative (to the
    // ink already there, Photoshop's default) or Absolute.
    std::array<std::array<int, 4>, 9> selective{};
    bool selective_absolute = false;
    // s063: Gradient Map -- each pixel's luminosity looked up along the
    // gradient (black to white to start: Photoshop's foreground to
    // background). Two to kGradStops stops, by `at`. Reverse runs it the
    // other way; Dither is kept (and carried to PSD) but not drawn yet.
    std::vector<GradStop> grad{{0, 0.5, {0, 0, 0}}, {1, 0.5, {1, 1, 1}}};
    bool grad_reverse = false, grad_dither = false;
    bool operator==(const Adjust&) const = default;
};

namespace adjust {
std::string_view label(AdjustKind k);   // "Hue/Saturation"
std::string_view key(AdjustKind k);     // "hue-saturation" (files, scripts)
std::optional<AdjustKind> from_key(std::string_view key);
const std::vector<AdjustKind>& kinds(); // menu order

// What a new one starts as: does nothing (Curves: the diagonal) -- except
// the kinds that are their own effect (Invert; Posterize at 4 levels;
// Threshold at 128), as Photoshop's start.
Adjust fresh(AdjustKind k);
// Every value inside its range, curve points sorted and inside 0..1.
Adjust normalised(Adjust a);
// Does it change nothing at all?
bool identity(const Adjust& a);
// s061: no settings at all (Invert): Image > Adjustments runs it at once.
bool settingless(AdjustKind k);
// s062: shows the histogram of what it changes behind its controls
// (Levels, Curves, Threshold).
bool wants_histogram(AdjustKind k);
// "" when `a` is well formed (a file is trusted only after this).
std::string refusal(const Adjust& a);

// The pieces, for the editors and the tests.
double levels_map(const LevelsCh& c, double v);
// The curve through `pts` (a natural cubic spline, flat past the end points,
// clamped to 0..1) sampled n times at x = i / (n - 1).
std::vector<double> curve_table(const std::vector<CurvePt>& pts, int n);
// s062: RGB <-> HSL, all 0..1 (hue in turns) -- Hue/Saturation's, and the
// PSD's Black & White tint.
void to_hsl(double r, double g, double b, double& h, double& s, double& l);
void from_hsl(double h, double s, double l, double& r, double& g, double& b);
// s063: the gradient's colour at t (0..1), stops by `at`, each segment bent
// so it is half and half at its midpoint.
std::array<double, 3> gradient_at(const std::vector<GradStop>& stops, double t);
// s064: how much of colour range `r` a hue (degrees) is, 0..1.
double range_weight(const std::array<int, 4>& band, double hue);
// The adjustment on one straight (not premultiplied) colour, in place.
void apply_rgb(const Adjust& a, float& r, float& g, float& b);

// The compiled form: lookup tables made once, then run over many pixels.
struct Compiled {
    AdjustKind kind = AdjustKind::None;
    std::array<std::vector<float>, 3> lut;   // Levels, Curves, Brightness / Contrast; s061: Invert, Posterize, Exposure
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
// s064: Auto, Photoshop's default ("Enhance Per Channel Contrast"): each of
// R, G, B stretched so its darkest and lightest -- past `clip` of the pixels
// at each end -- become black and white. Levels: the channels' input points;
// Curves: each channel a straight line through those two. The rest of `a`
// (and the composite channel) kept. A channel with nothing in it is left.
Adjust auto_tone(const Adjust& a, const Histogram& h, double clip = 0.001);
// s064: Levels / Curves' eyedroppers -- `rgb` (0..1, picked on the canvas)
// set as each channel's black point, white point, or (grey) made neutral
// at the average of what its three channels map to now. Levels: the input
// black / white points, or each channel's gamma; Curves: the end point, or a
// point through it. The composite channel is left as it is.
enum class Dropper : std::uint8_t { None, Black, Grey, White };
Adjust dropped(const Adjust& a, Dropper d, const std::array<double, 3>& rgb);
}  // namespace adjust

}  // namespace klimt::core
