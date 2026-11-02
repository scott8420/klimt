// core -- ADJUSTMENTS (s013). docs/adjustments.md.
#include "core/Adjust.hpp"
#include "core/Selection.hpp"

#include <algorithm>
#include <cmath>

namespace klimt::core::adjust {

namespace {
constexpr int kLut = 1024;   // entries per channel; read with linear interpolation

struct Info { AdjustKind k; const char* key; const char* label; };
constexpr Info kInfo[] = {
    {AdjustKind::Levels,    "levels",              "Levels"},
    {AdjustKind::Curves,    "curves",              "Curves"},
    {AdjustKind::HueSat,    "hue-saturation",      "Hue/Saturation"},
    {AdjustKind::BrightCon, "brightness-contrast", "Brightness/Contrast"},
    {AdjustKind::Exposure,  "exposure",            "Exposure"},
    {AdjustKind::Invert,    "invert",              "Invert"},
    {AdjustKind::Posterize, "posterize",           "Posterize"},
    {AdjustKind::Threshold, "threshold",           "Threshold"},
    {AdjustKind::Vibrance,  "vibrance",            "Vibrance"},
    {AdjustKind::BlackWhite,"black-white",         "Black & White"},
    {AdjustKind::PhotoFilter,"photo-filter",       "Photo Filter"},
    {AdjustKind::ColourBalance,"colour-balance",   "Colour Balance"},   // s063
    {AdjustKind::ChannelMixer,"channel-mixer",     "Channel Mixer"},
    {AdjustKind::SelectiveColour,"selective-colour","Selective Colour"},
    {AdjustKind::GradientMap,"gradient-map",       "Gradient Map"},
    {AdjustKind::Duotone,   "duotone",             "Duotone"},   // s065
    {AdjustKind::ColourLookup,"colour-lookup",     "Colour Lookup"},   // s066
};

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

float lut_at(const std::vector<float>& lut, float v) {
    const float x = std::clamp(v, 0.f, 1.f) * (kLut - 1);
    const int i = std::min(static_cast<int>(x), kLut - 2);
    const float f = x - static_cast<float>(i);
    return lut[static_cast<std::size_t>(i)] + (lut[static_cast<std::size_t>(i) + 1] - lut[static_cast<std::size_t>(i)]) * f;
}

// Brightness / Contrast, Photoshop's modern way: brightness bends the middle
// (black and white stay), contrast pulls away from / toward mid grey.
double bright_con(int brightness, int contrast, double v) {
    v = std::pow(clamp01(v), std::pow(2.0, -brightness / 75.0));
    const double k = contrast >= 0 ? 1.0 / (1.0 - contrast / 200.0) : 1.0 + contrast / 100.0;
    return clamp01(0.5 + (v - 0.5) * k);
}

// s061: Exposure, in linear light (Photoshop: "calculations in a linear
// colour space, gamma 1.0") -- the sRGB curve off, then on again.
double srgb_to_linear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double linear_to_srgb(double v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }
double exposure(const Adjust& a, double v) {
    double l = srgb_to_linear(clamp01(v)) * std::pow(2.0, a.exposure) + a.offset;
    l = std::pow(std::max(0.0, l), 1.0 / std::max(0.01, a.exp_gamma));
    return linear_to_srgb(clamp01(l));
}
// s061: Posterize -- n even steps, black and white kept.
double posterize(int n, double v) {
    n = std::clamp(n, 2, 255);
    const int i = std::min(n - 1, static_cast<int>(clamp01(v) * n));
    return static_cast<double>(i) / (n - 1);
}

double luma(double r, double g, double b) { return 0.299 * r + 0.587 * g + 0.114 * b; }

// RGB <-> HSL, all 0..1 (hue in turns).
void hsl_of(double r, double g, double b, double& h, double& s, double& l) {
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2;
    if (mx - mn < 1e-12) { h = 0; s = 0; return; }
    const double d = mx - mn;
    s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r)      h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) h = (b - r) / d + 2;
    else              h = (r - g) / d + 4;
    h /= 6;
}
double hue_part(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}
void rgb_of(double h, double s, double l, double& r, double& g, double& b) {
    if (s <= 0) { r = g = b = l; return; }
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hue_part(p, q, h + 1.0 / 3);
    g = hue_part(p, q, h);
    b = hue_part(p, q, h - 1.0 / 3);
}

void hue_sat(const Adjust& a, double& r, double& g, double& b) {
    // s064: the master plus each colour range by how much of it this hue is
    // (none on greys: the weight fades in over the first 8 % of chroma).
    int H = a.hue, S = a.saturation, L = a.lightness;
    bool any_range = false;
    for (const auto& rg : a.ranges) any_range = any_range || rg.hue || rg.saturation || rg.lightness;
    if (!a.colorize && any_range) {
        double h, s, l;
        hsl_of(r, g, b, h, s, l);
        const double chroma = std::max({r, g, b}) - std::min({r, g, b});
        const double fade = std::clamp(chroma / 0.08, 0.0, 1.0);
        double dh = 0, ds = 0, dl = 0;
        for (const auto& rg : a.ranges) {
            if (!rg.hue && !rg.saturation && !rg.lightness) continue;
            const double w = range_weight(rg.band, h * 360) * fade;
            dh += w * rg.hue; ds += w * rg.saturation; dl += w * rg.lightness;
        }
        const double hh = H + dh, ss = std::clamp(S + ds, -100.0, 100.0), ll = std::clamp(L + dl, -100.0, 100.0);
        // The same maths as the master's, with fractional amounts.
        hsl_of(r, g, b, h, s, l);
        if (hh != 0 || ss != 0) {
            h = std::fmod(h + hh / 360.0 + 2.0, 1.0);
            const double k = ss / 100.0;
            s = k <= 0 ? s * (1 + k) : clamp01(s + (1 - s) * s * k * 2);
            rgb_of(h, s, l, r, g, b);
        }
        if (ll != 0) {
            const double k = ll / 100.0;
            const auto f = [k](double v) { return k < 0 ? v * (1 + k) : v + (1 - v) * k; };
            r = f(r); g = f(g); b = f(b);
        }
        return;
    }
    if (a.colorize) {
        // The colour's lightness kept, hue and saturation set.
        const double lum = 0.299 * r + 0.587 * g + 0.114 * b;
        double h = std::fmod(a.hue + 360.0, 360.0) / 360.0;
        rgb_of(h, std::clamp(a.saturation, 0, 100) / 100.0, lum, r, g, b);
    } else if (a.hue != 0 || a.saturation != 0) {
        double h, s, l;
        hsl_of(r, g, b, h, s, l);
        h = std::fmod(h + a.hue / 360.0 + 1.0, 1.0);
        const double k = a.saturation / 100.0;
        s = k <= 0 ? s * (1 + k) : clamp01(s + (1 - s) * s * k * 2);   // more: the grey stays grey
        rgb_of(h, s, l, r, g, b);
    }
    if (a.lightness != 0) {
        const double k = a.lightness / 100.0;
        const auto f = [k](double v) { return k < 0 ? v * (1 + k) : v + (1 - v) * k; };
        r = f(r); g = f(g); b = f(b);
    }
}

// s062: Vibrance -- each colour pushed from (or toward) its own grey: by
// vibrance x how dull it is (max - min small), then by saturation evenly.
void vibrance(const Adjust& a, double& r, double& g, double& b) {
    const auto push = [&](double k) {
        const double l = luma(r, g, b);
        r = clamp01(l + (r - l) * (1 + k)); g = clamp01(l + (g - l) * (1 + k)); b = clamp01(l + (b - l) * (1 + k));
    };
    if (a.vibrance != 0) push(a.vibrance / 100.0 * (1 - (std::max({r, g, b}) - std::min({r, g, b}))));
    if (a.saturation != 0) push(a.saturation / 100.0);
}

// s062: Black & White -- the grey from the hue's two weights, as Photoshop's
// (the darktable / GIMP reading): the largest channel picks a primary (red,
// green, blue), the two largest a secondary (yellow, cyan, magenta);
// grey = min + (mid - min) x secondary + (max - mid) x primary.
double black_white(const Adjust& a, double r, double g, double b) {
    const double w[6] = {a.bw[0] / 100.0, a.bw[1] / 100.0, a.bw[2] / 100.0, a.bw[3] / 100.0, a.bw[4] / 100.0, a.bw[5] / 100.0};
    enum { Re, Ye, Gr, Cy, Bl, Ma };
    double mx, md, mn, wp, ws;
    if (r >= g && r >= b) { mx = r; wp = w[Re]; if (g >= b) { md = g; mn = b; ws = w[Ye]; } else { md = b; mn = g; ws = w[Ma]; } }
    else if (g >= b)      { mx = g; wp = w[Gr]; if (r >= b) { md = r; mn = b; ws = w[Ye]; } else { md = b; mn = r; ws = w[Cy]; } }
    else                  { mx = b; wp = w[Bl]; if (r >= g) { md = r; mn = g; ws = w[Ma]; } else { md = g; mn = r; ws = w[Cy]; } }
    return clamp01(mn + (md - mn) * ws + (mx - md) * wp);
}

// s062: Photo Filter -- the colour seen through the filter (multiplied),
// mixed in by density; Preserve Luminosity scales it back to its own luma.
void photo_filter(const Adjust& a, double& r, double& g, double& b) {
    const double d = std::clamp(a.density, 1, 100) / 100.0, l0 = luma(r, g, b);
    double R = r + (r * a.filter[0] - r) * d, G = g + (g * a.filter[1] - g) * d, B = b + (b * a.filter[2] - b) * d;
    if (a.keep_luma) {
        const double l1 = luma(R, G, B);
        if (l1 > 1e-6) { const double k = l0 / l1; R *= k; G *= k; B *= k; }
        else R = G = B = l0;
        // Over white: what can't be scaled up is added evenly.
        const double over = l0 - luma(clamp01(R), clamp01(G), clamp01(B));
        if (over > 1e-6) { R += over; G += over; B += over; }
    }
    r = clamp01(R); g = clamp01(G); b = clamp01(B);
}

// s063: Colour Balance, GIMP's reading of Photoshop's: each tone's shift
// weighted by how far the pixel's lightness lies in that tone (shadows fade
// out by a third, highlights in from two thirds, midtones between), scaled
// by 0.7; Preserve Luminosity puts the HSL lightness back.
double balance_map(double v, double l, double sh, double mid, double hi) {
    constexpr double a = 0.25, b = 0.333, scale = 0.7;
    sh  *= std::clamp((l - b) / -a + 0.5, 0.0, 1.0) * scale;
    mid *= std::clamp((l - b) / a + 0.5, 0.0, 1.0) * std::clamp((l + b - 1) / -a + 0.5, 0.0, 1.0) * scale;
    hi  *= std::clamp((l + b - 1) / a + 0.5, 0.0, 1.0) * scale;
    return clamp01(v + sh + mid + hi);
}
void colour_balance(const Adjust& a, double& r, double& g, double& b) {
    double h, s, l;
    hsl_of(r, g, b, h, s, l);
    double* ch[3] = {&r, &g, &b};
    for (std::size_t c = 0; c < 3; ++c)
        *ch[c] = balance_map(*ch[c], l, a.balance[0][c] / 100.0, a.balance[1][c] / 100.0, a.balance[2][c] / 100.0);
    if (a.keep_luma) {
        double h2, s2, l2;
        hsl_of(r, g, b, h2, s2, l2);
        rgb_of(h2, s2, l, r, g, b);
    }
}

// s063: Channel Mixer -- each output a sum of the inputs plus the constant.
void channel_mixer(const Adjust& a, double& r, double& g, double& b) {
    const auto mix = [&](const std::array<int, 4>& m) { return clamp01((m[0] * r + m[1] * g + m[2] * b + m[3]) / 100.0); };
    if (a.monochrome) { r = g = b = mix(a.mono); return; }
    const double R = mix(a.mixer[0]), G = mix(a.mixer[1]), B = mix(a.mixer[2]);
    r = R; g = G; b = B;
}

// s063: Selective Colour. How much of each of the nine colours a pixel is:
// a primary (red, green, blue) by how far its largest channel leads the
// middle one, a secondary (yellow, cyan, magenta) by how far the middle one
// leads the smallest; whites by the smallest past half, blacks by the
// largest under half, neutrals the rest of the way to grey. Each colour's
// inks then move the pixel by that much (the shader-world reading of
// Photoshop's: ink c on a channel v moves it by ((-1 - c) k - c), times
// the ink already there (1 - v) when Relative).
void selective_colour(const Adjust& a, double& r, double& g, double& b) {
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const double md = r + g + b - mx - mn;
    double w[9] = {};
    enum { Re, Ye, Gr, Cy, Bl, Ma, Wh, Ne, Bk };
    if (mx > mn) {
        w[r == mx ? Re : g == mx ? Gr : Bl] = mx - md;
        w[b == mn ? Ye : r == mn ? Cy : Ma] = md - mn;
    }
    w[Wh] = mn > 0.5 ? (mn - 0.5) * 2 : 0;
    w[Bk] = mx < 0.5 ? (0.5 - mx) * 2 : 0;
    w[Ne] = std::max(0.0, 1 - (std::abs(mx - 0.5) + std::abs(mn - 0.5)));
    const double in[3] = {r, g, b};
    double out[3] = {r, g, b};
    for (int k = 0; k < 9; ++k) {
        if (w[k] <= 0) continue;
        const auto& s = a.selective[static_cast<std::size_t>(k)];
        const double blk = s[3] / 100.0;
        for (int c = 0; c < 3; ++c) {
            const double ink = s[static_cast<std::size_t>(c)] / 100.0, v = in[c];
            double d = (-1 - ink) * blk - ink;
            if (!a.selective_absolute) d *= 1 - v;
            out[c] += std::clamp(d, -v, 1 - v) * w[k];
        }
    }
    r = clamp01(out[0]); g = clamp01(out[1]); b = clamp01(out[2]);
}

// Channel c (0..2 = R, G, B) of the LUT kinds, on v.
double lut_kind(const Adjust& a, const std::vector<double>* tables, int c, double v) {
    switch (a.kind) {
        case AdjustKind::Levels: return levels_map(a.levels[0], levels_map(a.levels[static_cast<std::size_t>(c) + 1], v));
        case AdjustKind::Curves: {
            const auto at = [&](const std::vector<double>& t, double x) {
                const double p = clamp01(x) * static_cast<double>(t.size() - 1);
                const auto i = std::min(static_cast<std::size_t>(p), t.size() - 2);
                return t[i] + (t[i + 1] - t[i]) * (p - static_cast<double>(i));
            };
            return at(tables[0], at(tables[c + 1], v));
        }
        case AdjustKind::BrightCon: return bright_con(a.brightness, a.contrast, v);
        case AdjustKind::Invert:    return 1 - v;
        case AdjustKind::Posterize: return posterize(a.posterize, v);
        case AdjustKind::Exposure:  return exposure(a, v);
        case AdjustKind::GradientMap: return gradient_at(a.grad, a.grad_reverse ? 1 - v : v)[static_cast<std::size_t>(c)];   // s063: v is the luminosity
        default: return v;
    }
}
}  // namespace

void to_hsl(double r, double g, double b, double& h, double& s, double& l) { hsl_of(r, g, b, h, s, l); }

double range_weight(const std::array<int, 4>& bd, double hue) {
    const auto off = [&](double v) { return std::fmod(std::fmod(v - bd[0], 360.0) + 360.0, 360.0); };
    const double o1 = off(bd[1]), o2 = off(bd[2]), o3 = off(bd[3]), x = off(hue);
    if (x < o1) return o1 > 0 ? x / o1 : 1;
    if (x <= o2) return 1;
    if (x < o3) return (o3 - x) / std::max(1e-9, o3 - o2);
    return 0;
}

std::array<double, 3> gradient_at(const std::vector<GradStop>& stops, double t) {
    if (stops.empty()) return {t, t, t};
    t = clamp01(t);
    if (t <= stops.front().at) return stops.front().rgb;
    if (t >= stops.back().at) return stops.back().rgb;
    std::size_t hi = 1;
    while (hi + 1 < stops.size() && stops[hi].at < t) ++hi;
    const auto& p = stops[hi - 1];
    const auto& q = stops[hi];
    const double span = q.at - p.at;
    if (span <= 1e-9) return q.rgb;
    double u = (t - p.at) / span;
    const double m = std::clamp(q.mid, 0.05, 0.95);
    u = u < m ? 0.5 * u / m : 0.5 + 0.5 * (u - m) / (1 - m);
    return {p.rgb[0] + (q.rgb[0] - p.rgb[0]) * u, p.rgb[1] + (q.rgb[1] - p.rgb[1]) * u, p.rgb[2] + (q.rgb[2] - p.rgb[2]) * u};
}
void from_hsl(double h, double s, double l, double& r, double& g, double& b) { rgb_of(h, s, l, r, g, b); }

std::string_view label(AdjustKind k) {
    for (const auto& i : kInfo) if (i.k == k) return i.label;
    return "Adjustment";
}
std::string_view key(AdjustKind k) {
    for (const auto& i : kInfo) if (i.k == k) return i.key;
    return "none";
}
std::optional<AdjustKind> from_key(std::string_view k) {
    for (const auto& i : kInfo) if (k == i.key) return i.k;
    return std::nullopt;
}
const std::vector<AdjustKind>& kinds() {
    static const std::vector<AdjustKind> v{AdjustKind::BrightCon, AdjustKind::Levels, AdjustKind::Curves, AdjustKind::Exposure,
                                           AdjustKind::Vibrance, AdjustKind::HueSat, AdjustKind::ColourBalance, AdjustKind::BlackWhite,
                                           AdjustKind::PhotoFilter, AdjustKind::ChannelMixer, AdjustKind::ColourLookup,
                                           AdjustKind::Invert, AdjustKind::Posterize, AdjustKind::Threshold,
                                           AdjustKind::GradientMap, AdjustKind::SelectiveColour, AdjustKind::Duotone};
    return v;
}

Adjust fresh(AdjustKind k) {
    Adjust a;
    a.kind = k;
    if (k == AdjustKind::Curves)
        for (auto& c : a.curves) c = {{0, 0}, {1, 1}};
    return a;
}

Adjust normalised(Adjust a) {
    for (auto& c : a.levels) {
        c.in_black = clamp01(c.in_black);
        c.in_white = std::clamp(c.in_white, std::min(1.0, c.in_black + 2.0 / 255), 1.0);
        c.in_black = std::min(c.in_black, c.in_white - 2.0 / 255);
        c.gamma = std::clamp(c.gamma, 0.1, 9.99);
        c.out_black = clamp01(c.out_black);
        c.out_white = clamp01(c.out_white);
    }
    for (auto& c : a.curves) {
        for (auto& p : c) { p.x = clamp01(p.x); p.y = clamp01(p.y); }
        std::stable_sort(c.begin(), c.end(), [](const CurvePt& p, const CurvePt& q) { return p.x < q.x; });
        // Two points closer than a level apart in x: the later one wins.
        std::vector<CurvePt> kept;
        for (const auto& p : c) {
            if (!kept.empty() && p.x - kept.back().x < 1.0 / 255) kept.back() = p;
            else kept.push_back(p);
        }
        c = std::move(kept);
    }
    a.hue = a.colorize ? ((a.hue % 360) + 360) % 360 : std::clamp(a.hue, -180, 180);
    a.saturation = std::clamp(a.saturation, a.colorize ? 0 : -100, 100);
    a.lightness = std::clamp(a.lightness, -100, 100);
    for (auto& rg : a.ranges) {   // s064: degrees 0..359, in order round the wheel from the first
        for (auto& v : rg.band) v = ((v % 360) + 360) % 360;
        int o[4] = {0, (rg.band[1] - rg.band[0] + 360) % 360, (rg.band[2] - rg.band[0] + 360) % 360, (rg.band[3] - rg.band[0] + 360) % 360};
        o[2] = std::max(o[2], o[1]);
        o[3] = std::max(o[3], o[2]);
        for (int i = 1; i < 4; ++i) rg.band[static_cast<std::size_t>(i)] = (rg.band[0] + o[i]) % 360;
        rg.hue = std::clamp(rg.hue, -180, 180);
        rg.saturation = std::clamp(rg.saturation, -100, 100);
        rg.lightness = std::clamp(rg.lightness, -100, 100);
    }
    a.brightness = std::clamp(a.brightness, -150, 150);
    a.contrast = std::clamp(a.contrast, -50, 100);
    a.posterize = std::clamp(a.posterize, 2, 255);
    a.threshold = std::clamp(a.threshold, 1, 255);
    a.exposure = std::clamp(a.exposure, -20.0, 20.0);
    a.offset = std::clamp(a.offset, -0.5, 0.5);
    a.exp_gamma = std::clamp(a.exp_gamma, 0.01, 9.99);
    a.vibrance = std::clamp(a.vibrance, -100, 100);
    for (auto& w : a.bw) w = std::clamp(w, -200, 300);
    a.tint_hue = ((a.tint_hue % 360) + 360) % 360;
    a.tint_sat = std::clamp(a.tint_sat, 0, 100);
    for (auto& c : a.filter) c = clamp01(c);
    a.density = std::clamp(a.density, 1, 100);
    for (auto& t : a.balance) for (auto& v : t) v = std::clamp(v, -100, 100);   // s063
    for (auto& o : a.mixer) for (auto& v : o) v = std::clamp(v, -200, 200);
    for (auto& v : a.mono) v = std::clamp(v, -200, 200);
    for (auto& c : a.selective) for (auto& v : c) v = std::clamp(v, -100, 100);
    for (auto& s : a.grad) {
        s.at = clamp01(s.at);
        s.mid = std::clamp(s.mid, 0.05, 0.95);
        for (auto& v : s.rgb) v = clamp01(v);
    }
    std::stable_sort(a.grad.begin(), a.grad.end(), [](const GradStop& p, const GradStop& q) { return p.at < q.at; });
    if (a.grad.size() > kGradStops) a.grad.resize(kGradStops);
    if (a.grad.size() < 2) a.grad = Adjust{}.grad;
    // s065: the inks -- one to four; each plate's Levels and Curve as Levels' and Curves' own.
    if (a.inks.empty()) a.inks = Adjust{}.inks;
    if (a.inks.size() > kMaxInks) a.inks.resize(kMaxInks);
    for (auto& k : a.inks) {
        for (auto& v : k.rgb) v = clamp01(v);
        auto& c = k.levels;
        c.in_black = clamp01(c.in_black);
        c.in_white = std::clamp(c.in_white, std::min(1.0, c.in_black + 2.0 / 255), 1.0);
        c.in_black = std::min(c.in_black, c.in_white - 2.0 / 255);
        c.gamma = std::clamp(c.gamma, 0.1, 9.99);
        c.out_black = clamp01(c.out_black);
        c.out_white = clamp01(c.out_white);
        for (auto& p : k.curve) { p.x = clamp01(p.x); p.y = clamp01(p.y); }
        std::stable_sort(k.curve.begin(), k.curve.end(), [](const CurvePt& p, const CurvePt& q) { return p.x < q.x; });
        std::vector<CurvePt> kept;
        for (const auto& p : k.curve) {
            if (!kept.empty() && p.x - kept.back().x < 1.0 / 255) kept.back() = p;
            else kept.push_back(p);
        }
        if (kept.size() > 16) kept.resize(16);
        if (kept.size() < 2) kept = {{0, 0}, {1, 1}};
        k.curve = std::move(kept);
    }
    return a;
}

bool identity(const Adjust& a) {
    switch (a.kind) {
        case AdjustKind::None: return true;
        case AdjustKind::Levels:
            for (const auto& c : a.levels) if (!(c == LevelsCh{})) return false;
            return true;
        case AdjustKind::Curves:
            for (const auto& c : a.curves)
                for (const auto& p : c) if (std::abs(p.x - p.y) > 1e-9) return false;
            return true;
        case AdjustKind::HueSat:
            if (a.colorize || a.hue != 0 || a.saturation != 0 || a.lightness != 0) return false;
            for (const auto& rg : a.ranges) if (rg.hue || rg.saturation || rg.lightness) return false;   // s064
            return true;
        case AdjustKind::BrightCon: return a.brightness == 0 && a.contrast == 0;
        case AdjustKind::Exposure:  return a.exposure == 0 && a.offset == 0 && a.exp_gamma == 1;
        case AdjustKind::Invert:
        case AdjustKind::Posterize:
        case AdjustKind::Threshold:
        case AdjustKind::BlackWhite:
        case AdjustKind::PhotoFilter: return false;   // their own effect
        case AdjustKind::Vibrance:  return a.vibrance == 0 && a.saturation == 0;
        case AdjustKind::ColourBalance:   // s063 (Preserve Luminosity alone changes nothing)
            for (const auto& t : a.balance) for (const int v : t) if (v != 0) return false;
            return true;
        case AdjustKind::ChannelMixer: return !a.monochrome && a.mixer == fresh(AdjustKind::ChannelMixer).mixer;
        case AdjustKind::SelectiveColour:
            for (const auto& c : a.selective) for (const int v : c) if (v != 0) return false;
            return true;
        case AdjustKind::GradientMap:
        case AdjustKind::Duotone: return false;   // their own effect
        case AdjustKind::ColourLookup: return !a.lut.p;   // s066: no table yet
    }
    return true;
}

bool settingless(AdjustKind k) { return k == AdjustKind::Invert; }
bool wants_histogram(AdjustKind k) {
    return k == AdjustKind::Levels || k == AdjustKind::Curves || k == AdjustKind::Threshold || k == AdjustKind::Duotone ||   // s065: behind each ink's plate
           k == AdjustKind::BlackWhite;   // s066: its Auto reads the colours
}

std::string refusal(const Adjust& a) {
    if (a.kind == AdjustKind::None) return "no adjustment";
    const Adjust n = normalised(a);
    if (!(n == a)) return "settings out of range";
    for (const auto& c : a.curves) if (c.size() > 16) return "more than 16 points on a curve";
    if (a.lut.p) if (auto why = lut::refusal(*a.lut.p); !why.empty()) return "colour lookup: " + why;   // s066
    return {};
}

double levels_map(const LevelsCh& c, double v) {
    const double span = std::max(1e-6, c.in_white - c.in_black);
    double t = clamp01((v - c.in_black) / span);
    if (c.gamma != 1) t = std::pow(t, 1.0 / c.gamma);
    return c.out_black + (c.out_white - c.out_black) * t;
}

std::vector<double> curve_table(const std::vector<CurvePt>& in, int n) {
    std::vector<double> out(static_cast<std::size_t>(std::max(n, 2)));
    const auto N = out.size();
    std::vector<CurvePt> p = in;
    std::stable_sort(p.begin(), p.end(), [](const CurvePt& a, const CurvePt& b) { return a.x < b.x; });
    if (p.size() < 2) {
        for (std::size_t i = 0; i < N; ++i) out[i] = static_cast<double>(i) / static_cast<double>(N - 1);
        return out;
    }
    // Natural cubic spline: second derivatives m (m = 0 at both ends).
    const std::size_t k = p.size();
    std::vector<double> m(k, 0.0), u(k, 0.0);
    for (std::size_t i = 1; i + 1 < k; ++i) {
        const double hl = p[i].x - p[i - 1].x, hr = p[i + 1].x - p[i].x;
        if (hl <= 0 || hr <= 0) continue;
        const double sig = hl / (hl + hr), q = sig * m[i - 1] + 2;
        m[i] = (sig - 1) / q;
        u[i] = (6 * ((p[i + 1].y - p[i].y) / hr - (p[i].y - p[i - 1].y) / hl) / (hl + hr) - sig * u[i - 1]) / q;
    }
    m[k - 1] = 0;
    for (std::size_t i = k - 1; i-- > 0;) m[i] = m[i] * m[i + 1] + u[i];
    for (std::size_t s = 0; s < N; ++s) {
        const double x = static_cast<double>(s) / static_cast<double>(N - 1);
        double y;
        if (x <= p.front().x) y = p.front().y;
        else if (x >= p.back().x) y = p.back().y;
        else {
            std::size_t hi = 1;
            while (hi < k - 1 && p[hi].x < x) ++hi;
            const std::size_t lo = hi - 1;
            const double h = p[hi].x - p[lo].x;
            if (h <= 0) { y = p[hi].y; }
            else {
                const double A = (p[hi].x - x) / h, B = (x - p[lo].x) / h;
                y = A * p[lo].y + B * p[hi].y + ((A * A * A - A) * m[lo] + (B * B * B - B) * m[hi]) * h * h / 6;
            }
        }
        out[s] = clamp01(y);
    }
    return out;
}

void apply_rgb(const Adjust& a, float& r, float& g, float& b) {
    run_rgb(*compile(a), r, g, b);
}

std::shared_ptr<const Compiled> compile(const Adjust& a) {
    // A few recent ones per thread: a composite asks once per tile per layer.
    thread_local std::vector<std::shared_ptr<const Compiled>> cache;
    for (const auto& c : cache) if (c->a == a) return c;
    auto c = std::make_shared<Compiled>();
    c->kind = a.kind;
    c->a = a;
    // Per channel, so tables; the rest (Hue/Saturation, Threshold, s062's)
    // mix the channels and run per pixel.
    if (a.kind == AdjustKind::Levels || a.kind == AdjustKind::Curves || a.kind == AdjustKind::BrightCon ||
        a.kind == AdjustKind::Invert || a.kind == AdjustKind::Posterize || a.kind == AdjustKind::Exposure ||
        a.kind == AdjustKind::GradientMap) {
        std::vector<double> tables[kAdjustChannels];
        if (a.kind == AdjustKind::Curves)
            for (int i = 0; i < kAdjustChannels; ++i) tables[i] = curve_table(a.curves[static_cast<std::size_t>(i)], kLut);
        for (int ch = 0; ch < 3; ++ch) {
            auto& lut = c->lut[static_cast<std::size_t>(ch)];
            lut.resize(kLut);
            for (int i = 0; i < kLut; ++i)
                lut[static_cast<std::size_t>(i)] = static_cast<float>(clamp01(lut_kind(a, tables, ch, static_cast<double>(i) / (kLut - 1))));
        }
    }
    if (a.kind == AdjustKind::ColourLookup) c->table = a.lut.p;   // s066
    if (a.kind == AdjustKind::Duotone)   // s065: each ink's amount, by grey
        for (const auto& k : a.inks) {
            std::vector<float> p(kLut);
            const auto t = k.use_curve ? curve_table(k.curve, kLut) : std::vector<double>{};
            for (int i = 0; i < kLut; ++i)
                p[static_cast<std::size_t>(i)] = static_cast<float>(1 - (k.use_curve ? t[static_cast<std::size_t>(i)] : levels_map(k.levels, static_cast<double>(i) / (kLut - 1))));
            c->plates.push_back(std::move(p));
        }
    if (cache.size() >= 8) cache.erase(cache.begin());
    cache.push_back(c);
    return c;
}

void run_rgb(const Compiled& c, float& r, float& g, float& b) {
    switch (c.kind) {
        case AdjustKind::None: return;
        case AdjustKind::HueSat: {
            double R = std::clamp(r, 0.f, 1.f), G = std::clamp(g, 0.f, 1.f), B = std::clamp(b, 0.f, 1.f);
            hue_sat(c.a, R, G, B);
            r = static_cast<float>(clamp01(R)); g = static_cast<float>(clamp01(G)); b = static_cast<float>(clamp01(B));
            return;
        }
        case AdjustKind::Vibrance:
        case AdjustKind::BlackWhite:
        case AdjustKind::PhotoFilter:     // s062
        case AdjustKind::ColourBalance:   // s063
        case AdjustKind::ChannelMixer:
        case AdjustKind::SelectiveColour: {
            double R = std::clamp(r, 0.f, 1.f), G = std::clamp(g, 0.f, 1.f), B = std::clamp(b, 0.f, 1.f);
            if (c.kind == AdjustKind::Vibrance) vibrance(c.a, R, G, B);
            else if (c.kind == AdjustKind::PhotoFilter) photo_filter(c.a, R, G, B);
            else if (c.kind == AdjustKind::ColourBalance) colour_balance(c.a, R, G, B);
            else if (c.kind == AdjustKind::ChannelMixer) channel_mixer(c.a, R, G, B);
            else if (c.kind == AdjustKind::SelectiveColour) selective_colour(c.a, R, G, B);
            else {
                const double v = black_white(c.a, R, G, B);
                if (c.a.bw_tint) rgb_of(c.a.tint_hue / 360.0, c.a.tint_sat / 100.0, v, R, G, B);
                else R = G = B = v;
            }
            r = static_cast<float>(clamp01(R)); g = static_cast<float>(clamp01(G)); b = static_cast<float>(clamp01(B));
            return;
        }
        case AdjustKind::ColourLookup:   // s066: through the table
            if (c.table) lut::apply(*c.table, r, g, b);
            return;
        case AdjustKind::GradientMap: {   // s063: the luminosity down the gradient's table
            const float l = static_cast<float>(luma(std::clamp(r, 0.f, 1.f), std::clamp(g, 0.f, 1.f), std::clamp(b, 0.f, 1.f)));
            r = lut_at(c.lut[0], l); g = lut_at(c.lut[1], l); b = lut_at(c.lut[2], l);
            return;
        }
        case AdjustKind::Duotone: {   // s065: the inks multiplied over white, each by its plate
            const double grey = duotone_grey(c.a, std::clamp(r, 0.f, 1.f), std::clamp(g, 0.f, 1.f), std::clamp(b, 0.f, 1.f));
            float R = 1, G = 1, B = 1;
            for (std::size_t i = 0; i < c.plates.size() && i < c.a.inks.size(); ++i) {
                const float k = lut_at(c.plates[i], static_cast<float>(grey));
                const auto& ink = c.a.inks[i].rgb;
                R *= 1 - k * (1 - static_cast<float>(ink[0]));
                G *= 1 - k * (1 - static_cast<float>(ink[1]));
                B *= 1 - k * (1 - static_cast<float>(ink[2]));
            }
            r = R; g = G; b = B;
            return;
        }
        case AdjustKind::Threshold: {   // s061: Photoshop's luminance weights, in levels
            const double lum = 0.299 * std::clamp(r, 0.f, 1.f) + 0.587 * std::clamp(g, 0.f, 1.f) + 0.114 * std::clamp(b, 0.f, 1.f);
            r = g = b = lum * 255 + 1e-6 >= c.a.threshold ? 1.f : 0.f;
            return;
        }
        default:
            r = lut_at(c.lut[0], r);
            g = lut_at(c.lut[1], g);
            b = lut_at(c.lut[2], b);
    }
}

float dither_at(int x, int y) {   // s066: Gradient.cpp's fixed hash
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffffu) / 65535.f - 0.5f;
}

namespace {
// s066: a Gradient Map's Dither -- each channel nudged by its own fixed noise,
// one 8-bit step wide, so the steps of a long gentle gradient break up.
bool dithers(const Compiled& c) { return c.kind == AdjustKind::GradientMap && c.a.grad_dither; }
void dither_rgb(int x, int y, float& r, float& g, float& b) {
    constexpr float q = 1.f / 255;
    r = std::clamp(r + dither_at(x, y) * q, 0.f, 1.f);
    g = std::clamp(g + dither_at(x + 40503, y + 7) * q, 0.f, 1.f);
    b = std::clamp(b + dither_at(x + 81001, y + 13) * q, 0.f, 1.f);
}
}  // namespace

void run(const Compiled& c, float* p, std::size_t n, int x0, int y0, int w) {
    if (!dithers(c) || w <= 0) { run(c, p, n); return; }
    for (std::size_t i = 0; i < n; ++i, p += 4) {
        const float a = p[3];
        if (a <= 0.f) continue;
        float r = p[0] / a, g = p[1] / a, b = p[2] / a;
        run_rgb(c, r, g, b);
        const auto k = static_cast<int>(i);
        dither_rgb(x0 + k % w, y0 + k / w, r, g, b);
        p[0] = r * a; p[1] = g * a; p[2] = b * a;
    }
}

void run(const Compiled& c, float* p, std::size_t n) {
    if (c.kind == AdjustKind::None) return;
    for (std::size_t i = 0; i < n; ++i, p += 4) {
        const float a = p[3];
        if (a <= 0.f) continue;
        float r = p[0] / a, g = p[1] / a, b = p[2] / a;
        run_rgb(c, r, g, b);
        p[0] = r * a; p[1] = g * a; p[2] = b * a;
    }
}

std::optional<pixel::TileMap> applied(const pixel::TileMap& src, const Adjust& a, const pixel::TileMap* sel,
                                      const std::function<bool()>& cancelled) {
    const auto& f = src.format();
    if (f.mode != pixel::Mode::RGB && f.mode != pixel::Mode::Gray) return std::nullopt;
    if (identity(a)) return src;
    const auto c = compile(a);
    const bool gray = f.mode == pixel::Mode::Gray, dith = dithers(*c);
    const auto ai = static_cast<std::size_t>(f.channels() - 1);
    pixel::Rect r{0, 0, src.width(), src.height()};
    if (sel) {
        r = select::extent(*sel);
        if (r.empty()) return src;
    }
    pixel::TileMap out = src;
    for (int y0 = r.y; y0 < r.y + r.h; y0 += pixel::kTile) {
        if (cancelled && cancelled()) return std::nullopt;
        const pixel::Rect band = pixel::Rect{r.x, y0, r.w, pixel::kTile}.intersect(r);
        out = out.paint(band, [&](int x, int y, const pixel::Px& o) {
            if (f.alpha && o.c[ai] <= 0.f) return o;
            const float k = sel ? select::coverage(*sel, x, y) : 1.f;
            if (k <= 0.f) return o;
            pixel::Px p = o;
            if (gray) {
                float v = o.c[0], g = v, b = v;
                run_rgb(*c, v, g, b);
                if (dith) dither_rgb(x, y, v, g, b);
                p.c[0] = o.c[0] + ((v + g + b) / 3 - o.c[0]) * k;
            } else {
                float R = o.c[0], G = o.c[1], B = o.c[2];
                run_rgb(*c, R, G, B);
                if (dith) dither_rgb(x, y, R, G, B);
                p.c[0] = o.c[0] + (R - o.c[0]) * k;
                p.c[1] = o.c[1] + (G - o.c[1]) * k;
                p.c[2] = o.c[2] + (B - o.c[2]) * k;
            }
            return p;
        });
    }
    return out;
}

Adjust auto_tone(const Adjust& in, const Histogram& h, double clip) {
    Adjust a = in;
    for (std::size_t c = 1; c <= 3; ++c) {
        std::uint64_t total = 0;
        for (const auto v : h[c]) total += v;
        if (total == 0) continue;
        const auto cut = static_cast<std::uint64_t>(std::floor(static_cast<double>(total) * std::clamp(clip, 0.0, 0.49)));
        int lo = 0, hi = 255;
        for (std::uint64_t run = 0; lo < 255; ++lo) { run += h[c][static_cast<std::size_t>(lo)]; if (run > cut) break; }
        for (std::uint64_t run = 0; hi > 0; --hi) { run += h[c][static_cast<std::size_t>(hi)]; if (run > cut) break; }
        if (hi - lo < 2) continue;   // one tone: nothing to stretch
        if (a.kind == AdjustKind::Levels) {
            auto& L = a.levels[c];
            L.in_black = lo / 255.0;
            L.in_white = hi / 255.0;
            L.gamma = 1;
        } else if (a.kind == AdjustKind::Curves) {
            a.curves[c] = {{lo / 255.0, 0}, {hi / 255.0, 1}};
        }
    }
    return normalised(a);
}

// ── s078: the other autos ───────────────────────────────────────────────────
const char* auto_label(AutoMethod m) {
    switch (m) {
        case AutoMethod::Tone:         return "Auto Tone";
        case AutoMethod::Contrast:     return "Auto Contrast";
        case AutoMethod::Colour:       return "Auto Color";
        case AutoMethod::WhiteBalance: return "Auto White Balance";
    }
    return "Auto";
}

namespace {
// The darkest and lightest levels of a histogram, past `clip` of it at each end.
bool ends(const std::array<std::uint32_t, 256>& h, double clip, int& lo, int& hi) {
    std::uint64_t total = 0;
    for (const auto v : h) total += v;
    if (total == 0) return false;
    const auto cut = static_cast<std::uint64_t>(std::floor(static_cast<double>(total) * std::clamp(clip, 0.0, 0.49)));
    lo = 0; hi = 255;
    for (std::uint64_t run = 0; lo < 255; ++lo) { run += h[static_cast<std::size_t>(lo)]; if (run > cut) break; }
    for (std::uint64_t run = 0; hi > 0; --hi) { run += h[static_cast<std::size_t>(hi)]; if (run > cut) break; }
    return hi - lo >= 2;
}
// The picture's neutral: the average of its near-grey midtone colours (each
// channel within 5/32 of the others, luminance between a quarter and three
// quarters); too few of them (< 0.5 %): the average of every colour.
std::array<double, 3> neutral_of(const Histogram& h) {
    double w = 0, all = 0;
    std::array<double, 3> sum{}, sum_all{};
    for (std::size_t i = 0; i < kColourBins; ++i) {
        const double n = h.colours[i];
        if (n == 0) continue;
        const int r = static_cast<int>(i >> 10), g = static_cast<int>((i >> 5) & 31), b = static_cast<int>(i & 31);
        const std::array<double, 3> c{(r + 0.5) / 32, (g + 0.5) / 32, (b + 0.5) / 32};
        for (std::size_t k = 0; k < 3; ++k) sum_all[k] += n * c[k];
        all += n;
        const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
        const double lum = 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
        if (mx - mn <= 5 && lum >= 0.25 && lum <= 0.75) {
            for (std::size_t k = 0; k < 3; ++k) sum[k] += n * c[k];
            w += n;
        }
    }
    if (all == 0) return {0.5, 0.5, 0.5};
    if (w < all * 0.005) return {sum_all[0] / all, sum_all[1] / all, sum_all[2] / all};
    return {sum[0] / w, sum[1] / w, sum[2] / w};
}
}  // namespace

Adjust auto_adjust(const Adjust& in, const Histogram& h, AutoMethod m, double clip) {
    if (in.kind != AdjustKind::Levels && in.kind != AdjustKind::Curves) return in;
    if (m == AutoMethod::Tone) return auto_tone(in, h, clip);
    Adjust a = in;
    if (m == AutoMethod::Contrast) {   // the three as one: the colours keep their balance
        std::array<std::uint32_t, 256> rgb{};
        for (std::size_t c = 1; c <= 3; ++c)
            for (std::size_t i = 0; i < 256; ++i) rgb[i] += h[c][i];
        int lo = 0, hi = 255;
        if (!ends(rgb, clip, lo, hi)) return a;
        if (a.kind == AdjustKind::Levels) { auto& L = a.levels[0]; L.in_black = lo / 255.0; L.in_white = hi / 255.0; L.gamma = 1; }
        else a.curves[0] = {{lo / 255.0, 0}, {hi / 255.0, 1}};
        return normalised(a);
    }
    if (m == AutoMethod::Colour) a = auto_tone(a, h, clip);
    // The neutral, through what the channels do now, made grey -- each
    // channel's midtone moved to the three's average.
    const auto nt = neutral_of(h);
    std::array<double, 3> now{};
    for (std::size_t c = 0; c < 3; ++c) {
        if (a.kind == AdjustKind::Levels) now[c] = levels_map(a.levels[c + 1], nt[c]);
        else {
            const auto t = curve_table(a.curves[c + 1], 1024);
            now[c] = t[static_cast<std::size_t>(std::clamp(nt[c], 0.0, 1.0) * 1023)];
        }
    }
    const double target = (now[0] + now[1] + now[2]) / 3;
    if (target <= 0.02 || target >= 0.98) return normalised(a);   // nothing in the middle to balance
    for (std::size_t c = 0; c < 3; ++c) {
        if (now[c] <= 0.005 || now[c] >= 0.995 || std::abs(now[c] - target) < 1e-4) continue;
        if (a.kind == AdjustKind::Levels) {
            // t^(1/g) = target, t the channel's stretched value before its gamma
            auto& L = a.levels[c + 1];
            const double span = std::max(1e-6, L.in_white - L.in_black);
            const double t = std::clamp((nt[c] - L.in_black) / span, 1e-4, 0.9999);
            L.gamma = std::clamp(std::log(t) / std::log(target), 0.1, 9.99);
        } else {
            auto pts = a.curves[c + 1];
            if (pts.size() < 2) pts = {{0, 0}, {1, 1}};
            pts.push_back({std::clamp(nt[c], 0.02, 0.98), target});
            std::sort(pts.begin(), pts.end(), [](const CurvePt& p, const CurvePt& q) { return p.x < q.x; });
            a.curves[c + 1] = pts;
        }
    }
    return normalised(a);
}

Histogram histogram_of(const pixel::TileMap& px, const pixel::TileMap* sel) {
    const int w = px.width(), hgt = px.height();
    std::vector<std::uint32_t> argb;
    const double area = static_cast<double>(w) * hgt;
    const int step = std::max(1, static_cast<int>(std::floor(std::sqrt(area / 1.0e6))));
    argb.reserve(static_cast<std::size_t>(area / (step * step)) + 1);
    const bool gray = px.format().mode == pixel::Mode::Gray;
    for (int y = 0; y < hgt; y += step)
        for (int x = 0; x < w; x += step) {
            if (sel && sel->get(x, y).c[0] < 0.5f) continue;
            const auto p = px.get(x, y);
            const float al = px.format().alpha ? p.c[gray ? 1 : 3] : 1.f;
            if (al <= 0.f) continue;
            const auto q = [&](float v) { return static_cast<std::uint32_t>(std::clamp(v * al, 0.f, 1.f) * 255.f + 0.5f); };
            const float r = p.c[0], g = gray ? p.c[0] : p.c[1], b = gray ? p.c[0] : p.c[2];
            argb.push_back((static_cast<std::uint32_t>(std::clamp(al, 0.f, 1.f) * 255.f + 0.5f) << 24) | (q(r) << 16) | (q(g) << 8) | q(b));
        }
    return histogram(argb);
}

Adjust dropped(const Adjust& in, Dropper d, const std::array<double, 3>& rgb) {
    Adjust a = in;
    if (d == Dropper::None || (a.kind != AdjustKind::Levels && a.kind != AdjustKind::Curves)) return a;
    const auto curve_at = [&](std::size_t c, double x) {
        const auto t = curve_table(a.curves[c], 1024);
        const double p = clamp01(x) * 1023;
        const auto i = std::min<std::size_t>(static_cast<std::size_t>(p), 1022);
        return t[i] + (t[i + 1] - t[i]) * (p - static_cast<double>(i));
    };
    double target = 0;   // grey: the average of what the three map to now
    for (std::size_t c = 0; c < 3; ++c)
        target += (a.kind == AdjustKind::Levels ? levels_map(a.levels[c + 1], rgb[c]) : curve_at(c + 1, rgb[c])) / 3;
    for (std::size_t c = 0; c < 3; ++c) {
        const double v = clamp01(rgb[c]);
        if (a.kind == AdjustKind::Levels) {
            auto& L = a.levels[c + 1];
            if (d == Dropper::Black) L.in_black = std::min(v, L.in_white - 2.0 / 255);
            else if (d == Dropper::White) L.in_white = std::max(v, L.in_black + 2.0 / 255);
            else {
                const double t = clamp01((v - L.in_black) / std::max(1e-6, L.in_white - L.in_black));
                const double u = (target - L.out_black) / std::max(1e-6, L.out_white - L.out_black);
                if (t > 1e-4 && t < 1 - 1e-4 && u > 1e-4 && u < 1 - 1e-4) L.gamma = std::clamp(std::log(t) / std::log(u), 0.1, 9.99);
            }
        } else {
            auto pts = a.curves[c + 1];
            if (pts.size() < 2) pts = {{0, 0}, {1, 1}};
            std::vector<CurvePt> kept;
            if (d == Dropper::Black) {
                kept.push_back({v, 0});
                for (const auto& p : pts) if (p.x > v + 1.0 / 255) kept.push_back(p);
                if (kept.size() < 2) kept.push_back({1, 1});
            } else if (d == Dropper::White) {
                for (const auto& p : pts) if (p.x < v - 1.0 / 255) kept.push_back(p);
                if (kept.empty()) kept.push_back({0, 0});
                kept.push_back({v, 1});
            } else {
                if (v <= 1.0 / 255 || v >= 1 - 1.0 / 255) continue;
                for (const auto& p : pts) if (std::abs(p.x - v) >= 1.0 / 255) kept.push_back(p);
                if (kept.size() >= 16) continue;
                kept.push_back({v, clamp01(target)});
            }
            a.curves[c + 1] = std::move(kept);
        }
    }
    return normalised(a);
}

double duotone_grey(const Adjust& a, double r, double g, double b) {
    return a.grey_bw ? black_white(a, clamp01(r), clamp01(g), clamp01(b)) : clamp01(luma(r, g, b));
}
double ink_plate(const Ink& k, double grey) {
    if (!k.use_curve) return levels_map(k.levels, grey);
    const auto t = curve_table(k.curve, 1024);
    const double p = clamp01(grey) * 1023;
    const auto i = std::min<std::size_t>(static_cast<std::size_t>(p), 1022);
    return t[i] + (t[i + 1] - t[i]) * (p - static_cast<double>(i));
}
std::array<double, 3> duotone_at(const Adjust& a, double grey) {
    std::array<double, 3> out{1, 1, 1};
    for (const auto& k : a.inks) {
        const double amount = 1 - ink_plate(k, grey);
        for (std::size_t c = 0; c < 3; ++c) out[c] *= 1 - amount * (1 - k.rgb[c]);
    }
    return out;
}
Adjust duotone_as_gradient(const Adjust& a) {
    Adjust g = fresh(AdjustKind::GradientMap);
    g.grad.clear();
    for (std::size_t i = 0; i < kGradStops; ++i) {
        const double t = static_cast<double>(i) / (kGradStops - 1);
        g.grad.push_back({t, 0.5, duotone_at(a, t)});
    }
    return g;
}

Histogram histogram(const std::vector<std::uint32_t>& argb) {
    Histogram h{};
    for (const std::uint32_t px : argb) {
        const std::uint32_t a = px >> 24;
        if (a == 0) continue;
        const auto un = [a](std::uint32_t v) { return std::min<std::uint32_t>(255, (v * 255 + a / 2) / a); };
        const std::uint32_t r = un((px >> 16) & 0xff), g = un((px >> 8) & 0xff), b = un(px & 0xff);
        ++h[1][r]; ++h[2][g]; ++h[3][b];
        ++h.colours[(r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)];
        ++h[0][std::min<std::uint32_t>(255, (299 * r + 587 * g + 114 * b + 500) / 1000)];
    }
    return h;
}

namespace {
// s066: one colour bin, as Black & White's mix sees it: grey = mn + d1 x the
// secondary's weight + d2 x the primary's (black_white above).
struct BwBin { double mn, d1, d2, n; int ws, wp; };
std::vector<BwBin> bw_bins(const Histogram& h, double& total, double& coloured) {
    std::vector<BwBin> v;
    total = coloured = 0;
    enum { Re, Ye, Gr, Cy, Bl, Ma };
    for (std::size_t i = 0; i < kColourBins; ++i) {
        const std::uint32_t n = h.colours[i];
        if (n == 0) continue;
        const double r = (static_cast<double>(i >> 10 & 31) + 0.5) / 32, g = (static_cast<double>(i >> 5 & 31) + 0.5) / 32,
                     b = (static_cast<double>(i & 31) + 0.5) / 32;
        double mx, md, mn; int wp, ws;
        if (r >= g && r >= b) { mx = r; wp = Re; if (g >= b) { md = g; mn = b; ws = Ye; } else { md = b; mn = g; ws = Ma; } }
        else if (g >= b)      { mx = g; wp = Gr; if (r >= b) { md = r; mn = b; ws = Ye; } else { md = b; mn = r; ws = Cy; } }
        else                  { mx = b; wp = Bl; if (r >= g) { md = r; mn = g; ws = Ma; } else { md = g; mn = r; ws = Cy; } }
        v.push_back({mn, md - mn, mx - md, static_cast<double>(n), ws, wp});
        total += n;
        if (mx - mn > 1.5 / 32) coloured += n;
    }
    return v;
}
}  // namespace

std::array<std::uint32_t, 256> bw_histogram(const Adjust& a, const Histogram& h) {
    std::array<std::uint32_t, 256> out{};
    double total = 0, coloured = 0;
    for (const auto& b : bw_bins(h, total, coloured)) {
        const double g = clamp01(b.mn + b.d1 * a.bw[static_cast<std::size_t>(b.ws)] / 100.0 + b.d2 * a.bw[static_cast<std::size_t>(b.wp)] / 100.0);
        out[static_cast<std::size_t>(std::lround(g * 255))] += static_cast<std::uint32_t>(b.n);
    }
    return out;
}

Adjust bw_auto(const Adjust& in, const Histogram& h) {
    Adjust a = in;
    double total = 0, coloured = 0;
    const auto bins = bw_bins(h, total, coloured);
    if (total <= 0 || coloured < total * 0.002) return a;   // a grey picture: nothing to mix
    const auto fresh_bw = fresh(AdjustKind::BlackWhite).bw;
    // How good a mix is: the spread of its greys (standard deviation), less
    // what it drives past black or white beyond what the default mix already
    // does, less a light pull toward the defaults -- so a hue the picture has
    // little of stays where Photoshop starts it.
    // How far a mix pushes greys to the ends: past black / white (clipped),
    // or into the last 6 % at either end (blocked up / blown out).
    struct Ends { double clipped, extreme; };
    const auto ends = [&](const std::array<int, 6>& w) {
        Ends e{0, 0};
        for (const auto& b : bins) {
            const double g = b.mn + b.d1 * w[static_cast<std::size_t>(b.ws)] / 100.0 + b.d2 * w[static_cast<std::size_t>(b.wp)] / 100.0;
            if (g < -0.5 / 255 || g > 1 + 0.5 / 255) e.clipped += b.n;
            if (g < 0.06 || g > 0.94) e.extreme += b.n;
        }
        e.clipped /= total; e.extreme /= total;
        return e;
    };
    const Ends start = ends(fresh_bw);
    const double free_clip = start.clipped + 0.002, free_extreme = start.extreme + 0.01;
    const auto score = [&](const std::array<int, 6>& w) {
        double s = 0, s2 = 0;
        for (const auto& b : bins) {
            const double g = clamp01(b.mn + b.d1 * w[static_cast<std::size_t>(b.ws)] / 100.0 + b.d2 * w[static_cast<std::size_t>(b.wp)] / 100.0);
            s += g * b.n; s2 += g * g * b.n;
        }
        const double m = s / total, sd = std::sqrt(std::max(0.0, s2 / total - m * m));
        const Ends e = ends(w);
        double pull = 0;
        for (std::size_t i = 0; i < 6; ++i) { const double d = (w[i] - fresh_bw[i]) / 100.0; pull += d * d; }
        return sd - 20 * std::max(0.0, e.clipped - free_clip) - 4 * std::max(0.0, e.extreme - free_extreme) - 0.01 * pull;
    };
    std::array<int, 6> w = fresh_bw;
    double best = score(w);
    // Coordinate ascent: each slider over its range in 20s, then in 5s round
    // the best, until nothing moves.
    for (int pass = 0; pass < 8; ++pass) {
        bool moved = false;
        for (std::size_t i = 0; i < 6; ++i) {
            for (const int step : {20, 5}) {
                const int lo = step == 20 ? -200 : std::max(-200, w[i] - 20), hi = step == 20 ? 300 : std::min(300, w[i] + 20);
                for (int v = lo; v <= hi; v += step) {
                    if (v == w[i]) continue;
                    auto t = w;
                    t[i] = v;
                    const double sc = score(t);
                    if (sc > best + 1e-9) { best = sc; w = t; moved = true; }
                }
            }
        }
        if (!moved) break;
    }
    a.bw = w;
    return normalised(a);
}

}  // namespace klimt::core::adjust
