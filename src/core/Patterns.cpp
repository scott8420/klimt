// core::pattern -- the built-in patterns (s028 m4). See core/Patterns.hpp.
#include "core/Patterns.hpp"

#include "formats/Formats.hpp"   // s033: the library's PNGs

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstdint>

namespace klimt::core::pattern {

const std::vector<Info>& builtin() {
    static const std::vector<Info> v = {
        {"paper", "Paper", 128},  {"canvas", "Canvas", 8},   {"checker", "Checker", 32}, {"stripes", "Stripes", 12},
        {"dots", "Dots", 16},     {"grid", "Grid", 16},      {"bricks", "Bricks", 32},   {"hatch", "Hatch", 8},
    };
    return v;
}

int index_of(const std::string& key) {
    const auto& v = builtin();
    for (std::size_t i = 0; i < v.size(); ++i)
        if (key == v[i].key) return static_cast<int>(i);
    return -1;
}

namespace {
double wrap(double v, double p) { v = std::fmod(v, p); return v < 0 ? v + p : v; }
// A smooth step over one pattern pixel: 0 below e - 0.5, 1 above e + 0.5.
double edge(double v, double e) { return std::clamp(v - e + 0.5, 0.0, 1.0); }
// Periodic value noise: a hashed lattice, wrapped at `cells`.
float hash(int x, int y, int seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343U ^ static_cast<std::uint32_t>(y) * 0xd8163841U ^
                      static_cast<std::uint32_t>(seed) * 0xcb1ab31fU;
    h ^= h >> 13; h *= 0x5bd1e995U; h ^= h >> 15;
    return static_cast<float>(h & 0xffff) / 65535.f;
}
double noise(double u, double v, int cells, int seed) {
    const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
    const double fx = u - x0, fy = v - y0;
    const double sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const auto at = [&](int x, int y) { return hash(((x % cells) + cells) % cells, ((y % cells) + cells) % cells, seed); };
    const double a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return (a * (1 - sx) + b * sx) * (1 - sy) + (c * (1 - sx) + d * sx) * sy;
}
constexpr double kDark = 0.15, kLight = 1.0;

// The pattern at a point in its own units (u, v), in [0, period).
double value(int id, double u, double v) {
    switch (id) {
        case 0: {   // Paper: three octaves of grain, around middle grey
            const double n = 0.55 * noise(u / 4, v / 4, 32, 1) + 0.3 * noise(u / 2, v / 2, 64, 2) + 0.15 * noise(u, v, 128, 3);
            return std::clamp(0.5 + (n - 0.5) * 0.9, 0.0, 1.0);
        }
        case 1: {   // Canvas: threads over and under, around middle grey
            const bool over = (static_cast<int>(u / 4) + static_cast<int>(v / 4)) % 2 == 0;
            const double t = over ? std::sin(std::fmod(u, 4.0) / 4 * M_PI) : std::sin(std::fmod(v, 4.0) / 4 * M_PI);
            return 0.38 + 0.24 * t;
        }
        case 2: {   // Checker: 16 px squares
            const bool a = (u < 16) != (v < 16);
            return a ? kDark : kLight;
        }
        case 3: {   // Stripes: diagonal, half dark (12 px along x and y)
            const double d = wrap(u + v, 12);
            return kLight + (kDark - kLight) * std::min(edge(d, 3), 1 - edge(d, 9));
        }
        case 4: {   // Dots: a 4.5 px dot in each 16 px cell
            const double r = std::hypot(u - 8, v - 8);
            return kLight + (kDark - kLight) * (1 - edge(r, 4.5));
        }
        case 5: {   // Grid: 1 px lines, 16 px apart -- centred on a pixel's centre, so crisp at 100 %
            const double a = wrap(u - 0.5, 16), b = wrap(v - 0.5, 16);
            const double dx = std::min(a, 16 - a), dy = std::min(b, 16 - b);
            return kLight + (kDark - kLight) * std::max(1 - edge(dx, 0.5), 1 - edge(dy, 0.5));
        }
        case 6: {   // Bricks: 32 x 16, the rows offset by half, 2 px mortar
            const double row = std::floor(v / 16);
            const double uu = wrap(u + (static_cast<int>(row) % 2 ? 16 : 0), 32), vv = wrap(v, 16);
            const double dx = std::min(uu, 32 - uu), dy = std::min(vv, 16 - vv);
            return kLight + (kDark - kLight) * std::max(1 - edge(dx, 1), 1 - edge(dy, 1));
        }
        case 7: {   // Hatch: crossed diagonal 1 px lines, 8 px apart
            const double a = wrap(u + v - 1, 8), b = wrap(u - v, 8);   // through pixel centres
            const double da = std::min(a, 8 - a), db = std::min(b, 8 - b);
            return kLight + (kDark - kLight) * std::max(1 - edge(da, 0.6), 1 - edge(db, 0.6));
        }
        default: return 0.5;
    }
}
}  // namespace

float grey(int id, double x, double y, double scale, double ox, double oy) {
    const auto& v = builtin();
    if (id < 0 || id >= static_cast<int>(v.size())) return 0.5f;
    const double s = std::clamp(scale, 0.01, 100.0), p = v[static_cast<std::size_t>(id)].period;
    // Samples per axis: one at 100 % or larger, more as the pattern shrinks.
    const int n = std::clamp(static_cast<int>(std::ceil(1.0 / s)), 1, 4);
    double sum = 0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const double px = x + (i + 0.5) / n - ox, py = y + (j + 0.5) / n - oy;
            sum += value(id, wrap(px / s, p), wrap(py / s, p));
        }
    return static_cast<float>(sum / (n * n));
}

// ── s033: your patterns ─────────────────────────────────────────────────────

void sample(const pixel::TileMap& img, double x, double y, double scale, double ox, double oy, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0.f;
    const int w = img.width(), h = img.height();
    if (w <= 0 || h <= 0) return;
    const double s = std::clamp(scale, 0.01, 100.0);
    const int n = std::clamp(static_cast<int>(std::ceil(1.0 / s)), 1, 4);
    const bool alpha = img.format().alpha;
    double acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const double px = (x + (i + 0.5) / n - ox) / s, py = (y + (j + 0.5) / n - oy) / s;
            const int ix = static_cast<int>(std::floor(wrap(px, w))) % w, iy = static_cast<int>(std::floor(wrap(py, h))) % h;
            const pixel::Px p = img.get(ix, iy);
            const double a = alpha ? p.c[3] : 1.0;
            for (int k = 0; k < 3; ++k) acc[k] += p.c[static_cast<std::size_t>(k)] * a;   // premultiplied while averaging
            acc[3] += a;
        }
    const double nn = n * n;
    out[3] = static_cast<float>(acc[3] / nn);
    for (int k = 0; k < 3; ++k) out[k] = acc[3] > 0 ? static_cast<float>(acc[k] / acc[3]) : 0.f;
}

namespace {
std::string g_dir;
std::vector<UserPattern> g_lib;
std::string trimmed(const std::string& in) {
    const auto b = in.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return in.substr(b, in.find_last_not_of(" \t\r\n") - b + 1);
}
// A name that is safe as a file name: no slashes, no leading dot.
std::string safe(std::string n) {
    for (auto& ch : n) if (ch == '/' || ch == '\\' || ch == ':' || static_cast<unsigned char>(ch) < 32) ch = '_';
    while (!n.empty() && n.front() == '.') n.erase(n.begin());
    if (n.size() > 60) n.resize(60);
    return n;
}
// Straight RGB + alpha, 8-bit: what a library pattern always is.
std::shared_ptr<const pixel::TileMap> as_pattern(const pixel::TileMap& m) {
    const pixel::Format want{pixel::Depth::U8, pixel::Mode::RGB, true};
    if (m.format() == want) return std::make_shared<const pixel::TileMap>(m);
    pixel::TileMap out(want, m.width(), m.height());
    const pixel::Format f = m.format();
    out = out.paint({0, 0, m.width(), m.height()}, [&](int x, int y, const pixel::Px&) {
        const pixel::Px p = m.get(x, y);
        pixel::Px q{};
        if (f.mode == pixel::Mode::Gray) q.c = {p.c[0], p.c[0], p.c[0], f.alpha ? p.c[1] : 1.f, 0};
        else q.c = {p.c[0], p.c[1], p.c[2], f.alpha ? p.c[static_cast<std::size_t>(f.channels() - 1)] : 1.f, 0};
        return q;
    });
    return std::make_shared<const pixel::TileMap>(std::move(out));
}
}  // namespace

void set_library_dir(const std::string& dir) {
    g_dir = dir;
    g_lib.clear();
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".png") continue;
        std::string err;
        auto r = formats::read_png(e.path().string(), err);
        if (!r || r->pixels.width() > kMaxSide || r->pixels.height() > kMaxSide) continue;
        g_lib.push_back({e.path().stem().string(), as_pattern(r->pixels)});
    }
    std::sort(g_lib.begin(), g_lib.end(), [](const UserPattern& a, const UserPattern& b) { return a.name < b.name; });
}

const std::vector<UserPattern>& library() { return g_lib; }

std::shared_ptr<const pixel::TileMap> find(const std::string& name) {
    for (const auto& p : g_lib) if (p.name == name) return p.image;
    return nullptr;
}

std::string add_to_library(const pixel::TileMap& img, const std::string& want_in, std::string& error) {
    if (img.width() <= 0 || img.height() <= 0) { error = "an empty pattern"; return {}; }
    if (img.width() > kMaxSide || img.height() > kMaxSide) { error = "a pattern is at most 1024 pixels a side"; return {}; }
    std::string want = safe(trimmed(want_in));
    if (want.empty()) want = "Pattern";
    const auto taken = [](const std::string& n) { return static_cast<bool>(find(n)); };
    std::string name = want;
    for (int k = 2; taken(name); ++k) name = want + " " + std::to_string(k);
    auto pat = as_pattern(img);
    if (!g_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(g_dir, ec);
        formats::Raster r;
        r.pixels = *pat;
        if (!formats::write_png((std::filesystem::path(g_dir) / (name + ".png")).string(), r, error)) return {};
    }
    g_lib.push_back({name, pat});
    std::sort(g_lib.begin(), g_lib.end(), [](const UserPattern& a, const UserPattern& b) { return a.name < b.name; });
    return name;
}

bool remove_from_library(const std::string& name) {
    const auto it = std::find_if(g_lib.begin(), g_lib.end(), [&](const UserPattern& p) { return p.name == name; });
    if (it == g_lib.end()) return false;
    if (!g_dir.empty()) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(g_dir) / (name + ".png"), ec);
    }
    g_lib.erase(it);
    return true;
}

std::optional<pixel::TileMap> define(const Image& im, const Layers& layers, pixel::Rect r, std::string& why) {
    r = r.intersect({0, 0, im.width, im.height});
    if (r.empty()) { why = "select the part of the picture to make a pattern of first"; return std::nullopt; }
    if (r.w > kMaxSide || r.h > kMaxSide) { why = "a pattern is at most 1024 pixels a side -- select less"; return std::nullopt; }
    const pixel::Format f{pixel::Depth::U8, pixel::Mode::RGB, true};
    pixel::TileMap out(f, r.w, r.h);
    std::vector<float> tile;
    int tc = -1, tr = -1;
    bool any = false;
    out = out.paint({0, 0, r.w, r.h}, [&](int x, int y, const pixel::Px&) {
        const int ix = r.x + x, iy = r.y + y;
        if (ix / pixel::kTile != tc || iy / pixel::kTile != tr) {
            tc = ix / pixel::kTile;
            tr = iy / pixel::kTile;
            composite_tile(im, layers, tc, tr, tile);
        }
        const float* p = tile.data() + (static_cast<std::size_t>(iy % pixel::kTile) * pixel::kTile + static_cast<std::size_t>(ix % pixel::kTile)) * 4;
        pixel::Px q{};
        const float a = std::clamp(p[3], 0.f, 1.f);
        if (a > 0.f) {
            any = true;
            q.c = {std::clamp(p[0] / a, 0.f, 1.f), std::clamp(p[1] / a, 0.f, 1.f), std::clamp(p[2] / a, 0.f, 1.f), a, 0};
        }
        return q;
    });
    if (!any) { why = "nothing to see there -- every layer is empty or hidden"; return std::nullopt; }
    return out;
}

}  // namespace klimt::core::pattern
