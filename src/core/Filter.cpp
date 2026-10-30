// core::Filter -- the Filter menu's maths (s014). docs/filters.md.
#include "core/Filter.hpp"
#include "core/Selection.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>

namespace klimt::core::filter {

namespace {
using pixel::kTile;
using pixel::Px;
using pixel::Rect;

struct Info { FilterKind k; const char* key; const char* label; const char* group; };
constexpr Info kInfo[] = {
    {FilterKind::GaussianBlur, "gaussian-blur", "Gaussian Blur", "Blur"},
    {FilterKind::UnsharpMask,  "unsharp-mask",  "Unsharp Mask",  "Sharpen"},
    {FilterKind::HighPass,     "high-pass",     "High Pass",     "Other"},
    {FilterKind::AddNoise,     "add-noise",     "Add Noise",     "Noise"},
    {FilterKind::BoxBlur,      "box-blur",      "Box Blur",      "Blur"},     // m3
    {FilterKind::MotionBlur,   "motion-blur",   "Motion Blur",   "Blur"},
    {FilterKind::Median,       "median",        "Median",        "Noise"},
    {FilterKind::SmartSharpen, "smart-sharpen", "Smart Sharpen", "Sharpen"},   // s034
    {FilterKind::DustScratches,"dust-scratches","Dust & Scratches", "Noise"},
    {FilterKind::ReduceNoise,  "reduce-noise",  "Reduce Noise",  "Noise"},
    {FilterKind::Maximum,      "maximum",       "Maximum",       "Other"},   // s034 m3
    {FilterKind::Minimum,      "minimum",       "Minimum",       "Other"},
    {FilterKind::SurfaceBlur,  "surface-blur",  "Surface Blur",  "Blur"},    // s034 m4
    {FilterKind::LensBlur,     "lens-blur",     "Lens Blur",     "Blur"},    // s037
    {FilterKind::RadialBlur,   "radial-blur",   "Radial Blur",   "Blur"},    // s040
    {FilterKind::Twirl,        "twirl",         "Twirl",         "Distort"}, // s040 m4
    {FilterKind::Pinch,        "pinch",         "Pinch",         "Distort"},
    {FilterKind::Spherize,     "spherize",      "Spherize",      "Distort"},
};
// s040 m4: Distort -- a map from where each pixel takes its colour.
bool warps(FilterKind k) { return k == FilterKind::Twirl || k == FilterKind::Pinch || k == FilterKind::Spherize; }
const Info* info(FilterKind k) {
    for (const auto& i : kInfo)
        if (i.k == k) return &i;
    return nullptr;
}

// Reads its neighbours (all but Add Noise).
bool reads(FilterKind k) { return k != FilterKind::None && k != FilterKind::AddNoise; }
// Its result IS the worked buffer (a blur, the median) -- not a difference
// from it (Unsharp Mask, High Pass).
bool replaces(FilterKind k) {
    return k == FilterKind::GaussianBlur || k == FilterKind::BoxBlur || k == FilterKind::MotionBlur || k == FilterKind::Median ||
           k == FilterKind::Maximum || k == FilterKind::Minimum || k == FilterKind::SurfaceBlur || k == FilterKind::LensBlur ||
           k == FilterKind::RadialBlur || warps(k);   // s040
}
// Their worked buffer is a mean / rank of the neighbours -- colour may land above alpha (clamped).
bool ranked(FilterKind k) {
    return k == FilterKind::Median || k == FilterKind::Maximum || k == FilterKind::Minimum || k == FilterKind::SurfaceBlur ||
           k == FilterKind::LensBlur;   // s037: brightened highlights may go past 1
}
// s034: works its own result from the band's buffer (straight colour into
// `out`, alpha kept) -- Smart Sharpen, Dust & Scratches, Reduce Noise.
bool custom(FilterKind k) { return k == FilterKind::SmartSharpen || k == FilterKind::DustScratches || k == FilterKind::ReduceNoise; }
// The Gaussian under Gaussian Blur, Unsharp Mask and High Pass.
bool gaussian(FilterKind k) { return k == FilterKind::GaussianBlur || k == FilterKind::UnsharpMask || k == FilterKind::HighPass; }

// How a blur of sigma runs: an exact kernel (small sigma) or three boxes.
struct Plan {
    bool exact = true;
    int half = 0;                     // exact: the kernel's half width
    std::vector<float> k;             // exact: 2 * half + 1 weights
    std::array<int, 3> box{};         // boxes: each box's half width
    int reach = 0;
};

Plan plan_of(double sigma) {
    Plan p;
    if (sigma <= 0) return p;
    if (sigma < 3) {
        p.half = std::max(1, static_cast<int>(std::ceil(3 * sigma)));
        p.k.resize(static_cast<std::size_t>(2 * p.half + 1));
        double sum = 0;
        for (int i = -p.half; i <= p.half; ++i) sum += p.k[static_cast<std::size_t>(i + p.half)] = static_cast<float>(std::exp(-(i * i) / (2 * sigma * sigma)));
        for (auto& w : p.k) w = static_cast<float>(w / sum);
        p.reach = p.half;
        return p;
    }
    // Kovesi, "Fast almost-Gaussian filtering": three boxes, two widths.
    p.exact = false;
    constexpr int n = 3;
    const double ideal = std::sqrt(12 * sigma * sigma / n + 1);
    int wl = static_cast<int>(std::floor(ideal));
    if (wl % 2 == 0) --wl;
    const int wu = wl + 2;
    const int m = static_cast<int>(std::lround((12 * sigma * sigma - n * wl * wl - 4 * n * wl - 3 * n) / (-4.0 * wl - 4)));
    for (int i = 0; i < n; ++i) {
        p.box[static_cast<std::size_t>(i)] = ((i < m) ? wl : wu) / 2;
        p.reach += p.box[static_cast<std::size_t>(i)];
    }
    return p;
}

// One line, stride s, n samples, through tmp (n floats). Edges clamp.
void box_line(float* p, int n, int s, int h, std::vector<float>& tmp) {
    if (h <= 0 || n <= 0) return;
    const auto at = [&](int i) { return p[static_cast<std::size_t>(std::clamp(i, 0, n - 1)) * static_cast<std::size_t>(s)]; };
    double sum = 0;
    for (int i = -h; i <= h; ++i) sum += at(i);
    const double inv = 1.0 / (2 * h + 1);
    for (int i = 0; i < n; ++i) {
        tmp[static_cast<std::size_t>(i)] = static_cast<float>(sum * inv);
        sum += at(i + h + 1) - at(i - h);
    }
    for (int i = 0; i < n; ++i) p[static_cast<std::size_t>(i) * static_cast<std::size_t>(s)] = tmp[static_cast<std::size_t>(i)];
}

void kernel_line(float* p, int n, int s, const Plan& pl, std::vector<float>& tmp) {
    const auto at = [&](int i) { return p[static_cast<std::size_t>(std::clamp(i, 0, n - 1)) * static_cast<std::size_t>(s)]; };
    for (int i = 0; i < n; ++i) {
        float v = 0;
        for (int j = -pl.half; j <= pl.half; ++j) v += pl.k[static_cast<std::size_t>(j + pl.half)] * at(i + j);
        tmp[static_cast<std::size_t>(i)] = v;
    }
    for (int i = 0; i < n; ++i) p[static_cast<std::size_t>(i) * static_cast<std::size_t>(s)] = tmp[static_cast<std::size_t>(i)];
}

// A buffer of W x H pixels, C floats each, blurred in place: rows by line,
// columns row-wise (cache-friendly) through `out`.
void blur_buffer(std::vector<float>& buf, int W, int H, int C, const Plan& pl, const std::function<bool()>& stop) {
    const auto row_len = static_cast<std::size_t>(W) * static_cast<std::size_t>(C);
    std::vector<float> tmp(static_cast<std::size_t>(std::max(W, H)));
    // Horizontal.
    for (int y = 0; y < H; ++y) {
        if ((y & 31) == 0 && stop && stop()) return;
        float* r = buf.data() + static_cast<std::size_t>(y) * row_len;
        for (int c = 0; c < C; ++c) {
            if (pl.exact) kernel_line(r + c, W, C, pl, tmp);
            else for (const int h : pl.box) box_line(r + c, W, C, h, tmp);
        }
    }
    // Vertical, a row at a time.
    std::vector<float> out(buf.size());
    const auto row = [&](std::vector<float>& b, int y) { return b.data() + static_cast<std::size_t>(std::clamp(y, 0, H - 1)) * row_len; };
    if (pl.exact) {
        for (int y = 0; y < H; ++y) {
            if ((y & 31) == 0 && stop && stop()) return;
            float* o = out.data() + static_cast<std::size_t>(y) * row_len;
            std::fill(o, o + row_len, 0.f);
            for (int j = -pl.half; j <= pl.half; ++j) {
                const float w = pl.k[static_cast<std::size_t>(j + pl.half)];
                const float* in = row(buf, y + j);
                for (std::size_t i = 0; i < row_len; ++i) o[i] += w * in[i];
            }
        }
        buf.swap(out);
        return;
    }
    std::vector<double> sum(row_len);
    for (const int h : pl.box) {
        if (h <= 0) continue;
        std::fill(sum.begin(), sum.end(), 0.0);
        for (int j = -h; j <= h; ++j) {
            const float* in = row(buf, j);
            for (std::size_t i = 0; i < row_len; ++i) sum[i] += in[i];
        }
        const double inv = 1.0 / (2 * h + 1);
        for (int y = 0; y < H; ++y) {
            if ((y & 31) == 0 && stop && stop()) return;
            float* o = out.data() + static_cast<std::size_t>(y) * row_len;
            const float* add = row(buf, y + h + 1);
            const float* sub = row(buf, y - h);
            for (std::size_t i = 0; i < row_len; ++i) {
                o[i] = static_cast<float>(sum[i] * inv);
                sum[i] += static_cast<double>(add[i]) - sub[i];
            }
        }
        buf.swap(out);
    }
}

// Box Blur: one box each way, r either side.
Plan box_plan(int r) {
    Plan p;
    p.exact = false;
    p.box = {r, 0, 0};
    p.reach = r;
    return p;
}

// Motion Blur: the mean of d + 1 samples along a line of length d through
// each pixel, at `angle` degrees (counter-clockwise; image y runs down),
// sampled bilinearly. The offsets are the same for every pixel, so they are
// worked out once. Only the band's own pixels (`b`, buffer coordinates) are
// written, into `out`.
void motion(const std::vector<float>& buf, std::vector<float>& out, int W, int H, int C, const Rect& b, int d, int angle,
            const std::function<bool()>& stop) {
    struct S { int ox, oy; float wx, wy; };
    const double a = angle * M_PI / 180, dx = std::cos(a), dy = -std::sin(a);
    std::vector<S> taps;
    for (int i = 0; i <= d; ++i) {
        const double t = -d / 2.0 + i, x = t * dx, y = t * dy;
        const double fx = std::floor(x), fy = std::floor(y);
        taps.push_back({static_cast<int>(fx), static_cast<int>(fy), static_cast<float>(x - fx), static_cast<float>(y - fy)});
    }
    const float inv = 1.0f / static_cast<float>(taps.size());
    const auto at = [&](int x, int y) {
        return buf.data() + (static_cast<std::size_t>(std::clamp(y, 0, H - 1)) * static_cast<std::size_t>(W) + static_cast<std::size_t>(std::clamp(x, 0, W - 1))) * static_cast<std::size_t>(C);
    };
    std::array<float, pixel::kMaxChannels> acc{};
    for (int y = b.y; y < b.y + b.h; ++y) {
        if ((y & 15) == 0 && stop && stop()) return;
        for (int x = b.x; x < b.x + b.w; ++x) {
            acc.fill(0.f);
            for (const S& s : taps) {
                const int px = x + s.ox, py = y + s.oy;
                const float* p00 = at(px, py); const float* p10 = at(px + 1, py);
                const float* p01 = at(px, py + 1); const float* p11 = at(px + 1, py + 1);
                const float w00 = (1 - s.wx) * (1 - s.wy), w10 = s.wx * (1 - s.wy), w01 = (1 - s.wx) * s.wy, w11 = s.wx * s.wy;
                for (int c = 0; c < C; ++c) acc[static_cast<std::size_t>(c)] += w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c];
            }
            float* o = out.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(C);
            for (int c = 0; c < C; ++c) o[c] = acc[static_cast<std::size_t>(c)] * inv;
        }
    }
}

// Median: each channel's median over the (2r + 1)^2 square (edges clamp), by a
// histogram of `bins` levels slid along the row (Huang): a step costs two
// columns, not the square. Only the band's own pixels are written.
void median(const std::vector<float>& buf, std::vector<float>& out, int W, int H, int C, const Rect& b, int r, int bins,
            const std::function<bool()>& stop) {
    const auto top = static_cast<float>(bins - 1);
    std::vector<std::uint16_t> q(buf.size());
    for (std::size_t i = 0; i < buf.size(); ++i)
        q[i] = static_cast<std::uint16_t>(std::clamp(static_cast<int>(buf[i] * top + 0.5f), 0, bins - 1));
    const auto Q = [&](int x, int y, int c) {
        return q[(static_cast<std::size_t>(std::clamp(y, 0, H - 1)) * static_cast<std::size_t>(W) + static_cast<std::size_t>(std::clamp(x, 0, W - 1))) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)];
    };
    const int n = (2 * r + 1) * (2 * r + 1), half = n / 2;
    std::vector<int> h(static_cast<std::size_t>(bins));
    for (int y = b.y; y < b.y + b.h; ++y) {
        if ((y & 7) == 0 && stop && stop()) return;
        for (int c = 0; c < C; ++c) {
            std::fill(h.begin(), h.end(), 0);
            for (int j = -r; j <= r; ++j)
                for (int i = -r; i <= r; ++i) ++h[Q(b.x + i, y + j, c)];
            int m = 0, lt = 0;
            while (lt + h[static_cast<std::size_t>(m)] <= half) lt += h[static_cast<std::size_t>(m++)];
            for (int x = b.x;; ) {
                out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] =
                    static_cast<float>(m) / top;
                if (++x >= b.x + b.w) break;
                for (int j = -r; j <= r; ++j) {   // the column leaving, the column arriving
                    const int gone = Q(x - r - 1, y + j, c), come = Q(x + r, y + j, c);
                    --h[static_cast<std::size_t>(gone)];
                    if (gone < m) --lt;
                    ++h[static_cast<std::size_t>(come)];
                    if (come < m) ++lt;
                }
                while (lt > half) lt -= h[static_cast<std::size_t>(--m)];
                while (lt + h[static_cast<std::size_t>(m)] <= half) lt += h[static_cast<std::size_t>(m++)];
            }
        }
    }
}

// ---- s034: Smart Sharpen, Dust & Scratches, Reduce Noise -------------------

float luma_of(const float* p, int cc) { return cc >= 3 ? 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] : p[0]; }

// Lens: the mean over a DISC of `radius` (+ half a pixel) around each of the
// band's own pixels (`b`), from row prefix sums: a pixel costs 2r + 1 rows.
// Edges clamp, as the other blurs.
void disc(const std::vector<float>& buf, std::vector<float>& out, int W, int H, int C, const Rect& b, double radius,
          const std::function<bool()>& stop) {
    const double rr = radius + 0.5;
    const int r = static_cast<int>(std::floor(rr));
    std::vector<int> span(static_cast<std::size_t>(2 * r + 1));
    double count = 0;
    for (int dy = -r; dy <= r; ++dy) {
        const int w = static_cast<int>(std::floor(std::sqrt(std::max(0.0, rr * rr - dy * dy))));
        span[static_cast<std::size_t>(dy + r)] = w;
        count += 2 * w + 1;
    }
    const auto row_n = static_cast<std::size_t>(W + 1) * static_cast<std::size_t>(C);
    // Only the rows the own pixels read.
    const int y0 = std::max(0, b.y - r), y1 = std::min(H - 1, b.y + b.h - 1 + r);
    std::vector<double> pre(static_cast<std::size_t>(y1 - y0 + 1) * row_n, 0.0);
    for (int y = y0; y <= y1; ++y) {
        double* P = pre.data() + static_cast<std::size_t>(y - y0) * row_n;
        const float* in = buf.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(W) * static_cast<std::size_t>(C);
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < C; ++c)
                P[static_cast<std::size_t>(x + 1) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] =
                    P[static_cast<std::size_t>(x) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] + in[static_cast<std::size_t>(x) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)];
    }
    const double inv = 1.0 / count;
    std::array<double, pixel::kMaxChannels> acc{};
    for (int y = b.y; y < b.y + b.h; ++y) {
        if ((y & 15) == 0 && stop && stop()) return;
        for (int x = b.x; x < b.x + b.w; ++x) {
            acc.fill(0.0);
            for (int dy = -r; dy <= r; ++dy) {
                const int yy = std::clamp(y + dy, 0, H - 1);
                const double* P = pre.data() + static_cast<std::size_t>(yy - y0) * row_n;
                const float* in = buf.data() + static_cast<std::size_t>(yy) * static_cast<std::size_t>(W) * static_cast<std::size_t>(C);
                const int w = span[static_cast<std::size_t>(dy + r)];
                const int x0 = x - w, x1 = x + w;
                const int a = std::max(x0, 0), z = std::min(x1, W - 1);
                const int lo = std::max(0, -x0), hi = std::max(0, x1 - (W - 1));   // samples beyond each edge
                for (int c = 0; c < C; ++c) {
                    double v = P[static_cast<std::size_t>(z + 1) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] -
                               P[static_cast<std::size_t>(a) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)];
                    if (lo) v += lo * static_cast<double>(in[static_cast<std::size_t>(c)]);
                    if (hi) v += hi * static_cast<double>(in[static_cast<std::size_t>(W - 1) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)]);
                    acc[static_cast<std::size_t>(c)] += v;
                }
            }
            float* o = out.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(C);
            for (int c = 0; c < C; ++c) o[c] = static_cast<float>(acc[static_cast<std::size_t>(c)] * inv);
        }
    }
}

int motion_distance(const Filter& f) { return std::max(1, static_cast<int>(std::lround(f.radius * 2))); }

// The luminance and alpha of `buf` (premultiplied), as 2 planes, blurred by
// sigma: the tone the shadows / highlights fade reads.
std::vector<float> tone(const std::vector<float>& buf, int W, int H, int C, int cc, bool has_a, double sigma,
                        const std::function<bool()>& stop) {
    std::vector<float> t(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * 2);
    for (std::size_t i = 0, n = static_cast<std::size_t>(W) * static_cast<std::size_t>(H); i < n; ++i) {
        const float* p = buf.data() + i * static_cast<std::size_t>(C);
        t[2 * i] = luma_of(p, cc);
        t[2 * i + 1] = has_a ? p[cc] : 1.f;
    }
    blur_buffer(t, W, H, 2, plan_of(sigma), stop);
    return t;
}

float smooth(float x) { x = std::clamp(x, 0.f, 1.f); return x * x * (3 - 2 * x); }

// Smart Sharpen: v + amount * (v - K v) with K the blur it removes (Gaussian,
// a lens's disc, a motion streak); the difference cored where it is no bigger
// than the noise (Reduce Noise), and less of it in the shadows / highlights.
void smart(const std::vector<float>& buf, std::vector<float>& out, int W, int H, int C, int cc, bool has_a, const Rect& b,
           const Filter& f, const std::function<bool()>& stop) {
    std::vector<float> k;
    if (f.remove == Remove::Gaussian) {
        k = buf;
        blur_buffer(k, W, H, C, plan_of(f.radius), stop);
    } else {
        k.assign(buf.size(), 0.f);
        if (f.remove == Remove::Lens) disc(buf, k, W, H, C, b, f.radius, stop);
        else motion(buf, k, W, H, C, b, motion_distance(f), f.angle, stop);
    }
    if (stop && stop()) return;
    std::vector<float> ts, tl;
    if (f.shadow_fade > 0) ts = tone(buf, W, H, C, cc, has_a, f.shadow_radius, stop);
    if (f.light_fade > 0) tl = tone(buf, W, H, C, cc, has_a, f.light_radius, stop);
    if (stop && stop()) return;
    const float amt = static_cast<float>(f.amount / 100);
    const float t = static_cast<float>(f.noise / 100 * 0.06), t2 = t * t;
    const auto level = [](const std::vector<float>& tp, std::size_t i) { return tp[2 * i + 1] > 1e-6f ? tp[2 * i] / tp[2 * i + 1] : 0.f; };
    std::array<float, pixel::kMaxChannels> d{};
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
            const float* p = buf.data() + i * static_cast<std::size_t>(C);
            const float* q = k.data() + i * static_cast<std::size_t>(C);
            float* o = out.data() + i * static_cast<std::size_t>(C);
            const float a = has_a ? p[cc] : 1.f, ka = has_a ? q[cc] : 1.f;
            if (a <= 1e-6f) continue;
            for (int c = 0; c < cc; ++c) {
                const float s = p[c] / a;
                o[c] = s;
                d[static_cast<std::size_t>(c)] = s - (ka > 1e-6f ? q[c] / ka : s);
            }
            float g = amt;
            if (t2 > 0) {
                const float m = luma_of(d.data(), cc), m2 = m * m;
                g *= m2 / (m2 + t2);
            }
            if (!ts.empty()) {
                const float tw = static_cast<float>(f.shadow_tonal / 100);
                const float w = tw > 0 ? smooth(1 - level(ts, i) / tw) : 0.f;
                g *= 1 - static_cast<float>(f.shadow_fade / 100) * w;
            }
            if (!tl.empty()) {
                const float tw = static_cast<float>(f.light_tonal / 100);
                const float w = tw > 0 ? smooth(1 - (1 - level(tl, i)) / tw) : 0.f;
                g *= 1 - static_cast<float>(f.light_fade / 100) * w;
            }
            for (int c = 0; c < cc; ++c) o[c] += g * d[static_cast<std::size_t>(c)];
        }
}

// Dust & Scratches: the median of the square, where a pixel differs from it
// by more than the threshold (0: everywhere -- the Median).
void dust(const std::vector<float>& buf, std::vector<float>& out, int W, int H, int C, int cc, bool has_a, const Rect& b,
          const Filter& f, int bins, bool clamp01, const std::function<bool()>& stop) {
    std::vector<float> m(buf.size());
    median(buf, m, W, H, C, b, static_cast<int>(f.radius), bins, stop);
    if (stop && stop()) return;
    const float th = static_cast<float>(f.threshold) / 255.f;
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(C);
            const float a = has_a ? buf[i + static_cast<std::size_t>(cc)] : 1.f;
            const float ma = has_a ? m[i + static_cast<std::size_t>(cc)] : 1.f;
            if (a <= 1e-6f) continue;
            for (int c = 0; c < cc; ++c) {
                const float s = buf[i + static_cast<std::size_t>(c)] / a;
                float v = ma > 1e-6f ? m[i + static_cast<std::size_t>(c)] / ma : s;
                if (clamp01) v = std::min(v, 1.f);
                out[i + static_cast<std::size_t>(c)] = std::abs(s - v) > th ? v : s;
            }
        }
}

// Reduce Noise: luminance through a guided filter (He et al.: edge-keeping,
// box sums only) -- Strength the noise it takes for flat, Preserve Details
// lowering that; colour noise: the chroma blurred; Sharpen Details: an
// unsharp mask on the cleaned luminance. All premultiplied, back to straight
// at the end.
int rn_box(const Filter& f) { return 2 + static_cast<int>(std::lround(f.amount / 3)); }
double rn_chroma(const Filter& f) { return f.colour / 100 * 8; }

void reduce(const std::vector<float>& buf, std::vector<float>& out, int W, int H, int C, int cc, bool has_a, const Rect& b,
            const Filter& f, const std::function<bool()>& stop) {
    const auto N = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
    const auto Cz = static_cast<std::size_t>(C);
    std::vector<float> Y(N), ch;
    const bool colour = cc >= 3;
    if (colour) ch.resize(N * 2);
    for (std::size_t i = 0; i < N; ++i) {
        const float* p = buf.data() + i * Cz;
        Y[i] = luma_of(p, cc);
        if (colour) { ch[2 * i] = (p[2] - Y[i]) * 0.564f; ch[2 * i + 1] = (p[0] - Y[i]) * 0.713f; }
    }
    const double sig = f.amount / 10 * 0.12 * (1 - 0.75 * f.preserve / 100);
    if (sig > 0) {
        const float eps = static_cast<float>(sig * sig);
        const Plan box = box_plan(rn_box(f));
        std::vector<float> mi = Y, mii(N);
        for (std::size_t i = 0; i < N; ++i) mii[i] = Y[i] * Y[i];
        blur_buffer(mi, W, H, 1, box, stop);
        blur_buffer(mii, W, H, 1, box, stop);
        if (stop && stop()) return;
        std::vector<float> A(N), B(N);
        for (std::size_t i = 0; i < N; ++i) {
            const float var = std::max(0.f, mii[i] - mi[i] * mi[i]);
            A[i] = var / (var + eps);
            B[i] = mi[i] - A[i] * mi[i];
        }
        blur_buffer(A, W, H, 1, box, stop);
        blur_buffer(B, W, H, 1, box, stop);
        if (stop && stop()) return;
        for (std::size_t i = 0; i < N; ++i) Y[i] = A[i] * Y[i] + B[i];
    }
    if (f.sharpen > 0) {
        std::vector<float> g = Y;
        blur_buffer(g, W, H, 1, plan_of(1.0), stop);
        const float s = static_cast<float>(f.sharpen / 100 * 1.5);
        for (std::size_t i = 0; i < N; ++i) Y[i] += s * (Y[i] - g[i]);
    }
    if (colour && rn_chroma(f) > 0) blur_buffer(ch, W, H, 2, plan_of(rn_chroma(f)), stop);
    if (stop && stop()) return;
    for (int y = b.y; y < b.y + b.h; ++y)
        for (int x = b.x; x < b.x + b.w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
            const float a = has_a ? buf[i * Cz + static_cast<std::size_t>(cc)] : 1.f;
            if (a <= 1e-6f) continue;
            float* o = out.data() + i * Cz;
            const float l = Y[i] / a;
            if (!colour) { o[0] = l; continue; }
            const float cb = ch[2 * i] / a, cr = ch[2 * i + 1] / a;
            // The exact inverse of the forward transform (all at 0: no change).
            o[0] = l + cr / 0.713f;
            o[2] = l + cb / 0.564f;
            o[1] = (l - 0.299f * o[0] - 0.114f * o[2]) / 0.587f;
        }
}

// ---- s034 m3: Maximum / Minimum ---------------------------------------------

// The largest (smallest) of each window of 2r + 1 along a line of n samples
// (stride `si` in, `so` out; edges clamp) -- van Herk / Gil-Werman: blocks of
// the window's length, a running extreme forward and back in each, so a
// sample costs three comparisons whatever r.
void extreme_line(const float* in, int si, float* out, int so, int n, int r, bool mx, std::vector<float>& e,
                  std::vector<float>& g, std::vector<float>& h) {
    const int k = 2 * r + 1, m = n + 2 * r;
    e.resize(static_cast<std::size_t>(m));
    g.resize(static_cast<std::size_t>(m));
    h.resize(static_cast<std::size_t>(m));
    for (int i = 0; i < m; ++i) e[static_cast<std::size_t>(i)] = in[static_cast<std::size_t>(std::clamp(i - r, 0, n - 1)) * static_cast<std::size_t>(si)];
    const auto pick = [mx](float a, float b) { return mx ? std::max(a, b) : std::min(a, b); };
    for (int i = 0; i < m; ++i)
        g[static_cast<std::size_t>(i)] = (i % k == 0) ? e[static_cast<std::size_t>(i)] : pick(g[static_cast<std::size_t>(i - 1)], e[static_cast<std::size_t>(i)]);
    for (int i = m - 1; i >= 0; --i)
        h[static_cast<std::size_t>(i)] = (i == m - 1 || (i + 1) % k == 0) ? e[static_cast<std::size_t>(i)] : pick(h[static_cast<std::size_t>(i + 1)], e[static_cast<std::size_t>(i)]);
    // The window of out[i] is e[i .. i + k - 1].
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t>(i) * static_cast<std::size_t>(so)] = pick(h[static_cast<std::size_t>(i)], g[static_cast<std::size_t>(i + k - 1)]);
}

// Maximum / Minimum of each channel over a square (separable: rows, then
// columns) or a disc (each row of the disc a window of its own half width,
// so a pixel costs 2r + 1 windows). Only the band's own pixels are written.
void extreme(const std::vector<float>& buf, std::vector<float>& out, int W, int H, int C, const Rect& b, int r, bool round, bool mx,
             const std::function<bool()>& stop) {
    std::vector<float> e, g, h;
    const auto Cz = static_cast<std::size_t>(C);
    const auto ix = [&](int x, int y) { return (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * Cz; };
    if (!round) {
        // Rows (only those the own pixels' columns read), then the own columns.
        std::vector<float> rows(buf.size());
        const int y0 = std::max(0, b.y - r), y1 = std::min(H - 1, b.y + b.h - 1 + r);
        for (int y = y0; y <= y1; ++y) {
            if ((y & 31) == 0 && stop && stop()) return;
            for (int c = 0; c < C; ++c) extreme_line(buf.data() + ix(0, y) + c, C, rows.data() + ix(0, y) + c, C, W, r, mx, e, g, h);
        }
        std::vector<float> col(static_cast<std::size_t>(H));
        for (int x = b.x; x < b.x + b.w; ++x) {
            if ((x & 31) == 0 && stop && stop()) return;
            for (int c = 0; c < C; ++c) {
                extreme_line(rows.data() + ix(x, 0) + c, W * C, col.data(), 1, H, r, mx, e, g, h);
                for (int y = b.y; y < b.y + b.h; ++y) out[ix(x, y) + static_cast<std::size_t>(c)] = col[static_cast<std::size_t>(y)];
            }
        }
        return;
    }
    const double rr = r + 0.5;
    std::vector<float> line(static_cast<std::size_t>(W));
    for (int y = b.y; y < b.y + b.h; ++y) {
        if (stop && stop()) return;
        float* o = out.data() + ix(0, y);
        bool first = true;
        for (int dy = -r; dy <= r; ++dy) {
            const int w = static_cast<int>(std::floor(std::sqrt(std::max(0.0, rr * rr - dy * dy))));
            const int yy = std::clamp(y + dy, 0, H - 1);
            for (int c = 0; c < C; ++c) {
                extreme_line(buf.data() + ix(0, yy) + c, C, line.data(), 1, W, w, mx, e, g, h);
                for (int x = b.x; x < b.x + b.w; ++x) {
                    float& v = o[static_cast<std::size_t>(x) * Cz + static_cast<std::size_t>(c)];
                    const float n = line[static_cast<std::size_t>(x)];
                    v = first ? n : (mx ? std::max(v, n) : std::min(v, n));
                }
            }
            first = false;
        }
    }
}

// ---- s034 m4: Surface Blur ----------------------------------------------------

// Each channel's mean over the (2r + 1)^2 square, each neighbour weighted by
// how close it is to the pixel: 1 - |difference| / (2.5 x threshold), none
// beyond (Photoshop's Surface Blur). Flat areas smooth, edges -- differences
// past the threshold -- don't mix. The same sliding histogram as the Median:
// a step costs two columns plus the bins within reach of the pixel's value.
void surface(const std::vector<float>& buf, std::vector<float>& out, int W, int H, int C, const Rect& b, int r, int threshold,
             int bins, const std::function<bool()>& stop) {
    const auto top = static_cast<float>(bins - 1);
    std::vector<std::uint16_t> q(buf.size());
    for (std::size_t i = 0; i < buf.size(); ++i)
        q[i] = static_cast<std::uint16_t>(std::clamp(static_cast<int>(buf[i] * top + 0.5f), 0, bins - 1));
    const auto Q = [&](int x, int y, int c) {
        return q[(static_cast<std::size_t>(std::clamp(y, 0, H - 1)) * static_cast<std::size_t>(W) + static_cast<std::size_t>(std::clamp(x, 0, W - 1))) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)];
    };
    // The weights by distance in bins.
    const double reach_bins = 2.5 * threshold / 255.0 * (bins - 1);
    const int span = std::max(1, static_cast<int>(std::ceil(reach_bins)) - 1);
    std::vector<float> wt(static_cast<std::size_t>(span + 1));
    for (int d = 0; d <= span; ++d) wt[static_cast<std::size_t>(d)] = static_cast<float>(std::max(0.0, 1 - d / reach_bins));
    // A channel the same everywhere in the buffer (alpha on an opaque photo)
    // comes out as it went in.
    std::vector<bool> flat(static_cast<std::size_t>(C), true);
    for (std::size_t i = static_cast<std::size_t>(C); i < q.size(); ++i)
        if (q[i] != q[i % static_cast<std::size_t>(C)]) flat[i % static_cast<std::size_t>(C)] = false;
    std::vector<int> h(static_cast<std::size_t>(bins));
    for (int y = b.y; y < b.y + b.h; ++y) {
        if ((y & 7) == 0 && stop && stop()) return;
        for (int c = 0; c < C; ++c) {
            if (flat[static_cast<std::size_t>(c)]) {
                for (int x = b.x; x < b.x + b.w; ++x)
                    out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] =
                        buf[static_cast<std::size_t>(c)];
                continue;
            }
            std::fill(h.begin(), h.end(), 0);
            for (int j = -r; j <= r; ++j)
                for (int i = -r; i <= r; ++i) ++h[Q(b.x + i, y + j, c)];
            for (int x = b.x;; ) {
                const int v = Q(x, y, c);
                const int lo = std::max(0, v - span), hi = std::min(bins - 1, v + span);
                double sum = 0, den = 0;
                for (int k = lo; k <= hi; ++k) {
                    const int n = h[static_cast<std::size_t>(k)];
                    if (!n) continue;
                    const double w = n * static_cast<double>(wt[static_cast<std::size_t>(std::abs(k - v))]);
                    sum += w * k;
                    den += w;
                }
                out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] =
                    static_cast<float>(den > 0 ? sum / den / top : v / top);
                if (++x >= b.x + b.w) break;
                for (int j = -r; j <= r; ++j) {
                    --h[Q(x - r - 1, y + j, c)];
                    ++h[Q(x + r, y + j, c)];
                }
            }
        }
    }
}

// s037: LENS BLUR over the band's own pixels. Highlights first: a pixel whose
// lightness passes the threshold is brightened (premultiplied colour scaled,
// more the further past it), so after the blur a light is a bright shape of
// the iris, not a faint smear. Then the mean over the iris around each pixel,
// row spans from prefix sums (the ends weighted by how much of the end pixel
// the span covers) -- a pixel costs 2r + 1 rows, as the disc.
// (A brightened light may pass 1: the write-out clamps it -- ranked().)
// Each iris row's whole pixels [a, z] and its two part pixels (weights wl, wr).
struct LensSpan { int a, z; double wl, wr; };
// s039: one radius's iris as spans, and 1 / its area.
struct LensLevel { int r = 0; std::vector<LensSpan> sp; double inv = 1; };
LensLevel lens_level(const std::vector<IrisRow>& rows) {
    LensLevel lv;
    lv.r = static_cast<int>(rows.size() / 2);
    lv.sp.resize(rows.size());
    double count = 0;
    for (std::size_t k = 0; k < rows.size(); ++k) {
        const double lo = rows[k].lo - 0.5, hi = rows[k].hi + 0.5;   // pixel j covers [j - .5, j + .5]
        LensSpan s{0, -1, 0, 0};
        if (hi > lo) {
            s.a = static_cast<int>(std::ceil(lo + 0.5));     // first pixel wholly inside
            s.z = static_cast<int>(std::floor(hi - 0.5));    // last
            if (s.a > s.z) { s.a = 0; s.z = -1; s.wl = hi - lo; }   // within one pixel: a part of it
            else { s.wl = (s.a - 0.5) - lo; s.wr = hi - (s.z + 0.5); }
        }
        count += (s.z - s.a + 1) + s.wl + s.wr;
        lv.sp[k] = s;
    }
    lv.inv = count > 0 ? 1.0 / count : 1.0;
    return lv;
}
// s039: a DEPTH MAP's irises -- a level every quarter pixel of radius, 0 (sharp) .. the full one.
constexpr int kLensSteps = 4;
std::vector<LensLevel> lens_levels(const Filter& f) {
    const int n = static_cast<int>(std::ceil(f.radius * kLensSteps));
    std::vector<LensLevel> v(static_cast<std::size_t>(n + 1));
    for (int k = 1; k <= n; ++k)
        v[static_cast<std::size_t>(k)] = lens_level(iris(static_cast<double>(k) / kLensSteps, static_cast<int>(f.blades), f.curvature / 100, f.angle));
    return v;
}

// `amt` (s039, nullptr: 1 everywhere) -- each of the band's own pixels' share
// of the full radius, from the depth map; `levels` its irises. A pixel at 0
// keeps its own value (not the brightened one).
void lens(const std::vector<float>& buf, std::vector<float>& out, int W, int H, int C, int cc, bool has_a, const Rect& b,
          const Filter& f, const std::function<bool()>& stop, const float* amt = nullptr,
          const std::vector<LensLevel>* levels = nullptr) {
    const auto rows = iris(f.radius, static_cast<int>(f.blades), f.curvature / 100, f.angle);
    const int r = static_cast<int>(rows.size() / 2);
    // The highlights.
    std::vector<float> lit;
    const std::vector<float>* src = &buf;
    if (f.bright > 0 && f.threshold < 255) {
        lit = buf;
        // The gain grows exponentially, to 40x at 100 %: a clipped highlight
        // stands for a far brighter light, and a small one must fill an iris
        // many times its size (seen under Xvfb: 8x left small lights dim).
        const float t = static_cast<float>(f.threshold) / 255.f, gain = static_cast<float>(std::pow(41.0, f.bright / 100) - 1);
        for (std::size_t i = 0; i < lit.size(); i += static_cast<std::size_t>(C)) {
            const float a = has_a ? lit[i + static_cast<std::size_t>(cc)] : 1.f;
            if (a <= 1e-6f) continue;
            float l = luma_of(&lit[i], cc) / a;
            if (l <= t) continue;
            const float k = smooth((l - t) / std::max(1e-3f, 1 - t));
            for (int c = 0; c < cc; ++c) lit[i + static_cast<std::size_t>(c)] *= 1 + gain * k;
        }
        src = &lit;
    }
    const auto row_n = static_cast<std::size_t>(W + 1) * static_cast<std::size_t>(C);
    const int y0 = std::max(0, b.y - r), y1 = std::min(H - 1, b.y + b.h - 1 + r);
    std::vector<double> pre(static_cast<std::size_t>(y1 - y0 + 1) * row_n, 0.0);
    for (int y = y0; y <= y1; ++y) {
        double* P = pre.data() + static_cast<std::size_t>(y - y0) * row_n;
        const float* in = src->data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(W) * static_cast<std::size_t>(C);
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < C; ++c)
                P[static_cast<std::size_t>(x + 1) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] =
                    P[static_cast<std::size_t>(x) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] + in[static_cast<std::size_t>(x) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)];
    }
    using Span = LensSpan;
    const LensLevel full = lens_level(rows);
    const std::vector<Span>& sp = full.sp;
    const double inv = full.inv;
    // A pixel with its edges clamped.
    const auto at = [&](int x, int yy, int c) {
        return static_cast<double>((*src)[(static_cast<std::size_t>(yy) * static_cast<std::size_t>(W) + static_cast<std::size_t>(std::clamp(x, 0, W - 1))) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)]);
    };
    // A row of sums at a time: for each of the iris's rows, every pixel of
    // the band's row adds that row's span -- clamp-free where the span and its
    // part pixels lie inside the picture (most of it), the channel count a
    // template constant there.
    std::vector<double> acc(static_cast<std::size_t>(b.w) * static_cast<std::size_t>(C));
    const auto slow = [&](double* A, int x, const Span& s, const double* P, int yy) {
        for (int c = 0; c < C; ++c) {
            double v = 0;
            if (s.z >= s.a) {
                const int x0 = x + s.a, x1 = x + s.z;
                const int a = std::max(x0, 0), z = std::min(x1, W - 1);
                if (z >= a) v = P[static_cast<std::size_t>(z + 1) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)] - P[static_cast<std::size_t>(a) * static_cast<std::size_t>(C) + static_cast<std::size_t>(c)];
                if (x0 < 0) v += std::min(-x0, x1 - x0 + 1) * at(0, yy, c);
                if (x1 > W - 1) v += std::min(x1 - (W - 1), x1 - x0 + 1) * at(W - 1, yy, c);
                if (s.wl > 0) v += s.wl * at(x0 - 1, yy, c);
                if (s.wr > 0) v += s.wr * at(x1 + 1, yy, c);
            } else if (s.wl > 0) {
                v = s.wl * at(x, yy, c);
            }
            A[c] += v;
        }
    };
    const auto fast = [&]<int CC>(double* A, int xa, int xz, const Span& s, const double* P, const float* in) {
        const double wl = s.wl, wr = s.wr;
        for (int x = xa; x <= xz; ++x, A += CC) {
            const double* Pa = P + static_cast<std::size_t>(x + s.a) * CC;
            const double* Pz = P + static_cast<std::size_t>(x + s.z + 1) * CC;
            const float* Lp = in + static_cast<std::ptrdiff_t>(x + s.a - 1) * CC;
            const float* Rp = in + static_cast<std::size_t>(x + s.z + 1) * CC;
            for (int c = 0; c < CC; ++c) A[c] += Pz[c] - Pa[c] + wl * Lp[c] + wr * Rp[c];
        }
    };
    if (amt && levels && !levels->empty()) {
        // s039 -- A DEPTH MAP: each pixel its own iris (the level nearest its
        // share of the radius), its rows from the same prefix sums.
        const int top = static_cast<int>(levels->size()) - 1;
        for (int y = b.y; y < b.y + b.h; ++y) {
            if ((y & 7) == 0 && stop && stop()) return;
            for (int x = b.x; x < b.x + b.w; ++x) {
                const float t = amt[static_cast<std::size_t>(y - b.y) * static_cast<std::size_t>(b.w) + static_cast<std::size_t>(x - b.x)];
                const int k = std::clamp(static_cast<int>(std::lround(t * f.radius * kLensSteps)), 0, top);
                float* o = out.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(C);
                const float* own = buf.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * static_cast<std::size_t>(C);
                if (k == 0) { std::copy(own, own + C, o); continue; }
                const LensLevel& lv = (*levels)[static_cast<std::size_t>(k)];
                double A[4] = {0, 0, 0, 0};
                for (int dy = -lv.r; dy <= lv.r; ++dy) {
                    const Span& s = lv.sp[static_cast<std::size_t>(dy + lv.r)];
                    const int yy = std::clamp(y + dy, 0, H - 1);
                    const double* P = pre.data() + static_cast<std::size_t>(yy - y0) * row_n;
                    if (s.z >= s.a && x + s.a - 1 >= 0 && x + s.z + 1 <= W - 1) {
                        const float* in = src->data() + static_cast<std::size_t>(yy) * static_cast<std::size_t>(W) * static_cast<std::size_t>(C);
                        const double* Pa = P + static_cast<std::size_t>(x + s.a) * static_cast<std::size_t>(C);
                        const double* Pz = P + static_cast<std::size_t>(x + s.z + 1) * static_cast<std::size_t>(C);
                        const float* Lp = in + static_cast<std::ptrdiff_t>(x + s.a - 1) * C;
                        const float* Rp = in + static_cast<std::size_t>(x + s.z + 1) * static_cast<std::size_t>(C);
                        for (int c = 0; c < C; ++c) A[c] += Pz[c] - Pa[c] + s.wl * Lp[c] + s.wr * Rp[c];
                    } else {
                        slow(A, x, s, P, yy);
                    }
                }
                for (int c = 0; c < C; ++c) o[c] = static_cast<float>(A[c] * lv.inv);
            }
        }
        return;
    }
    for (int y = b.y; y < b.y + b.h; ++y) {
        if ((y & 7) == 0 && stop && stop()) return;
        std::fill(acc.begin(), acc.end(), 0.0);
        for (int dy = -r; dy <= r; ++dy) {
            const Span& s = sp[static_cast<std::size_t>(dy + r)];
            const int yy = std::clamp(y + dy, 0, H - 1);
            const double* P = pre.data() + static_cast<std::size_t>(yy - y0) * row_n;
            const float* in = src->data() + static_cast<std::size_t>(yy) * static_cast<std::size_t>(W) * static_cast<std::size_t>(C);
            // The pixels whose span (and its part pixels) is wholly inside.
            int xa = b.x + b.w, xz = b.x - 1;
            if (s.z >= s.a) { xa = std::max(b.x, 1 - s.a); xz = std::min(b.x + b.w - 1, W - 2 - s.z); }
            for (int x = b.x; x < b.x + b.w; ++x) {
                if (x == xa && xz >= xa) {
                    double* A = acc.data() + static_cast<std::size_t>(x - b.x) * static_cast<std::size_t>(C);
                    switch (C) {
                        case 4: fast.template operator()<4>(A, xa, xz, s, P, in); break;
                        case 3: fast.template operator()<3>(A, xa, xz, s, P, in); break;
                        case 2: fast.template operator()<2>(A, xa, xz, s, P, in); break;
                        default: fast.template operator()<1>(A, xa, xz, s, P, in); break;
                    }
                    x = xz;
                    continue;
                }
                slow(acc.data() + static_cast<std::size_t>(x - b.x) * static_cast<std::size_t>(C), x, s, P, yy);
            }
        }
        float* o = out.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(b.x)) * static_cast<std::size_t>(C);
        for (std::size_t i = 0; i < acc.size(); ++i) o[i] = static_cast<float>(acc[i] * inv);
    }
}

// s040: the box a band's streaks read -- the band swept round the centre by
// the arc (its outline's points at 9 angles, plus the arcs' bulge between
// them), or scaled about it from the least to the most (the corners).
Rect radial_box(const Rect& b, const Filter& f, double ccx, double ccy) {
    constexpr double kPi = 3.14159265358979323846;
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    const auto add = [&](double x, double y) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); };
    std::vector<std::array<double, 2>> pts;
    const auto edge = [&](double ax, double ay, double bx, double by) {
        const int n = std::max(1, static_cast<int>(std::hypot(bx - ax, by - ay) / 16));
        for (int i = 0; i <= n; ++i) pts.push_back({ax + (bx - ax) * i / n, ay + (by - ay) * i / n});
    };
    const double L = b.x, T = b.y, Rr = b.x + b.w, B = b.y + b.h;
    edge(L, T, Rr, T); edge(Rr, T, Rr, B); edge(Rr, B, L, B); edge(L, B, L, T);
    double far = 0;
    for (const auto& p : pts) far = std::max(far, std::hypot(p[0] - ccx, p[1] - ccy));
    double pad = 3;
    if (f.zoom) {
        const double span = f.amount / 100 * 0.5;
        for (const double s : {1 - span / 2, 1.0, 1 + span / 2})
            for (const auto& p : pts) add(ccx + (p[0] - ccx) * s, ccy + (p[1] - ccy) * s);
    } else {
        const double arc = f.amount * 0.5 * kPi / 180;
        for (int k = 0; k <= 8; ++k) {
            const double a = arc * (k / 8.0 - 0.5), c = std::cos(a), s = std::sin(a);
            for (const auto& p : pts) {
                const double vx = p[0] - ccx, vy = p[1] - ccy;
                add(ccx + vx * c - vy * s, ccy + vx * s + vy * c);
            }
        }
        pad += far * (1 - std::cos(arc / 16));   // an arc bulges past its chord by this, at most
        // the edge sampling (16 px) can miss a corner's sweep by the step
    }
    pad += 16;
    return Rect{static_cast<int>(std::floor(x0 - pad)), static_cast<int>(std::floor(y0 - pad)),
                static_cast<int>(std::ceil(x1 - x0 + 2 * pad)) + 1, static_cast<int>(std::ceil(y1 - y0 + 2 * pad)) + 1};
}

// s040: RADIAL BLUR, by DOUBLING. A box of 2^m taps along the streak is m
// passes of a two-tap mean -- each pixel the mean of the picture turned (or
// scaled) by +a and by -a about the centre, a halving each pass: rotations
// (and scalings) about one centre compose, so the taps add up to an even
// comb over the arc. A pass costs two bilinear taps a pixel, whatever the
// streak's length. `cur` is the region (ox, oy, W x H) of premultiplied
// colour; it is replaced by the result. False: cancelled.
bool radial_run(std::vector<float>& cur, int W, int H, int C, int ox, int oy, const Filter& f, double ccx, double ccy, int m,
                const std::function<bool()>& stop) {
    constexpr double kPi = 3.14159265358979323846;
    if (m <= 0) return true;
    std::vector<float> nxt(cur.size());
    const double A = f.zoom ? f.amount / 100 * 0.5 / 2 : f.amount * 0.5 * kPi / 180 / 2;   // half the streak (Zoom: in log scale)
    const unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    for (int k = 1; k <= m; ++k) {
        const double a = A / static_cast<double>(1 << k);
        // q = c + M (P - c) for the two taps; M a rotation by +-a, or a scaling by e^(+-a).
        double m11[2], m12[2], m21[2], m22[2];
        for (int t = 0; t < 2; ++t) {
            const double sa = t == 0 ? a : -a;
            if (f.zoom) { const double e = std::exp(sa); m11[t] = e; m12[t] = 0; m21[t] = 0; m22[t] = e; }
            else { m11[t] = std::cos(sa); m12[t] = -std::sin(sa); m21[t] = std::sin(sa); m22[t] = std::cos(sa); }
        }
        std::atomic<int> nextrow{0};
        std::atomic<bool> quit{false};
        const auto work = [&] {
            for (int y; (y = nextrow.fetch_add(1)) < H;) {
                if ((y & 15) == 0 && stop && stop()) { quit = true; return; }
                float* o = nxt.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(W) * static_cast<std::size_t>(C);
                const double py = y + oy + 0.5 - ccy;
                for (int t = 0; t < 2; ++t) {
                    const double px0 = ox + 0.5 - ccx;
                    // buffer coordinates of the tap for x = 0, and its step along the row
                    double qx = ccx + m11[t] * px0 + m12[t] * py - 0.5 - ox, qy = ccy + m21[t] * px0 + m22[t] * py - 0.5 - oy;
                    const double dx = m11[t], dy = m21[t];
                    for (int x = 0; x < W; ++x, qx += dx, qy += dy) {
                        const double cxq = std::clamp(qx, 0.0, W - 1.0), cyq = std::clamp(qy, 0.0, H - 1.0);
                        const int x0 = static_cast<int>(cxq), y0 = static_cast<int>(cyq);
                        const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
                        const float fx = static_cast<float>(cxq - x0), fy = static_cast<float>(cyq - y0);
                        const float w00 = (1 - fx) * (1 - fy) * 0.5f, w10 = fx * (1 - fy) * 0.5f, w01 = (1 - fx) * fy * 0.5f, w11 = fx * fy * 0.5f;
                        const float* r0 = cur.data() + static_cast<std::size_t>(y0) * static_cast<std::size_t>(W) * static_cast<std::size_t>(C);
                        const float* r1 = cur.data() + static_cast<std::size_t>(y1) * static_cast<std::size_t>(W) * static_cast<std::size_t>(C);
                        const float *p00 = r0 + x0 * C, *p10 = r0 + x1 * C, *p01 = r1 + x0 * C, *p11 = r1 + x1 * C;
                        float* op = o + static_cast<std::size_t>(x) * static_cast<std::size_t>(C);
                        for (int c = 0; c < C; ++c) {
                            const float v = w00 * p00[c] + w10 * p10[c] + w01 * p01[c] + w11 * p11[c];
                            op[c] = t == 0 ? v : op[c] + v;
                        }
                    }
                }
            }
        };
        if (nt <= 1) work();
        else {
            std::vector<std::thread> pool;
            for (unsigned i = 0; i < nt; ++i) pool.emplace_back(work);
            for (auto& th : pool) th.join();
        }
        if (quit || (stop && stop())) return false;
        cur.swap(nxt);
    }
    return true;
}

// The noise: a hash of where, which channel and the seed -- so a tile's noise
// doesn't depend on the order tiles are made in (the preview == the result).
std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float unit(int x, int y, int c, std::uint32_t seed) {   // [0, 1)
    std::uint32_t h = mix(seed ^ 0x9e3779b9U);
    h = mix(h ^ static_cast<std::uint32_t>(x));
    h = mix(h ^ static_cast<std::uint32_t>(y) * 0x85ebca6bU);
    h = mix(h ^ static_cast<std::uint32_t>(c) * 0xc2b2ae35U);
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}
float noise(const Filter& f, int x, int y, int c) {
    const float a = static_cast<float>(f.amount / 100 * 0.5);
    if (!f.gaussian) return (2 * unit(x, y, c, f.seed) - 1) * a;
    // Box-Muller; the spread a / 2.
    const float u1 = std::max(unit(x, y, c, f.seed), 1e-7f), u2 = unit(x, y, c + 8, f.seed);
    return std::sqrt(-2 * std::log(u1)) * std::cos(6.2831853f * u2) * (a / 2);
}
}  // namespace

std::vector<IrisRow> iris(double radius, int blades, double curvature, double angle_deg) {
    const double R = std::max(0.5, radius);
    const int n = std::clamp(blades, 3, 8);
    const double c = std::clamp(curvature, 0.0, 1.0);
    const int r = static_cast<int>(std::ceil(R));
    constexpr double kPi = 3.14159265358979323846;
    const double side = 2 * kPi / n, first = angle_deg * kPi / 180 - kPi / 2;   // 0 degrees: a corner straight up
    // The outline: each side in 16 pieces, its radius between the polygon's and the circle's.
    std::vector<std::array<double, 2>> pts;
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < 16; ++j) {
            const double u = static_cast<double>(j) / 16;               // 0 at a corner
            const double poly = R * std::cos(kPi / n) / std::cos((u - 0.5) * side);
            const double rad = (1 - c) * poly + c * R;
            const double phi = first + (k + u) * side;
            pts.push_back({rad * std::cos(phi), rad * std::sin(phi)});
        }
    std::vector<IrisRow> rows(static_cast<std::size_t>(2 * r + 1), IrisRow{1, -1});
    for (int dy = -r; dy <= r; ++dy) {
        double lo = 1e300, hi = -1e300;
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto& a = pts[i];
            const auto& b = pts[(i + 1) % pts.size()];
            const double ya = a[1], yb = b[1];
            if ((dy < std::min(ya, yb)) || (dy > std::max(ya, yb)) || ya == yb) continue;
            const double x = a[0] + (dy - ya) / (yb - ya) * (b[0] - a[0]);
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
        if (hi >= lo) rows[static_cast<std::size_t>(dy + r)] = {lo, hi};
    }
    return rows;
}

double radial_length(const Filter& f, double rho) {
    constexpr double kPi = 3.14159265358979323846;
    return f.zoom ? rho * f.amount / 100 * 0.5 : rho * f.amount * 0.5 * kPi / 180;
}
int radial_passes(const Filter& f, double rho) {
    // Taps a pixel of streak: Draft a third (its long streaks show steps),
    // Good one, Best two -- 2^passes of them, at most 1024.
    static constexpr double per_px[3] = {1.0 / 3, 1.0, 2.0};
    const double taps = radial_length(f, rho) * per_px[std::clamp(f.quality, 0, 2)];
    if (taps <= 1) return 0;
    return std::clamp(static_cast<int>(std::ceil(std::log2(taps))), 1, 10);
}

bool warp_source(const Filter& f, double ex, double ey, double rx, double ry, double x, double y, double& sx, double& sy) {
    constexpr double kPi = 3.14159265358979323846;
    if (rx <= 0 || ry <= 0) return false;
    const double nx = (x - ex) / rx, ny = (y - ey) / ry;
    const double d = std::hypot(nx, ny);
    if (d >= 1) return false;
    double mx = nx, my = ny;
    // Spherize's radial map: d -> where it reads (k > 0: the middle read from
    // nearer the middle -- magnified; k < 0: from further out -- shrunk).
    const auto sphere = [](double t, double k) {
        return k >= 0 ? t * (1 - k) + k * (1 - std::sqrt(std::max(0.0, 1 - t * t)))
                      : t * (1 + k) - k * std::sin(t * kPi / 2);
    };
    if (f.kind == FilterKind::Twirl) {
        const double a = -f.angle * kPi / 180 * (1 - d);   // read from turned back
        const double c = std::cos(a), s = std::sin(a);
        mx = nx * c - ny * s;
        my = nx * s + ny * c;
    } else if (f.kind == FilterKind::Pinch) {
        if (d < 1e-9) return false;
        const double k = f.amount / 100;
        // + : read from further out (the middle squeezed in); the rim stays
        // (GIMP's whirl-and-pinch map; never past the rim for |k| <= 1).
        const double factor = std::pow(std::sin(kPi / 2 * d), -k);
        mx = nx * factor;
        my = ny * factor;
    } else if (f.kind == FilterKind::Spherize) {
        const double k = f.amount / 100;
        if (f.axis == 1) {   // Horizontal only: along the row, within the ellipse's width there
            const double w = std::sqrt(std::max(0.0, 1 - ny * ny));
            if (w < 1e-9) return false;
            const double t = std::abs(nx) / w;
            mx = (nx < 0 ? -1 : 1) * sphere(t, k) * w;
        } else if (f.axis == 2) {
            const double h = std::sqrt(std::max(0.0, 1 - nx * nx));
            if (h < 1e-9) return false;
            const double t = std::abs(ny) / h;
            my = (ny < 0 ? -1 : 1) * sphere(t, k) * h;
        } else {
            if (d < 1e-9) return false;
            const double ds = sphere(d, k);
            mx = nx / d * ds;
            my = ny / d * ds;
        }
    } else {
        return false;
    }
    sx = ex + mx * rx;
    sy = ey + my * ry;
    return true;
}

double depth_amount(const Filter& f, double d) {
    d = std::clamp(d, 0.0, 1.0);
    if (f.invert) d = 1 - d;
    const double F = std::clamp(f.focal, 0.0, 255.0) / 255.0;
    if (d < F) return (F - d) / F;
    if (d > F) return (d - F) / (1 - F);
    return 0;
}
std::string_view depth_key(DepthSource s) {
    return s == DepthSource::Transparency ? "transparency" : s == DepthSource::LayerMask ? "layer-mask" : "none";
}
std::optional<DepthSource> depth_from(std::string_view k) {
    if (k == "none") return DepthSource::None;
    if (k == "transparency") return DepthSource::Transparency;
    if (k == "layer-mask") return DepthSource::LayerMask;
    return std::nullopt;
}
bool wants_mask(const Filter& f) { return f.kind == FilterKind::LensBlur && f.depth == DepthSource::LayerMask; }

const std::vector<FilterKind>& kinds() {
    // Alphabetical within each submenu, as Photoshop's.
    static const std::vector<FilterKind> v{FilterKind::BoxBlur, FilterKind::GaussianBlur, FilterKind::LensBlur, FilterKind::MotionBlur,
                                           FilterKind::RadialBlur, FilterKind::SurfaceBlur,   // s040
                                           FilterKind::Pinch, FilterKind::Spherize, FilterKind::Twirl,   // s040 m4: Distort
                                           FilterKind::AddNoise, FilterKind::DustScratches, FilterKind::Median, FilterKind::ReduceNoise,
                                           FilterKind::SmartSharpen, FilterKind::UnsharpMask, FilterKind::HighPass,
                                           FilterKind::Maximum, FilterKind::Minimum};
    return v;
}
std::string_view label(FilterKind k) { const auto* i = info(k); return i ? i->label : "Filter"; }
std::string_view key(FilterKind k)   { const auto* i = info(k); return i ? i->key : "none"; }
std::string_view group(FilterKind k) { const auto* i = info(k); return i ? i->group : ""; }

std::string_view remove_key(Remove r) { return r == Remove::Lens ? "lens" : r == Remove::Motion ? "motion" : "gaussian"; }
std::optional<Remove> remove_from(std::string_view k) {
    if (k == "gaussian") return Remove::Gaussian;
    if (k == "lens") return Remove::Lens;
    if (k == "motion") return Remove::Motion;
    return std::nullopt;
}

Filter fresh(FilterKind k) {
    Filter f;
    f.kind = k;
    switch (k) {
        case FilterKind::GaussianBlur: f.radius = 2; break;
        case FilterKind::UnsharpMask:  f.radius = 1; f.amount = 100; break;
        case FilterKind::HighPass:     f.radius = 10; break;
        case FilterKind::AddNoise:     f.amount = 10; break;
        case FilterKind::BoxBlur:      f.radius = 10; break;
        case FilterKind::MotionBlur:   f.radius = 10; break;
        case FilterKind::Median:       f.radius = 1; break;
        // s034: Photoshop CC's defaults.
        case FilterKind::SmartSharpen: f.radius = 1; f.amount = 200; f.noise = 10; f.remove = Remove::Lens; break;
        case FilterKind::DustScratches: f.radius = 1; break;
        case FilterKind::ReduceNoise:  f.amount = 6; f.preserve = 60; f.colour = 45; f.sharpen = 25; break;
        case FilterKind::Maximum:
        case FilterKind::Minimum:      f.radius = 1; break;   // m3: Preserve Squareness
        case FilterKind::SurfaceBlur:  f.radius = 5; f.threshold = 15; break;   // m4
        case FilterKind::LensBlur:     f.radius = 15; f.blades = 6; f.threshold = 255; break;   // s037: Photoshop's
        case FilterKind::RadialBlur:   f.amount = 10; break;   // s040: Photoshop's (Spin, Good, the middle)
        case FilterKind::Twirl:        f.angle = 50; break;    // s040 m4: Photoshop's
        case FilterKind::Pinch:        f.amount = 50; break;
        case FilterKind::Spherize:     f.amount = 100; break;
        case FilterKind::None: break;
    }
    return f;
}

Filter normalised(const Filter& in) {
    Filter f = in;
    switch (f.kind) {
        case FilterKind::BoxBlur:    f.radius = std::round(std::clamp(f.radius, 1.0, 250.0)); break;
        case FilterKind::MotionBlur: f.radius = std::round(std::clamp(f.radius, 1.0, 500.0)); break;
        case FilterKind::Median:
        case FilterKind::DustScratches: f.radius = std::round(std::clamp(f.radius, 1.0, 100.0)); break;
        case FilterKind::SmartSharpen:  f.radius = std::round(std::clamp(f.radius, 0.1, 64.0) * 10) / 10; break;
        case FilterKind::ReduceNoise:   f.radius = 0.1; break;
        case FilterKind::Maximum:
        case FilterKind::Minimum:       f.radius = std::round(std::clamp(f.radius, 1.0, 500.0)); break;   // m3
        case FilterKind::SurfaceBlur:   f.radius = std::round(std::clamp(f.radius, 1.0, 100.0)); break;   // m4
        case FilterKind::LensBlur:      f.radius = std::round(std::clamp(f.radius, 1.0, 100.0)); break;   // s037
        case FilterKind::RadialBlur:    f.radius = 1; break;   // s040: no radius
        case FilterKind::Twirl: case FilterKind::Pinch: case FilterKind::Spherize: f.radius = 1; break;   // s040 m4
        default:                     f.radius = std::round(std::clamp(f.radius, 0.1, 250.0) * 10) / 10; break;
    }
    const bool streak = f.kind == FilterKind::MotionBlur || (f.kind == FilterKind::SmartSharpen && f.remove == Remove::Motion);
    f.angle = streak ? std::clamp(f.angle, -90, 90) : f.kind == FilterKind::LensBlur ? std::clamp(f.angle, 0, 360)
            : f.kind == FilterKind::Twirl ? std::clamp(f.angle, -999, 999) : 0;   // s040 m4
    f.threshold = std::clamp(f.threshold, 0, 255);
    if (f.kind == FilterKind::UnsharpMask || f.kind == FilterKind::SmartSharpen) f.amount = std::round(std::clamp(f.amount, 1.0, 500.0));
    else if (f.kind == FilterKind::AddNoise) f.amount = std::round(std::clamp(f.amount, 0.1, 400.0) * 10) / 10;
    else if (f.kind == FilterKind::ReduceNoise) f.amount = std::round(std::clamp(f.amount, 0.0, 10.0));
    else if (f.kind == FilterKind::LensBlur) f.amount = std::round(std::clamp(f.amount, 0.0, 100.0));   // s039: its Noise
    else if (f.kind == FilterKind::RadialBlur) f.amount = std::round(std::clamp(f.amount, 1.0, 100.0));   // s040
    else if (f.kind == FilterKind::Pinch || f.kind == FilterKind::Spherize) f.amount = std::round(std::clamp(f.amount, -100.0, 100.0));   // s040 m4
    else f.amount = 0;
    if (f.kind == FilterKind::SurfaceBlur) f.threshold = std::max(f.threshold, 2);   // m4
    else if (f.kind != FilterKind::UnsharpMask && f.kind != FilterKind::DustScratches && f.kind != FilterKind::LensBlur) f.threshold = 0;
    const Filter blank;   // s034: the settings other kinds don't have go back to their defaults
    const auto pct = [](double v) { return std::round(std::clamp(v, 0.0, 100.0)); };
    if (f.kind == FilterKind::SmartSharpen) {
        if (f.remove != Remove::Lens && f.remove != Remove::Motion) f.remove = Remove::Gaussian;
        f.noise = pct(f.noise);
        f.shadow_fade = pct(f.shadow_fade); f.shadow_tonal = pct(f.shadow_tonal);
        f.shadow_radius = std::round(std::clamp(f.shadow_radius, 1.0, 100.0));
        f.light_fade = pct(f.light_fade); f.light_tonal = pct(f.light_tonal);
        f.light_radius = std::round(std::clamp(f.light_radius, 1.0, 100.0));
    } else {
        f.remove = blank.remove; f.noise = blank.noise;
        f.shadow_fade = blank.shadow_fade; f.shadow_tonal = blank.shadow_tonal; f.shadow_radius = blank.shadow_radius;
        f.light_fade = blank.light_fade; f.light_tonal = blank.light_tonal; f.light_radius = blank.light_radius;
    }
    if (f.kind == FilterKind::ReduceNoise) { f.preserve = pct(f.preserve); f.colour = pct(f.colour); f.sharpen = pct(f.sharpen); }
    else f.preserve = f.colour = f.sharpen = 0;
    if (f.kind != FilterKind::Maximum && f.kind != FilterKind::Minimum) f.round = false;   // m3
    if (f.kind == FilterKind::LensBlur) {   // s037
        f.blades = std::round(std::clamp(f.blades, 3.0, 8.0));
        f.curvature = pct(f.curvature);
        f.bright = pct(f.bright);
        // s039: the depth map
        if (f.depth != DepthSource::Transparency && f.depth != DepthSource::LayerMask) f.depth = DepthSource::None;
        f.focal = std::round(std::clamp(f.focal, 0.0, 255.0));
    } else {
        f.blades = blank.blades; f.curvature = blank.curvature; f.bright = blank.bright;
        f.depth = blank.depth; f.focal = blank.focal; f.invert = blank.invert;
    }
    if (f.kind == FilterKind::RadialBlur) {   // s040
        f.quality = std::clamp(f.quality, 0, 2);
        f.cx = std::round(std::clamp(f.cx, 0.0, 100.0) * 10) / 10;
        f.cy = std::round(std::clamp(f.cy, 0.0, 100.0) * 10) / 10;
    } else { f.zoom = blank.zoom; f.quality = blank.quality; f.cx = blank.cx; f.cy = blank.cy; }
    f.axis = f.kind == FilterKind::Spherize ? std::clamp(f.axis, 0, 2) : 0;   // s040 m4
    if (f.kind != FilterKind::AddNoise && f.kind != FilterKind::LensBlur) { f.gaussian = f.mono = false; f.seed = 1; }   // s039: Lens Blur's Noise has them
    if (f.kind == FilterKind::AddNoise) f.radius = 0.1;
    return f;
}

bool identity(const Filter& f) { return f.kind == FilterKind::None; }

int reach(const Filter& in) {
    const Filter f = normalised(in);
    switch (f.kind) {
        case FilterKind::BoxBlur:
        case FilterKind::Median:     return static_cast<int>(f.radius);
        case FilterKind::MotionBlur: return static_cast<int>(f.radius) / 2 + 2;
        case FilterKind::DustScratches:
        case FilterKind::Maximum:
        case FilterKind::Minimum:
        case FilterKind::SurfaceBlur:   return static_cast<int>(f.radius);
        case FilterKind::LensBlur:      return static_cast<int>(std::ceil(f.radius)) + 1;   // s037: the iris and its part pixels
        case FilterKind::RadialBlur:    return 1 << 16;   // s040: a streak may cross the picture -- it reads all of it
        case FilterKind::Twirl: case FilterKind::Pinch: case FilterKind::Spherize: return 1 << 16;   // s040 m4: the whole ellipse
        case FilterKind::SmartSharpen: {   // s034: the kernel's reach, and the tones'
            int r = f.remove == Remove::Gaussian ? plan_of(f.radius).reach
                  : f.remove == Remove::Lens     ? static_cast<int>(std::floor(f.radius + 0.5))
                                                 : motion_distance(f) / 2 + 2;
            if (f.shadow_fade > 0) r = std::max(r, plan_of(f.shadow_radius).reach);
            if (f.light_fade > 0) r = std::max(r, plan_of(f.light_radius).reach);
            return r;
        }
        case FilterKind::ReduceNoise: {
            int r = 2 * rn_box(f) + (f.sharpen > 0 ? 3 : 0);
            if (rn_chroma(f) > 0) r = std::max(r, plan_of(rn_chroma(f)).reach);
            return r;
        }
        default:                     return gaussian(f.kind) ? plan_of(f.radius).reach : 0;
    }
}

void blur_line(float* p, int n, int stride, double sigma) {
    const Plan pl = plan_of(sigma);
    std::vector<float> tmp(static_cast<std::size_t>(std::max(n, 1)));
    if (pl.exact) { if (pl.half > 0) kernel_line(p, n, stride, pl, tmp); }
    else for (const int h : pl.box) box_line(p, n, stride, h, tmp);
}

std::optional<pixel::TileMap> filtered(const pixel::TileMap& src, const Filter& in, const pixel::TileMap* sel, bool keep_alpha,
                                       const Rect* only, const std::function<bool()>& cancelled,
                                       const std::function<void(double)>& progress, const pixel::TileMap* depth) {
    const auto& fmt = src.format();
    if (fmt.mode != pixel::Mode::RGB && fmt.mode != pixel::Mode::Gray) return std::nullopt;
    if (in.kind == FilterKind::None) return src;
    const Filter f = normalised(in);
    const int IW = src.width(), IH = src.height();
    Rect r{0, 0, IW, IH};
    if (sel) r = r.intersect(select::extent(*sel));
    const Rect whole_r = r;   // s040 m4: Distort's ellipse fits this (the preview's rectangle doesn't move it)
    if (only) r = r.intersect(*only);
    if (r.empty()) return src;

    const int C = fmt.channels(), cc = fmt.colour_channels();
    const bool has_a = fmt.alpha;
    const int ai = cc;   // alpha's index, when there is one
    const bool clamp01 = fmt.depth != pixel::Depth::F32;
    const Plan pl = gaussian(f.kind) ? plan_of(f.radius) : f.kind == FilterKind::BoxBlur ? box_plan(static_cast<int>(f.radius)) : Plan{};
    const int pad = reach(f);
    const int bins = fmt.depth == pixel::Depth::U8 ? 256 : 1024;
    // s039: Lens Blur's depth map -- the layer's alpha, or the mask passed in.
    const bool by_depth = f.kind == FilterKind::LensBlur &&
                          (f.depth == DepthSource::Transparency || (f.depth == DepthSource::LayerMask && depth));
    const std::vector<LensLevel> levels = by_depth ? lens_levels(f) : std::vector<LensLevel>{};

    const int cols = src.cols(), rows = src.rows();
    std::vector<pixel::TileMap::TileRef> grid(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row)
        for (int col = 0; col < cols; ++col) grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(col)] = src.tile(col, row);

    const int row0 = r.y / kTile, row1 = (r.y + r.h - 1) / kTile;
    const int bands = row1 - row0 + 1;
    std::atomic<int> next{0}, done{0};
    std::atomic<bool> stopped{false};
    std::mutex prog;
    const auto stop = [&] {
        if (stopped.load(std::memory_order_relaxed)) return true;
        if (cancelled && cancelled()) { stopped = true; return true; }
        return false;
    };

    // s040: RADIAL BLUR is worked before the bands, over the region its
    // streaks reach for `r` (G), by doubling passes; the bands write it out.
    Rect G{};
    std::vector<float> gbuf;
    const auto load_g = [&] {
        gbuf.assign(static_cast<std::size_t>(G.w) * static_cast<std::size_t>(G.h) * static_cast<std::size_t>(C), 0.f);
        std::vector<float> t;
        for (int tr = G.y / kTile; tr <= (G.y + G.h - 1) / kTile; ++tr)
            for (int tc = G.x / kTile; tc <= (G.x + G.w - 1) / kTile; ++tc) {
                const auto tile = src.tile(tc, tr);
                if (!tile) continue;
                tile->premultiplied(t);
                const Rect cell = Rect{tc * kTile, tr * kTile, kTile, kTile}.intersect(G);
                for (int y = cell.y; y < cell.y + cell.h; ++y) {
                    const float* from = t.data() + (static_cast<std::size_t>(y - tr * kTile) * kTile + static_cast<std::size_t>(cell.x - tc * kTile)) * static_cast<std::size_t>(C);
                    float* to = gbuf.data() + (static_cast<std::size_t>(y - G.y) * static_cast<std::size_t>(G.w) + static_cast<std::size_t>(cell.x - G.x)) * static_cast<std::size_t>(C);
                    std::copy(from, from + static_cast<std::size_t>(cell.w) * static_cast<std::size_t>(C), to);
                }
            }
    };
    if (warps(f.kind)) {   // s040 m4: DISTORT -- each pixel of r read through the map, bilinear
        G = Rect{whole_r.x - 2, whole_r.y - 2, whole_r.w + 4, whole_r.h + 4}.intersect({0, 0, IW, IH});
        load_g();
        std::vector<float> res = gbuf;
        const double ex = whole_r.x + whole_r.w / 2.0, ey = whole_r.y + whole_r.h / 2.0, rx = whole_r.w / 2.0, ry = whole_r.h / 2.0;
        std::atomic<int> nextrow{r.y};
        std::atomic<bool> quit{false};
        const auto work = [&] {
            for (int y; (y = nextrow.fetch_add(1)) < r.y + r.h;) {
                if ((y & 15) == 0 && stop()) { quit = true; return; }
                for (int x = r.x; x < r.x + r.w; ++x) {
                    double sx = 0, sy = 0;
                    if (!warp_source(f, ex, ey, rx, ry, x + 0.5, y + 0.5, sx, sy)) continue;
                    const double bx = std::clamp(sx - 0.5 - G.x, 0.0, G.w - 1.0), by = std::clamp(sy - 0.5 - G.y, 0.0, G.h - 1.0);
                    const int x0 = static_cast<int>(bx), y0 = static_cast<int>(by);
                    const int x1 = std::min(x0 + 1, G.w - 1), y1 = std::min(y0 + 1, G.h - 1);
                    const float fx = static_cast<float>(bx - x0), fy = static_cast<float>(by - y0);
                    const auto P = [&](int px, int py) { return gbuf.data() + (static_cast<std::size_t>(py) * static_cast<std::size_t>(G.w) + static_cast<std::size_t>(px)) * static_cast<std::size_t>(C); };
                    const float *p00 = P(x0, y0), *p10 = P(x1, y0), *p01 = P(x0, y1), *p11 = P(x1, y1);
                    float* o = res.data() + (static_cast<std::size_t>(y - G.y) * static_cast<std::size_t>(G.w) + static_cast<std::size_t>(x - G.x)) * static_cast<std::size_t>(C);
                    for (int c = 0; c < C; ++c)
                        o[c] = (p00[c] * (1 - fx) + p10[c] * fx) * (1 - fy) + (p01[c] * (1 - fx) + p11[c] * fx) * fy;
                }
            }
        };
        const unsigned nt = std::max(1u, std::thread::hardware_concurrency());
        if (nt <= 1) work();
        else { std::vector<std::thread> pool; for (unsigned i = 0; i < nt; ++i) pool.emplace_back(work); for (auto& th : pool) th.join(); }
        if (quit || stop()) return std::nullopt;
        gbuf.swap(res);
    } else if (f.kind == FilterKind::RadialBlur) {
        const double ccx = f.cx / 100 * IW, ccy = f.cy / 100 * IH;
        G = radial_box(r, f, ccx, ccy).intersect({0, 0, IW, IH});
        load_g();
        double far = 0;   // the longest streak: the PICTURE's farthest corner's (not r's -- the preview's passes are the result's)
        for (const double x : {0.0, static_cast<double>(IW)})
            for (const double y : {0.0, static_cast<double>(IH)}) far = std::max(far, std::hypot(x - ccx, y - ccy));
        if (!radial_run(gbuf, G.w, G.h, C, G.x, G.y, f, ccx, ccy, radial_passes(f, far), stop)) return std::nullopt;
    }

    const auto band = [&](int row) {
        const Rect b = Rect{r.x, row * kTile, r.w, kTile}.intersect(r);
        // The pixels it reads, premultiplied (the filters that read neighbours).
        Rect R{};
        std::vector<float> buf;
        if (f.kind == FilterKind::RadialBlur || warps(f.kind)) R = G;   // s040: worked whole, before the bands
        else if (reads(f.kind)) {
            R = Rect{b.x - pad, b.y - pad, b.w + 2 * pad, b.h + 2 * pad}.intersect({0, 0, IW, IH});
            buf.assign(static_cast<std::size_t>(R.w) * static_cast<std::size_t>(R.h) * static_cast<std::size_t>(C), 0.f);
            std::vector<float> t;
            for (int tr = R.y / kTile; tr <= (R.y + R.h - 1) / kTile; ++tr)
                for (int tc = R.x / kTile; tc <= (R.x + R.w - 1) / kTile; ++tc) {
                    const auto tile = src.tile(tc, tr);
                    if (!tile) continue;
                    tile->premultiplied(t);
                    const Rect cell = Rect{tc * kTile, tr * kTile, kTile, kTile}.intersect(R);
                    for (int y = cell.y; y < cell.y + cell.h; ++y) {
                        const float* from = t.data() + (static_cast<std::size_t>(y - tr * kTile) * kTile + static_cast<std::size_t>(cell.x - tc * kTile)) * static_cast<std::size_t>(C);
                        float* to = buf.data() + (static_cast<std::size_t>(y - R.y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(cell.x - R.x)) * static_cast<std::size_t>(C);
                        std::copy(from, from + static_cast<std::size_t>(cell.w) * static_cast<std::size_t>(C), to);
                    }
                }
            if (custom(f.kind)) {   // s034
                std::vector<float> out(buf.size());
                const Rect own{b.x - R.x, b.y - R.y, b.w, b.h};
                if (f.kind == FilterKind::SmartSharpen) smart(buf, out, R.w, R.h, C, cc, has_a, own, f, stop);
                else if (f.kind == FilterKind::DustScratches) dust(buf, out, R.w, R.h, C, cc, has_a, own, f, bins, clamp01, stop);
                else reduce(buf, out, R.w, R.h, C, cc, has_a, own, f, stop);
                buf.swap(out);
            } else if (f.kind == FilterKind::MotionBlur || ranked(f.kind)) {
                std::vector<float> out(buf.size());
                const Rect own{b.x - R.x, b.y - R.y, b.w, b.h};
                if (f.kind == FilterKind::LensBlur) {   // s037
                    std::vector<float> amt;
                    if (by_depth) {   // s039: each pixel's share of the radius
                        amt.resize(static_cast<std::size_t>(b.w) * static_cast<std::size_t>(b.h));
                        for (int y = b.y; y < b.y + b.h; ++y)
                            for (int x = b.x; x < b.x + b.w; ++x) {
                                double d = 1;   // no alpha: opaque everywhere
                                if (f.depth == DepthSource::LayerMask) d = 1.0 - select::coverage(*depth, x, y);
                                else if (has_a) d = buf[(static_cast<std::size_t>(y - R.y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x - R.x)) * static_cast<std::size_t>(C) + static_cast<std::size_t>(ai)];
                                amt[static_cast<std::size_t>(y - b.y) * static_cast<std::size_t>(b.w) + static_cast<std::size_t>(x - b.x)] =
                                    static_cast<float>(depth_amount(f, d));
                            }
                    }
                    lens(buf, out, R.w, R.h, C, cc, has_a, own, f, stop, by_depth ? amt.data() : nullptr, by_depth ? &levels : nullptr);
                    if (f.amount > 0) {   // s039: NOISE -- grain back in what blurred, as much as it blurred
                        Filter nf = f;
                        nf.amount = f.amount / 4;   // 100 % here: Add Noise's 25 %
                        for (int y = b.y; y < b.y + b.h; ++y)
                            for (int x = b.x; x < b.x + b.w; ++x) {
                                const float t = by_depth ? amt[static_cast<std::size_t>(y - b.y) * static_cast<std::size_t>(b.w) + static_cast<std::size_t>(x - b.x)] : 1.f;
                                if (t <= 0.f) continue;
                                float* o = out.data() + (static_cast<std::size_t>(y - R.y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x - R.x)) * static_cast<std::size_t>(C);
                                const float a = has_a ? o[ai] : 1.f;
                                if (a <= 0.f) continue;
                                for (int c = 0; c < cc; ++c)
                                    o[c] = std::max(0.f, o[c] + noise(nf, x, y, f.mono ? 0 : c) * a * t);
                            }
                    }
                }
                else if (f.kind == FilterKind::SurfaceBlur)   // m4
                    surface(buf, out, R.w, R.h, C, own, static_cast<int>(f.radius), f.threshold, bins, stop);
                else if (f.kind == FilterKind::Maximum || f.kind == FilterKind::Minimum)   // m3
                    extreme(buf, out, R.w, R.h, C, own, static_cast<int>(f.radius), f.round, f.kind == FilterKind::Maximum, stop);
                else if (f.kind == FilterKind::MotionBlur) motion(buf, out, R.w, R.h, C, own, static_cast<int>(f.radius), f.angle, stop);
                else median(buf, out, R.w, R.h, C, own, static_cast<int>(f.radius), bins, stop);
                buf.swap(out);
            } else {
                blur_buffer(buf, R.w, R.h, C, pl, stop);
            }
            if (stop()) return;
        }
        const std::vector<float>& B = (f.kind == FilterKind::RadialBlur || warps(f.kind)) ? gbuf : buf;   // s040
        for (int col = b.x / kTile; col <= (b.x + b.w - 1) / kTile; ++col) {
            const Rect cell = Rect{col * kTile, row * kTile, kTile, kTile}.intersect(b);
            const auto old = src.tile(col, row);
            pixel::TileWriter w = old ? pixel::TileWriter(old) : pixel::TileWriter(fmt, pixel::transparent());
            bool touched = false;
            for (int y = cell.y; y < cell.y + cell.h; ++y)
                for (int x = cell.x; x < cell.x + cell.w; ++x) {
                    const float k = sel ? select::coverage(*sel, x, y) : 1.f;
                    if (k <= 0.f) continue;
                    const Px o = old ? w.get(x - col * kTile, y - row * kTile) : pixel::transparent();
                    Px n = o;
                    const float oa = has_a ? o.c[static_cast<std::size_t>(ai)] : 1.f;
                    if (custom(f.kind)) {   // s034: straight colour, alpha kept
                        if (oa <= 0.f) continue;
                        const float* bp = B.data() + (static_cast<std::size_t>(y - R.y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x - R.x)) * static_cast<std::size_t>(C);
                        for (int c = 0; c < cc; ++c)
                            n.c[static_cast<std::size_t>(c)] = clamp01 ? std::clamp(bp[c], 0.f, 1.f) : std::max(bp[c], 0.f);
                    } else if (reads(f.kind)) {
                        const float* bp = B.data() + (static_cast<std::size_t>(y - R.y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x - R.x)) * static_cast<std::size_t>(C);
                        const float ba = has_a ? bp[ai] : 1.f;
                        if (replaces(f.kind)) {
                            if (has_a && !keep_alpha) {
                                if (ba <= 1e-6f && oa <= 0.f) continue;
                                n.c[static_cast<std::size_t>(ai)] = ba;
                            }
                            if (has_a && keep_alpha && oa <= 0.f) continue;
                            for (int c = 0; c < cc; ++c) {
                                const float v = ba > 1e-6f ? bp[c] / ba : 0.f;
                                // The median of each channel apart may put colour above alpha.
                                n.c[static_cast<std::size_t>(c)] = (clamp01 && ranked(f.kind)) ? std::min(v, 1.f) : v;
                            }
                        } else {
                            if (oa <= 0.f) continue;
                            for (int c = 0; c < cc; ++c) {
                                const float bl = ba > 1e-6f ? bp[c] / ba : o.c[static_cast<std::size_t>(c)];
                                const float d = o.c[static_cast<std::size_t>(c)] - bl;
                                float v = o.c[static_cast<std::size_t>(c)];
                                if (f.kind == FilterKind::HighPass) v = 0.5f + d;
                                else if (std::abs(d) * 255.f >= static_cast<float>(f.threshold) && d != 0.f)
                                    v = o.c[static_cast<std::size_t>(c)] + static_cast<float>(f.amount / 100) * d;
                                n.c[static_cast<std::size_t>(c)] = clamp01 ? std::clamp(v, 0.f, 1.f) : std::max(v, 0.f);
                            }
                        }
                    } else {   // Add Noise
                        if (oa <= 0.f) continue;
                        for (int c = 0; c < cc; ++c) {
                            const float v = o.c[static_cast<std::size_t>(c)] + noise(f, x, y, f.mono ? 0 : c);
                            n.c[static_cast<std::size_t>(c)] = clamp01 ? std::clamp(v, 0.f, 1.f) : std::max(v, 0.f);
                        }
                    }
                    if (k < 1.f)
                        for (int c = 0; c < C; ++c) n.c[static_cast<std::size_t>(c)] = o.c[static_cast<std::size_t>(c)] + (n.c[static_cast<std::size_t>(c)] - o.c[static_cast<std::size_t>(c)]) * k;
                    if (n == o) continue;
                    w.set(x - col * kTile, y - row * kTile, n);
                    touched = true;
                }
            if (!touched) continue;
            auto t = w.freeze();
            if (t && has_a && t->is_uniform() && t->get(0, 0).c[static_cast<std::size_t>(ai)] <= 0.f) t = nullptr;
            grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(col)] = t;
        }
    };

    // The bands over the cores -- fewer when one band's buffer is big.
    std::size_t per_band = 0;
    if (reads(f.kind) && f.kind != FilterKind::RadialBlur && !warps(f.kind))   // s040: their work is done before the bands
        per_band = static_cast<std::size_t>(std::min(IW, r.w + 2 * pad)) * static_cast<std::size_t>(std::min(IH, kTile + 2 * pad)) *
                   static_cast<std::size_t>(C) * sizeof(float) * (custom(f.kind) ? 6 : 2);   // s034: their planes
    constexpr std::size_t kBudget = std::size_t{768} << 20;
    unsigned n = std::max(1u, std::thread::hardware_concurrency());
    if (per_band) n = static_cast<unsigned>(std::clamp<std::size_t>(kBudget / per_band, 1, n));
    n = std::min<unsigned>(n, static_cast<unsigned>(bands));
    const auto worker = [&] {
        for (int i; (i = next.fetch_add(1)) < bands;) {
            if (stop()) return;
            band(row0 + i);
            const int d = done.fetch_add(1) + 1;
            if (progress) { std::lock_guard g(prog); progress(static_cast<double>(d) / bands); }
        }
    };
    if (n <= 1) worker();
    else {
        std::vector<std::thread> pool;
        for (unsigned i = 0; i < n; ++i) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
    }
    if (stopped || (cancelled && cancelled())) return std::nullopt;
    return src.with_grid(std::move(grid));
}

}  // namespace klimt::core::filter
