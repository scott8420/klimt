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
#include <string_view>
#include <vector>

namespace klimt::core {

enum class FilterKind : std::uint8_t { None, GaussianBlur, UnsharpMask, HighPass, AddNoise, BoxBlur, MotionBlur, Median };

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
Filter fresh(FilterKind k);                   // Photoshop's defaults
Filter normalised(const Filter& f);           // clamped to the ranges above
bool identity(const Filter& f);               // changes nothing

// How far a pixel reads, in pixels (0 for Add Noise).
int reach(const Filter& f);

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
                                       const std::function<void(double)>& progress = {});
}  // namespace filter

}  // namespace klimt::core
