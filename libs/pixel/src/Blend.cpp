#include "pixel/Blend.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace klimt::pixel {

namespace {
float clamp01(float v) { return std::clamp(v, 0.f, 1.f); }

// ── separable B(cb, cs), straight colours ───────────────────────────────────
float sep(Blend m, float b, float s) {
    switch (m) {
        case Blend::Multiply:    return b * s;
        case Blend::Screen:      return b + s - b * s;
        case Blend::Overlay:     return b <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
        case Blend::Darken:      return std::min(b, s);
        case Blend::Lighten:     return std::max(b, s);
        case Blend::ColorDodge:  return b <= 0 ? 0.f : (s >= 1 ? 1.f : std::min(1.f, b / (1 - s)));
        case Blend::ColorBurn:   return b >= 1 ? 1.f : (s <= 0 ? 0.f : 1 - std::min(1.f, (1 - b) / s));
        case Blend::HardLight:   return s <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
        case Blend::SoftLight: {
            if (s <= 0.5f) return b - (1 - 2 * s) * b * (1 - b);
            const float d = b <= 0.25f ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
            return b + (2 * s - 1) * (d - b);
        }
        case Blend::Difference:  return std::abs(b - s);
        case Blend::Exclusion:   return b + s - 2 * b * s;
        case Blend::LinearBurn:  return clamp01(b + s - 1);
        case Blend::LinearDodge: return clamp01(b + s);
        case Blend::Subtract:    return clamp01(b - s);
        case Blend::Divide:      return s <= 0 ? (b <= 0 ? 0.f : 1.f) : clamp01(b / s);
        case Blend::VividLight:
            return s <= 0.5f ? (s <= 0 ? 0.f : clamp01(1 - (1 - b) / (2 * s)))
                             : (s >= 1 ? 1.f : clamp01(b / (2 * (1 - s))));
        case Blend::LinearLight: return clamp01(b + 2 * s - 1);
        case Blend::PinLight:    return s <= 0.5f ? std::min(b, 2 * s) : std::max(b, 2 * s - 1);
        case Blend::HardMix:     return b + s >= 1 ? 1.f : 0.f;
        default:                 return s;   // Normal, Dissolve
    }
}

// ── non-separable helpers (W3C) ─────────────────────────────────────────────
struct C3 { float r, g, b; };
float lum(C3 c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }
C3 clip(C3 c) {
    const float l = lum(c);
    const float n = std::min({c.r, c.g, c.b}), x = std::max({c.r, c.g, c.b});
    if (n < 0) c = {l + (c.r - l) * l / (l - n), l + (c.g - l) * l / (l - n), l + (c.b - l) * l / (l - n)};
    if (x > 1) c = {l + (c.r - l) * (1 - l) / (x - l), l + (c.g - l) * (1 - l) / (x - l), l + (c.b - l) * (1 - l) / (x - l)};
    return c;
}
C3 set_lum(C3 c, float l) {
    const float d = l - lum(c);
    return clip({c.r + d, c.g + d, c.b + d});
}
float sat(C3 c) { return std::max({c.r, c.g, c.b}) - std::min({c.r, c.g, c.b}); }
C3 set_sat(C3 c, float s) {
    float* v[3] = {&c.r, &c.g, &c.b};
    std::sort(v, v + 3, [](float* a, float* b) { return *a < *b; });
    float& mn = *v[0]; float& md = *v[1]; float& mx = *v[2];
    if (mx > mn) { md = (md - mn) * s / (mx - mn); mx = s; }
    else { md = 0; mx = 0; }
    mn = 0;
    return c;
}

// The pattern Dissolve thresholds against: a cheap, fixed hash of the pixel.
float dither(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffffu) / 65535.f;
}
}  // namespace

void blend_span(Blend mode, const float* src, float* dst, int n, float opacity, int x0, int y) {
    opacity = clamp01(opacity);
    if (opacity <= 0) return;
    for (int i = 0; i < n; ++i) {
        const float* s = src + 4 * i;
        float* d = dst + 4 * i;
        float as = s[3] * opacity;
        if (mode == Blend::Dissolve) as = (as > 0 && dither(x0 + i, y) < as) ? 1.f : 0.f;
        if (as <= 0) continue;
        const float ab = d[3];
        // Straight colours (from premultiplied).
        const C3 cs = s[3] > 0 ? C3{s[0] / s[3], s[1] / s[3], s[2] / s[3]} : C3{0, 0, 0};
        const C3 cb = ab > 0 ? C3{d[0] / ab, d[1] / ab, d[2] / ab} : C3{0, 0, 0};
        C3 bl;
        switch (mode) {
            case Blend::Hue:          bl = set_lum(set_sat(cs, sat(cb)), lum(cb)); break;
            case Blend::Saturation:   bl = set_lum(set_sat(cb, sat(cs)), lum(cb)); break;
            case Blend::Color:        bl = set_lum(cs, lum(cb)); break;
            case Blend::Luminosity:   bl = set_lum(cb, lum(cs)); break;
            case Blend::DarkerColor:  bl = lum(cs) < lum(cb) ? cs : cb; break;
            case Blend::LighterColor: bl = lum(cs) > lum(cb) ? cs : cb; break;
            default:
                bl = {sep(mode, cb.r, cs.r), sep(mode, cb.g, cs.g), sep(mode, cb.b, cs.b)};
        }
        const float sb[3] = {cs.r, cs.g, cs.b}, bb[3] = {cb.r, cb.g, cb.b}, lb[3] = {bl.r, bl.g, bl.b};
        for (int c = 0; c < 3; ++c)
            d[c] = sb[c] * as * (1 - ab) + bb[c] * ab * (1 - as) + as * ab * lb[c];
        d[3] = as + ab * (1 - as);
    }
}

}  // namespace klimt::pixel
