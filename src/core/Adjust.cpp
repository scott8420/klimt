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

// RGB <-> HSL, all 0..1 (hue in turns).
void to_hsl(double r, double g, double b, double& h, double& s, double& l) {
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
void from_hsl(double h, double s, double l, double& r, double& g, double& b) {
    if (s <= 0) { r = g = b = l; return; }
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    r = hue_part(p, q, h + 1.0 / 3);
    g = hue_part(p, q, h);
    b = hue_part(p, q, h - 1.0 / 3);
}

void hue_sat(const Adjust& a, double& r, double& g, double& b) {
    if (a.colorize) {
        // The colour's lightness kept, hue and saturation set.
        const double lum = 0.299 * r + 0.587 * g + 0.114 * b;
        double h = std::fmod(a.hue + 360.0, 360.0) / 360.0;
        from_hsl(h, std::clamp(a.saturation, 0, 100) / 100.0, lum, r, g, b);
    } else if (a.hue != 0 || a.saturation != 0) {
        double h, s, l;
        to_hsl(r, g, b, h, s, l);
        h = std::fmod(h + a.hue / 360.0 + 1.0, 1.0);
        const double k = a.saturation / 100.0;
        s = k <= 0 ? s * (1 + k) : clamp01(s + (1 - s) * s * k * 2);   // more: the grey stays grey
        from_hsl(h, s, l, r, g, b);
    }
    if (a.lightness != 0) {
        const double k = a.lightness / 100.0;
        const auto f = [k](double v) { return k < 0 ? v * (1 + k) : v + (1 - v) * k; };
        r = f(r); g = f(g); b = f(b);
    }
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
        default: return v;
    }
}
}  // namespace

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
    static const std::vector<AdjustKind> v{AdjustKind::BrightCon, AdjustKind::Levels, AdjustKind::Curves, AdjustKind::HueSat};
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
    a.brightness = std::clamp(a.brightness, -150, 150);
    a.contrast = std::clamp(a.contrast, -50, 100);
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
        case AdjustKind::HueSat: return !a.colorize && a.hue == 0 && a.saturation == 0 && a.lightness == 0;
        case AdjustKind::BrightCon: return a.brightness == 0 && a.contrast == 0;
    }
    return true;
}

std::string refusal(const Adjust& a) {
    if (a.kind == AdjustKind::None) return "no adjustment";
    const Adjust n = normalised(a);
    if (!(n == a)) return "settings out of range";
    for (const auto& c : a.curves) if (c.size() > 16) return "more than 16 points on a curve";
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
    if (a.kind == AdjustKind::Levels || a.kind == AdjustKind::Curves || a.kind == AdjustKind::BrightCon) {
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
        default:
            r = lut_at(c.lut[0], r);
            g = lut_at(c.lut[1], g);
            b = lut_at(c.lut[2], b);
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
    const bool gray = f.mode == pixel::Mode::Gray;
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
                p.c[0] = o.c[0] + ((v + g + b) / 3 - o.c[0]) * k;
            } else {
                float R = o.c[0], G = o.c[1], B = o.c[2];
                run_rgb(*c, R, G, B);
                p.c[0] = o.c[0] + (R - o.c[0]) * k;
                p.c[1] = o.c[1] + (G - o.c[1]) * k;
                p.c[2] = o.c[2] + (B - o.c[2]) * k;
            }
            return p;
        });
    }
    return out;
}

Histogram histogram(const std::vector<std::uint32_t>& argb) {
    Histogram h{};
    for (const std::uint32_t px : argb) {
        const std::uint32_t a = px >> 24;
        if (a == 0) continue;
        const auto un = [a](std::uint32_t v) { return std::min<std::uint32_t>(255, (v * 255 + a / 2) / a); };
        const std::uint32_t r = un((px >> 16) & 0xff), g = un((px >> 8) & 0xff), b = un(px & 0xff);
        ++h[1][r]; ++h[2][g]; ++h[3][b];
        ++h[0][std::min<std::uint32_t>(255, (299 * r + 587 * g + 114 * b + 500) / 1000)];
    }
    return h;
}

}  // namespace klimt::core::adjust
