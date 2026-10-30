#pragma once
// core::Filter -- THE FILTER MENU (s014). docs/filters.md.
//
// One value type says what a filter does -- Gaussian Blur, Unsharp Mask, High
// Pass, Add Noise; m3: Box Blur, Motion Blur, Median -- with its settings.
// Filter > ... applies one to a pixel layer's pixels for good, through the
// selection, as ONE step (Photoshop's classic way; live "smart" filters on a
// layer come later).
//
// Unlike an adjustment a filter reads a pixel's NEIGHBOURS (reach() of them),
// so it can't run inside the per-tile compositor: it works on bands of tile
// rows, each band reading reach() pixels around it, the bands spread over the
// cores. Blurs work on PREMULTIPLIED colour, so transparent pixels don't bleed
// black into their neighbours; the image's edges extend (clamp).
#include "pixel/Pixel.hpp"
#include "pixel/Blend.hpp"   // s023: a live filter's blending

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace klimt::core {

enum class FilterKind : std::uint8_t { None, GaussianBlur, UnsharpMask, HighPass, AddNoise, BoxBlur, MotionBlur, Median,
                                       SmartSharpen, DustScratches, ReduceNoise,   // s034
                                       Maximum, Minimum, SurfaceBlur,     // m4
                                       LensBlur,                          // s037
                                       RadialBlur,                        // s040
                                       Twirl, Pinch, Spherize,            // s040 m4: Distort
                                       PolarCoordinates, Ripple, Wave, ZigZag };   // s041 m3

// s034: what Smart Sharpen takes away -- the blur it undoes.
enum class Remove : std::uint8_t { Gaussian, Lens, Motion };

// s039: Lens Blur's DEPTH MAP source (Photoshop's): None (the same blur
// everywhere), the layer's Transparency (its alpha), or its Layer Mask (as
// painted: white 255, black 0).
// s041 m4: or a saved selection -- an ALPHA CHANNEL (`depth_channel`, its
// id; white 255) -- for a Lens Blur applied to the pixels (a live one sees
// only its layer: the blur is even).
enum class DepthSource : std::uint8_t { None, Transparency, LayerMask, Channel };

struct Filter {
    FilterKind kind = FilterKind::None;
    // Gaussian Blur, Unsharp Mask, High Pass: the radius in pixels, 0.1 .. 250
    // (the Gaussian's standard deviation, as Photoshop's Radius). Box Blur: 1
    // .. 250 whole pixels. Median: 1 .. 100. Motion Blur: the DISTANCE, 1 ..
    // 500 pixels (the length of the streak).
    double radius = 0;
    // Motion Blur: the streak's angle, -90 .. 90 degrees (0 = horizontal,
    // counter-clockwise up, as Photoshop's).
    int angle = 0;
    // Unsharp Mask: 1 .. 500 %. Add Noise: 0.1 .. 400 %.
    double amount = 0;
    // Unsharp Mask: 0 .. 255 levels -- a difference smaller than this is left.
    int threshold = 0;
    // Add Noise: Gaussian (else Uniform); Monochromatic (one offset for every
    // channel of a pixel); the seed (a new one each time the dialog opens, so
    // the preview and the result are the same noise).
    bool gaussian = false;
    bool mono = false;
    std::uint32_t seed = 1;
    // s034 -- Smart Sharpen: Remove (the blur undone; Motion takes `angle`),
    // Reduce Noise 0 .. 100 % (fine detail below the noise isn't sharpened),
    // and Fade Amount / Tonal Width 0 .. 100 % and Radius 1 .. 100 px for the
    // shadows and the highlights (less sharpening in the darks / lights).
    // `radius` 0.1 .. 64, `amount` 1 .. 500 %.
    Remove remove = Remove::Gaussian;
    double noise = 0;
    double shadow_fade = 0, shadow_tonal = 50, shadow_radius = 1;
    double light_fade = 0, light_tonal = 50, light_radius = 1;
    // s034 -- Reduce Noise: `amount` is the Strength 0 .. 10; Preserve
    // Details, Reduce Colour Noise, Sharpen Details 0 .. 100 %.
    // Dust & Scratches: `radius` 1 .. 100, `threshold` 0 .. 255.
    // m4 -- Surface Blur: `radius` 1 .. 100, `threshold` 2 .. 255 levels.
    double preserve = 0, colour = 0, sharpen = 0;
    // s034 m3 -- Maximum / Minimum: `radius` 1 .. 500; Preserve Roundness (a
    // disc) or, off, Squareness (a square -- the fast one).
    bool round = false;
    // s037 -- Lens Blur: `radius` 1 .. 100 px (the aperture's size); the
    // IRIS -- `blades` 3 .. 8 (its sides), `curvature` 0 .. 100 % (straight
    // sides .. a circle), `angle` 0 .. 360 degrees (its rotation); SPECULAR
    // HIGHLIGHTS -- `bright` 0 .. 100 %, `threshold` 0 .. 255 (what is lighter
    // than this is taken as a light, brightened before the blur, so it blooms
    // into the iris's shape -- bokeh).
    double blades = 6, curvature = 0, bright = 0;
    // s039 -- Lens Blur's DEPTH MAP: `depth` the source; `focal` 0 .. 255,
    // the Blur Focal Distance -- the depth that stays sharp, the blur growing
    // to the full radius at either end (focal 100: 1 and 255 both fully
    // blurred, Photoshop's example); `invert` turns the map over.
    // Its NOISE (s039 m2): `amount` 0 .. 100, `gaussian` / `mono` / `seed` as
    // Add Noise's -- grain put back in the blurred parts.
    DepthSource depth = DepthSource::None;
    std::string depth_channel;   // s041 m4: the alpha channel's id ("@channel/<n>"), Channel only
    double focal = 0;
    bool invert = false;
    // s040 -- Radial Blur: `amount` 1 .. 100; `zoom` (Zoom: streaks along the
    // rays from the centre; else Spin: arcs round it); `quality` 0 Draft, 1
    // Good, 2 Best (taps a pixel of streak: a third, 1, 2); the BLUR CENTRE
    // `cx`, `cy` in % of the image's width / height (50, 50: the middle).
    bool zoom = false;
    int quality = 1;
    double cx = 50, cy = 50;
    // s040 m4 -- DISTORT, in the ellipse fitted to the selection's bounds (or
    // the picture): Twirl -- `angle` -999 .. 999 degrees (the middle turned
    // most, the rim not at all; + clockwise); Pinch -- `amount` -100 .. 100
    // (+ squeezes the middle in, - pushes it out); Spherize -- `amount` -100
    // .. 100 (+ a bulge, - a dent) and `axis` 0 Normal, 1 Horizontal only, 2
    // Vertical only.
    int axis = 0;
    // s041 m3 -- more DISTORT. Polar Coordinates: `axis` 0 Rectangular to
    // Polar, 1 Polar to Rectangular (in the selection's bounds). Ripple:
    // `amount` -999 .. 999 %, `quality` the Size 0 Small, 1 Medium, 2 Large.
    // Wave (one generator): `radius` the Wavelength 2 .. 999 px, `amount` the
    // Amplitude 1 .. 999 px, `scale_h` / `scale_v` 0 .. 100 %, `axis` the Type
    // 0 Sine, 1 Triangle, 2 Square, `wrap` Wrap Around (else Repeat Edge).
    // ZigZag (in the ellipse): `amount` -100 .. 100, `ridges` 1 .. 20, `axis`
    // the Style 0 Around Center, 1 Out From Center, 2 Pond Ripples.
    double ridges = 5;
    double scale_h = 100, scale_v = 100;
    bool wrap = false;
    bool operator==(const Filter&) const = default;
};

// s018: a LIVE filter on a layer (Photoshop's smart filter): the filter, and
// whether it is on. A layer's stack runs bottom (first) to top (last) over
// its pixels whenever it is drawn; the pixels themselves never change.
struct LiveFilter {
    Filter f;
    bool   on = true;
    // s023: BLENDING OPTIONS (Photoshop's per smart filter): the filter's
    // result at `opacity` % in `mode` over what went into it.
    int    opacity = 100;
    pixel::Blend mode = pixel::Blend::Normal;
    bool operator==(const LiveFilter&) const = default;
};

namespace filter {
const std::vector<FilterKind>& kinds();       // the menu's order
std::string_view label(FilterKind k);         // "Gaussian Blur"
std::string_view key(FilterKind k);           // "gaussian-blur" (action names)
std::string_view group(FilterKind k);         // the submenu: "Blur", "Noise", "Sharpen", "Other"
std::string_view remove_key(Remove r);        // s034: "gaussian", "lens", "motion"
std::optional<Remove> remove_from(std::string_view key);
Filter fresh(FilterKind k);                   // Photoshop's defaults
Filter normalised(const Filter& f);           // clamped to the ranges above
bool identity(const Filter& f);               // changes nothing

// How far a pixel reads, in pixels (0 for Add Noise).
int reach(const Filter& f);

// s037: Lens Blur's iris -- for each row dy (-r .. r, r = ceil(radius)), the
// span of x it covers, [lo, hi] in real pixels (lo > hi: none). The polygon
// of `blades` sides on a circle of `radius`, rotated by `angle`, its sides
// bowed out toward the circle by `curvature` (0 .. 1). For the selftest too.
struct IrisRow { double lo, hi; };
std::vector<IrisRow> iris(double radius, int blades, double curvature, double angle_deg);

// s039: how much of the full radius a pixel of depth `d` (0 .. 1, as the map
// gives it -- before Invert) is blurred by: 0 at the focal distance, 1 at
// whichever end is farther from it on that side. For the selftest too.
double depth_amount(const Filter& f, double d);
std::string_view depth_key(DepthSource s);        // "none", "transparency", "layer-mask", s041: "channel"
std::optional<DepthSource> depth_from(std::string_view key);
// The Layer Mask source wants the layer's mask passed in (`depth` below);
// s041 m4: the Channel source the channel's saved selection.
bool wants_mask(const Filter& f);
bool wants_channel(const Filter& f);

// s040: Radial Blur's streak at a pixel `rho` px from the centre -- its
// length in pixels (Spin: the arc, amount x 0.5 degrees; Zoom: the ray's
// span, amount / 100 x 0.5 of rho); and the doubling passes the longest
// streak `rho` out needs (2^passes taps along it; 0 -- nothing to blur).
double radial_length(const Filter& f, double rho);
int radial_passes(const Filter& f, double rho);

// s040 m4: Distort's map -- where the output pixel at (x, y) (pixel centre,
// image coordinates) takes its colour from, for an ellipse of centre (ex, ey)
// and half-axes (rx, ry). False: outside it (unchanged).
bool warp_source(const Filter& f, double ex, double ey, double rx, double ry, double x, double y, double& sx, double& sy);
// s041: its result depends on where a pixel IS (not only on its neighbours) --
// Add Noise, Lens Blur's Noise, Radial Blur, Distort: a live look of it is
// always made whole, never in part.
bool placed(const Filter& f);

// The 1-D blur Gaussian Blur, Unsharp Mask and High Pass share, along a line of n samples with
// stride `stride` floats (edges clamp): sigma < 3 an exact Gaussian kernel,
// else three box passes of Kovesi's widths. For the selftest.
void blur_line(float* p, int n, int stride, double sigma);

// A pixel layer's pixels with `f` applied: straight colour, through the
// selection's coverage (nullptr: everywhere). Only the image's own area (the
// tiles beyond it, s010, are kept as they were); `only` (when given) limits
// the work further -- the preview asks for just what the view shows.
// `keep_alpha`: the layer's transparency is locked -- a blur leaves alpha
// (Unsharp Mask, High Pass and Add Noise never change it). RGB and Gray maps;
// nullopt for CMYK / Lab, or when cancelled. `progress` gets 0..1.
std::optional<pixel::TileMap> filtered(const pixel::TileMap& src, const Filter& f, const pixel::TileMap* sel,
                                       bool keep_alpha = false, const pixel::Rect* only = nullptr,
                                       const std::function<bool()>& cancelled = {},
                                       const std::function<void(double)>& progress = {},
                                       const pixel::TileMap* depth = nullptr);
// s039: `depth` -- Lens Blur's Layer Mask source: the layer's mask (stored as
// what it hides, as Document's). nullptr with that source: the blur is even
// (as None). Transparency reads `src`'s own alpha. s041 m4: the Channel
// source -- the channel's saved selection (stored as what is selected).
}  // namespace filter

}  // namespace klimt::core
