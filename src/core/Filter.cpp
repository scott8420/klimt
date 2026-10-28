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
};
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
    return k == FilterKind::GaussianBlur || k == FilterKind::BoxBlur || k == FilterKind::MotionBlur || k == FilterKind::Median;
}
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

const std::vector<FilterKind>& kinds() {
    // Alphabetical within each submenu, as Photoshop's.
    static const std::vector<FilterKind> v{FilterKind::BoxBlur, FilterKind::GaussianBlur, FilterKind::MotionBlur,
                                           FilterKind::AddNoise, FilterKind::Median, FilterKind::UnsharpMask, FilterKind::HighPass};
    return v;
}
std::string_view label(FilterKind k) { const auto* i = info(k); return i ? i->label : "Filter"; }
std::string_view key(FilterKind k)   { const auto* i = info(k); return i ? i->key : "none"; }
std::string_view group(FilterKind k) { const auto* i = info(k); return i ? i->group : ""; }

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
        case FilterKind::None: break;
    }
    return f;
}

Filter normalised(const Filter& in) {
    Filter f = in;
    switch (f.kind) {
        case FilterKind::BoxBlur:    f.radius = std::round(std::clamp(f.radius, 1.0, 250.0)); break;
        case FilterKind::MotionBlur: f.radius = std::round(std::clamp(f.radius, 1.0, 500.0)); break;
        case FilterKind::Median:     f.radius = std::round(std::clamp(f.radius, 1.0, 100.0)); break;
        default:                     f.radius = std::round(std::clamp(f.radius, 0.1, 250.0) * 10) / 10; break;
    }
    f.angle = f.kind == FilterKind::MotionBlur ? std::clamp(f.angle, -90, 90) : 0;
    f.threshold = std::clamp(f.threshold, 0, 255);
    if (f.kind == FilterKind::UnsharpMask) f.amount = std::round(std::clamp(f.amount, 1.0, 500.0));
    else if (f.kind == FilterKind::AddNoise) f.amount = std::round(std::clamp(f.amount, 0.1, 400.0) * 10) / 10;
    else f.amount = 0;
    if (f.kind != FilterKind::UnsharpMask) f.threshold = 0;
    if (f.kind != FilterKind::AddNoise) { f.gaussian = f.mono = false; f.seed = 1; }
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
                                       const std::function<void(double)>& progress) {
    const auto& fmt = src.format();
    if (fmt.mode != pixel::Mode::RGB && fmt.mode != pixel::Mode::Gray) return std::nullopt;
    if (in.kind == FilterKind::None) return src;
    const Filter f = normalised(in);
    const int IW = src.width(), IH = src.height();
    Rect r{0, 0, IW, IH};
    if (sel) r = r.intersect(select::extent(*sel));
    if (only) r = r.intersect(*only);
    if (r.empty()) return src;

    const int C = fmt.channels(), cc = fmt.colour_channels();
    const bool has_a = fmt.alpha;
    const int ai = cc;   // alpha's index, when there is one
    const bool clamp01 = fmt.depth != pixel::Depth::F32;
    const Plan pl = gaussian(f.kind) ? plan_of(f.radius) : f.kind == FilterKind::BoxBlur ? box_plan(static_cast<int>(f.radius)) : Plan{};
    const int pad = reach(f);
    const int bins = fmt.depth == pixel::Depth::U8 ? 256 : 1024;

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

    const auto band = [&](int row) {
        const Rect b = Rect{r.x, row * kTile, r.w, kTile}.intersect(r);
        // The pixels it reads, premultiplied (the filters that read neighbours).
        Rect R{};
        std::vector<float> buf;
        if (reads(f.kind)) {
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
            if (f.kind == FilterKind::MotionBlur || f.kind == FilterKind::Median) {
                std::vector<float> out(buf.size());
                const Rect own{b.x - R.x, b.y - R.y, b.w, b.h};
                if (f.kind == FilterKind::MotionBlur) motion(buf, out, R.w, R.h, C, own, static_cast<int>(f.radius), f.angle, stop);
                else median(buf, out, R.w, R.h, C, own, static_cast<int>(f.radius), bins, stop);
                buf.swap(out);
            } else {
                blur_buffer(buf, R.w, R.h, C, pl, stop);
            }
            if (stop()) return;
        }
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
                    if (reads(f.kind)) {
                        const float* bp = buf.data() + (static_cast<std::size_t>(y - R.y) * static_cast<std::size_t>(R.w) + static_cast<std::size_t>(x - R.x)) * static_cast<std::size_t>(C);
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
                                n.c[static_cast<std::size_t>(c)] = (clamp01 && f.kind == FilterKind::Median) ? std::min(v, 1.f) : v;
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
    if (reads(f.kind))
        per_band = static_cast<std::size_t>(std::min(IW, r.w + 2 * pad)) * static_cast<std::size_t>(std::min(IH, kTile + 2 * pad)) *
                   static_cast<std::size_t>(C) * sizeof(float) * 2;
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
