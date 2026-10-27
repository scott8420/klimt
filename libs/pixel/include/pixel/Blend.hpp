#pragma once
// pixel::Blend -- the blend modes (s006: moved here from core; core::Blend is
// an alias). The full Photoshop set plus Pass Through (groups only). The math
// lives in the compositor; the menu's labels in core's blend_modes().
#include <cstdint>

namespace klimt::pixel {

enum class Blend : std::uint8_t {
    PassThrough,                                         // groups only
    Normal, Dissolve,
    Darken, Multiply, ColorBurn, LinearBurn, DarkerColor,
    Lighten, Screen, ColorDodge, LinearDodge, LighterColor,
    Overlay, SoftLight, HardLight, VividLight, LinearLight, PinLight, HardMix,
    Difference, Exclusion, Subtract, Divide,
    Hue, Saturation, Color, Luminosity,
};

}  // namespace klimt::pixel

namespace klimt::pixel {

// Blend `n` pixels of `src` onto `dst`, both PREMULTIPLIED float RGBA (4
// floats a pixel), at `opacity` 0..1 -- the W3C compositing model with
// Photoshop's modes: co = cs(1-ab) + cb(1-as) + as*ab*B(Cb, Cs). Separable
// modes per channel; Hue / Saturation / Color / Luminosity and Darker /
// Lighter Color on the whole colour; Dissolve keeps or drops each pixel by a
// fixed pattern seeded from its position (x0 + i, y). RGB only for now: the
// other modes' compositing comes with colour management (libs/tint).
void blend_span(Blend mode, const float* src, float* dst, int n, float opacity, int x0, int y);

}  // namespace klimt::pixel
