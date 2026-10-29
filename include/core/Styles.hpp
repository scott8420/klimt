#pragma once
// core::Styles -- LAYER STYLES (s019): Photoshop's layer effects, live under a
// layer like its filters. docs/styles.md.
//
// Each effect is built from the layer's ALPHA (as it looks: after its live
// filters) and drawn with the layer -- Drop Shadow and Outer Glow under the
// content, the content (at the layer's FILL), then Inner Shadow and Color
// Overlay on it, Stroke on top. The layer's opacity and blend mode then act
// on the whole; FILL only on the content (Photoshop's distinction -- a fill of
// 0 leaves just the effects). Effects blend Normal within the layer.
#include <cstdint>
#include "pixel/Blend.hpp"   // s025: each effect's blend mode
#include "pixel/Pixel.hpp"   // s033: a user pattern's pixels

#include <memory>
#include <string>

namespace klimt::core {

struct Rgb {
    float r = 0, g = 0, b = 0;   // sRGB 0..1
    bool operator==(const Rgb&) const = default;
};

// Drop Shadow / Inner Shadow: the alpha moved AWAY from the light (angle,
// degrees, 120 = light from the upper left) by `distance`, grown by `spread`
// (%) and softened over `size` px.
struct ShadowFx {
    bool on = false;
    Rgb  colour{0, 0, 0};
    int  opacity = 75;     // %
    int  angle = 120;      // degrees
    int  distance = 5;     // px, 0..1000
    int  spread = 0;       // %, Inner Shadow: choke
    int  size = 5;         // px, 0..250
    pixel::Blend mode = pixel::Blend::Multiply;   // s025 (Photoshop's default; a file without it: Normal)
    bool operator==(const ShadowFx&) const = default;
};

// Outer Glow: the alpha grown by `spread` and softened over `size`, outside.
struct GlowFx {
    bool on = false;
    Rgb  colour{1.f, 1.f, 190.f / 255.f};
    int  opacity = 75;
    int  spread = 0;
    int  size = 5;
    pixel::Blend mode = pixel::Blend::Screen;     // s025
    bool operator==(const GlowFx&) const = default;
};

// Stroke: a band `size` px wide outside (0), inside (1) or centred on (2)
// the layer's edge.
struct StrokeFx {
    bool on = false;
    Rgb  colour{0, 0, 0};
    int  opacity = 100;
    int  size = 3;         // px, 1..250
    int  position = 0;     // 0 outside, 1 inside, 2 centre
    pixel::Blend mode = pixel::Blend::Normal;     // s025
    bool operator==(const StrokeFx&) const = default;
};

// Color Overlay: the content's colour replaced (its alpha kept).
struct OverlayFx {
    bool on = false;
    Rgb  colour{1, 0, 0};
    int  opacity = 100;
    pixel::Blend mode = pixel::Blend::Normal;     // s025
    bool operator==(const OverlayFx&) const = default;
};

// s023 -- Inner Glow: light inside the edge (Edge) softened over `size`, or
// from the middle out (Center); `choke` (%) firms it up.
struct InnerGlowFx {
    bool on = false;
    Rgb  colour{1.f, 1.f, 190.f / 255.f};
    int  opacity = 75;
    int  choke = 0;
    int  size = 5;
    bool center = false;
    pixel::Blend mode = pixel::Blend::Screen;     // s025
    bool operator==(const InnerGlowFx&) const = default;
};

// s025 -- Gradient Overlay: the content's colour from `from` to `to` across
// its own box -- Linear along `angle` (90: bottom to top), or Radial from
// its middle; `scale` (%) stretches it; Reverse swaps the ends.
struct GradientFx {
    bool on = false;
    Rgb  from{0, 0, 0};
    Rgb  to{1, 1, 1};
    int  opacity = 100;
    int  angle = 90;
    int  style = 0;        // 0 Linear, 1 Radial
    int  scale = 100;      // %, 10..150
    bool reverse = false;
    pixel::Blend mode = pixel::Blend::Normal;
    bool operator==(const GradientFx&) const = default;
};

// s028 m4 -- Pattern Overlay: a built-in pattern (core::pattern, by index)
// over the content, anchored to the image, at `scale` (%) and moved by
// (x, y) px; under Gradient and Color Overlay (Photoshop's order).
struct PatternFx {
    bool on = false;
    int  pattern = 0;      // core::pattern::builtin() index (when `image` is null)
    // s033: YOUR pattern (Edit > Define Pattern): its name and its pixels
    // (straight RGB + alpha, 8-bit), carried in the style itself -- so the
    // .klimt keeps it -- and compared by pointer.
    std::string user;
    std::shared_ptr<const pixel::TileMap> image;
    int  opacity = 100;
    int  scale = 100;      // %, 10..1000
    int  x = 0, y = 0;     // px, -1000..1000
    pixel::Blend mode = pixel::Blend::Normal;
    bool operator==(const PatternFx&) const = default;
};

// s026 -- Satin: the shape against itself, shifted `distance` px both ways
// along `angle`; where the two copies differ, softened over `size` -- the
// silky folds of a satin cloth -- over the content. Invert (Photoshop's
// default): the shading where they agree instead.
struct SatinFx {
    bool on = false;
    Rgb  colour{0, 0, 0};
    int  opacity = 50;
    int  angle = 19;
    int  distance = 11;    // px, 1..250
    int  size = 14;        // px, 0..250
    bool invert = true;
    pixel::Blend mode = pixel::Blend::Multiply;
    bool operator==(const SatinFx&) const = default;
};

// s021 -- Bevel & Emboss (Smooth): the alpha as a height field, lit from
// `angle` at `altitude` -- a highlight where the slope faces the light, a
// shadow where it faces away. Inner Bevel: inside the content's edge;
// Emboss: either side of it. `depth` (%) steepens the slopes; `up` false
// turns them over (pressed in); `soften` blurs the shading.
struct BevelFx {
    bool on = false;
    int  style = 0;        // 0 Inner Bevel, 1 Emboss
    int  depth = 100;      // %, 1..1000
    bool up = true;
    int  size = 5;         // px, 0..250
    int  soften = 0;       // px, 0..16
    int  angle = 120;      // degrees
    int  altitude = 30;    // degrees, 0..90
    Rgb  highlight{1, 1, 1};
    int  highlight_opacity = 75;
    Rgb  shadow{0, 0, 0};
    int  shadow_opacity = 75;
    pixel::Blend highlight_mode = pixel::Blend::Screen;   // s025
    pixel::Blend shadow_mode = pixel::Blend::Multiply;
    bool operator==(const BevelFx&) const = default;
};

struct Styles {
    ShadowFx  drop;
    GlowFx    outer;
    ShadowFx  inner{false, {0, 0, 0}, 75, 120, 5, 0, 5};
    OverlayFx overlay;
    StrokeFx  stroke;
    BevelFx   bevel;   // s021
    InnerGlowFx inner_glow;   // s023
    GradientFx gradient;      // s025
    SatinFx    satin;         // s026
    PatternFx  pattern;       // s028 m4
    bool any() const { return drop.on || outer.on || inner.on || overlay.on || stroke.on || bevel.on || inner_glow.on || gradient.on || satin.on || pattern.on; }
    bool operator==(const Styles&) const = default;
};

}  // namespace klimt::core
