#include "core/Lut.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <numbers>
#include <sstream>

namespace klimt::core::lut {

namespace {
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
// Every sample held at 16 bits -- what the .klimt keeps -- so a table read
// back is the table saved, sample for sample.
float q16(double v) { return static_cast<float>(std::lround(clamp01(v) * 65535.0)) / 65535.f; }
double luma(double r, double g, double b) { return 0.299 * r + 0.587 * g + 0.114 * b; }
// An S through 0 and 1 (k > 0 more contrast, k < 0 less; |k| < 1 keeps it rising).
double s_curve(double x, double k) { return clamp01(x - k * std::sin(2 * std::numbers::pi * x) / (2 * std::numbers::pi)); }
void saturate(double& r, double& g, double& b, double k) {
    const double l = luma(r, g, b);
    r = clamp01(l + (r - l) * k); g = clamp01(l + (g - l) * k); b = clamp01(l + (b - l) * k);
}

using Fn = std::function<void(double&, double&, double&)>;
struct Look { const char* name; Fn f; };

const std::vector<Look>& table() {
    static const std::vector<Look> v{
        {"Warm Film", [](double& r, double& g, double& b) {
             r = s_curve(r, 0.35); g = s_curve(g, 0.35); b = s_curve(b, 0.35);
             const double l = luma(r, g, b);
             r = 0.035 + r * 0.95 + 0.05 * l; g = 0.03 + g * 0.95 + 0.01 * l; b = 0.04 + b * 0.92 - 0.05 * l;
         }},
        {"Teal & Orange", [](double& r, double& g, double& b) {
             const double l = luma(r, g, b), sh = (1 - l) * (1 - l), hi = l * (2 - l);
             r += -0.07 * sh + 0.08 * hi; g += 0.02 * sh + 0.015 * hi; b += 0.07 * sh - 0.09 * hi;
             r = s_curve(clamp01(r), 0.3); g = s_curve(clamp01(g), 0.3); b = s_curve(clamp01(b), 0.3);
         }},
        {"Bleach Bypass", [](double& r, double& g, double& b) {
             saturate(r, g, b, 0.45);
             r = s_curve(r, 0.6); g = s_curve(g, 0.6); b = s_curve(b, 0.6);
         }},
        {"Cool Fade", [](double& r, double& g, double& b) {
             r = s_curve(r, -0.15); g = s_curve(g, -0.15); b = s_curve(b, -0.15);
             r = 0.07 + r * 0.84; g = 0.08 + g * 0.85; b = 0.11 + b * 0.86;
             saturate(r, g, b, 0.85);
         }},
        {"Crisp", [](double& r, double& g, double& b) {
             saturate(r, g, b, 1.2);
             r = s_curve(r, 0.4); g = s_curve(g, 0.4); b = s_curve(b, 0.4);
         }},
        {"Sepia Print", [](double& r, double& g, double& b) {
             const double l = s_curve(clamp01(luma(r, g, b)), 0.2);
             r = std::pow(l, 0.85); g = l; b = std::pow(l, 1.3) * 0.95;
         }},
        {"Cross Process", [](double& r, double& g, double& b) {
             r = s_curve(r, 0.5); g = 0.03 + s_curve(g, 0.3) * 0.95; b = 0.12 + b * 0.72;
         }},
        {"Day for Night", [](double& r, double& g, double& b) {
             saturate(r, g, b, 0.45);
             r = r * 0.5 - 0.01; g = g * 0.55; b = b * 0.6 + 0.05;
             r = s_curve(clamp01(r), 0.2); g = s_curve(clamp01(g), 0.2); b = s_curve(clamp01(b), 0.2);
         }},
    };
    return v;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
// Up to three numbers off a line; how many there were.
int numbers(std::string_view s, std::array<double, 3>& out) {
    int k = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) break;
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
        if (k >= 3) return 4;
        double v = 0;
        const auto r = std::from_chars(s.data() + i, s.data() + j, v);
        if (r.ec != std::errc{} || r.ptr != s.data() + j) return -1;
        out[static_cast<std::size_t>(k++)] = v;
        i = j;
    }
    return k;
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}  // namespace

const std::vector<std::string>& looks() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const auto& l : table()) out.emplace_back(l.name);
        return out;
    }();
    return v;
}

Lut look(std::string_view name) {
    for (const auto& l : table()) {
        if (name != l.name) continue;
        Lut t{l.name, kLookSize, {}};
        t.rgb.reserve(static_cast<std::size_t>(kLookSize * kLookSize * kLookSize * 3));
        for (int b = 0; b < kLookSize; ++b)
            for (int g = 0; g < kLookSize; ++g)
                for (int r = 0; r < kLookSize; ++r) {
                    double R = r / double(kLookSize - 1), G = g / double(kLookSize - 1), B = b / double(kLookSize - 1);
                    l.f(R, G, B);
                    t.rgb.push_back(q16(R));
                    t.rgb.push_back(q16(G));
                    t.rgb.push_back(q16(B));
                }
        return t;
    }
    return {};
}

std::string refusal(const Lut& t) {
    if (t.n < 2 || t.n > kMaxLut) return "a table of " + std::to_string(t.n) + " a side (2 to " + std::to_string(kMaxLut) + ")";
    if (t.rgb.size() != static_cast<std::size_t>(t.n) * t.n * t.n * 3) return "the samples don't fill the table";
    for (const float v : t.rgb) if (!(v >= 0.f && v <= 1.f)) return "a sample outside 0..1";
    return {};
}

std::optional<Lut> parse_cube(std::string_view text, std::string& why) {
    Lut t;
    int n3 = 0, n1 = 0;
    std::array<double, 3> lo{0, 0, 0}, hi{1, 1, 1};
    std::vector<double> rows;
    std::size_t at = 0;
    int line_no = 0;
    while (at <= text.size()) {
        const std::size_t nl = text.find('\n', at);
        std::string_view line = trim(text.substr(at, nl == std::string_view::npos ? std::string_view::npos : nl - at));
        at = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        ++line_no;
        if (line.empty() || line.front() == '#') continue;
        std::array<double, 3> v{};
        if ((line.front() >= '0' && line.front() <= '9') || line.front() == '-' || line.front() == '.' || line.front() == '+') {
            if (numbers(line, v) != 3) { why = "line " + std::to_string(line_no) + ": not three numbers"; return std::nullopt; }
            rows.insert(rows.end(), v.begin(), v.end());
            continue;
        }
        const std::size_t sp = line.find_first_of(" \t");
        const std::string_view key = line.substr(0, sp), rest = sp == std::string_view::npos ? std::string_view{} : trim(line.substr(sp));
        if (key == "TITLE") {
            std::string_view s = rest;
            if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
            t.name = std::string(s);
        } else if (key == "LUT_3D_SIZE" || key == "LUT_1D_SIZE") {
            if (numbers(rest, v) != 1 || v[0] != std::floor(v[0])) { why = "a bad " + std::string(key); return std::nullopt; }
            (key == "LUT_3D_SIZE" ? n3 : n1) = static_cast<int>(v[0]);
        } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            if (numbers(rest, v) != 3) { why = "a bad " + std::string(key); return std::nullopt; }
            (key == "DOMAIN_MIN" ? lo : hi) = v;
        } else if (key == "LUT_3D_INPUT_RANGE" || key == "LUT_1D_INPUT_RANGE") {   // Resolve's
            if (numbers(rest, v) != 2) { why = "a bad " + std::string(key); return std::nullopt; }
            lo = {v[0], v[0], v[0]}; hi = {v[1], v[1], v[1]};
        }
        // Other keywords (LUT_IN_VIDEO_RANGE ...): ignored.
    }
    for (std::size_t c = 0; c < 3; ++c)
        if (!(hi[c] > lo[c])) { why = "an empty domain"; return std::nullopt; }
    const bool unit = lo == std::array<double, 3>{0, 0, 0} && hi == std::array<double, 3>{1, 1, 1};
    if (n3 > 0) {
        if (n3 < 2 || n3 > kMaxLut) { why = "LUT_3D_SIZE " + std::to_string(n3) + " (2 to " + std::to_string(kMaxLut) + ")"; return std::nullopt; }
        if (rows.size() != static_cast<std::size_t>(n3) * n3 * n3 * 3) {
            why = std::to_string(rows.size() / 3) + " rows for a " + std::to_string(n3) + "-cube";
            return std::nullopt;
        }
        if (!unit) { why = "a domain other than 0..1 (not supported yet)"; return std::nullopt; }
        t.n = n3;
        t.rgb.reserve(rows.size());
        for (const double v : rows) t.rgb.push_back(q16(v));
    } else if (n1 > 0) {
        if (n1 < 2 || n1 > 65536) { why = "LUT_1D_SIZE " + std::to_string(n1); return std::nullopt; }
        if (rows.size() != static_cast<std::size_t>(n1) * 3) { why = std::to_string(rows.size() / 3) + " rows for a 1D table of " + std::to_string(n1); return std::nullopt; }
        // Each channel through its own curve: sampled into a 3D table.
        const auto curve = [&](std::size_t c, double x) {
            const double p = clamp01((x - lo[c]) / (hi[c] - lo[c])) * (n1 - 1);
            const auto i = std::min<std::size_t>(static_cast<std::size_t>(p), static_cast<std::size_t>(n1 - 2));
            const double f = p - static_cast<double>(i);
            return rows[i * 3 + c] + (rows[(i + 1) * 3 + c] - rows[i * 3 + c]) * f;
        };
        t.n = kLookSize;
        for (int b = 0; b < t.n; ++b)
            for (int g = 0; g < t.n; ++g)
                for (int r = 0; r < t.n; ++r) {
                    const double s = 1.0 / (t.n - 1);
                    t.rgb.push_back(q16((curve(0, lo[0] + r * s * (hi[0] - lo[0])))));
                    t.rgb.push_back(q16((curve(1, lo[1] + g * s * (hi[1] - lo[1])))));
                    t.rgb.push_back(q16((curve(2, lo[2] + b * s * (hi[2] - lo[2])))));
                }
    } else {
        why = "no LUT_3D_SIZE";
        return std::nullopt;
    }
    return t;
}

std::optional<Lut> load_cube(const std::string& path, std::string& why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { why = "can't be read"; return std::nullopt; }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.size() > 64u << 20) { why = "too big for a .cube"; return std::nullopt; }
    auto t = parse_cube(text, why);
    if (t && t->name.empty()) {
        std::string stem = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
        if (const auto dot = stem.rfind('.'); dot != std::string::npos && dot > 0) stem.resize(dot);
        t->name = stem;
    }
    return t;
}

void apply(const Lut& t, float& r, float& g, float& b) {
    if (t.n < 2) return;
    const int n = t.n;
    const float m = static_cast<float>(n - 1);
    const float pr = std::clamp(r, 0.f, 1.f) * m, pg = std::clamp(g, 0.f, 1.f) * m, pb = std::clamp(b, 0.f, 1.f) * m;
    const int r0 = std::min(static_cast<int>(pr), n - 2), g0 = std::min(static_cast<int>(pg), n - 2), b0 = std::min(static_cast<int>(pb), n - 2);
    const float fr = pr - static_cast<float>(r0), fg = pg - static_cast<float>(g0), fb = pb - static_cast<float>(b0);
    const auto at = [&](int R, int G, int B) { return t.rgb.data() + (static_cast<std::size_t>((B * n + G) * n + R)) * 3; };
    float out[3];
    for (int c = 0; c < 3; ++c) {
        const float c000 = at(r0, g0, b0)[c], c100 = at(r0 + 1, g0, b0)[c], c010 = at(r0, g0 + 1, b0)[c], c110 = at(r0 + 1, g0 + 1, b0)[c];
        const float c001 = at(r0, g0, b0 + 1)[c], c101 = at(r0 + 1, g0, b0 + 1)[c], c011 = at(r0, g0 + 1, b0 + 1)[c], c111 = at(r0 + 1, g0 + 1, b0 + 1)[c];
        const float x00 = c000 + (c100 - c000) * fr, x10 = c010 + (c110 - c010) * fr;
        const float x01 = c001 + (c101 - c001) * fr, x11 = c011 + (c111 - c011) * fr;
        const float y0 = x00 + (x10 - x00) * fg, y1 = x01 + (x11 - x01) * fg;
        out[c] = y0 + (y1 - y0) * fb;
    }
    r = out[0]; g = out[1]; b = out[2];
}

std::string encode(const Lut& t) {
    std::string bytes;
    bytes.reserve(t.rgb.size() * 2);
    for (const float v : t.rgb) {
        const auto u = static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.f, 1.f) * 65535.f));
        bytes.push_back(static_cast<char>(u & 0xff));
        bytes.push_back(static_cast<char>(u >> 8));
    }
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = static_cast<std::uint8_t>(bytes[i]);
        const std::uint32_t b = i + 1 < bytes.size() ? static_cast<std::uint8_t>(bytes[i + 1]) : 0;
        const std::uint32_t c = i + 2 < bytes.size() ? static_cast<std::uint8_t>(bytes[i + 2]) : 0;
        const std::uint32_t w = a << 16 | b << 8 | c;
        out.push_back(kB64[w >> 18 & 63]);
        out.push_back(kB64[w >> 12 & 63]);
        out.push_back(i + 1 < bytes.size() ? kB64[w >> 6 & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kB64[w & 63] : '=');
    }
    return out;
}

std::optional<Lut> decode(const std::string& name, int n, std::string_view data) {
    if (n < 2 || n > kMaxLut) return std::nullopt;
    std::array<int, 256> val{};
    val.fill(-1);
    for (int i = 0; i < 64; ++i) val[static_cast<std::size_t>(static_cast<unsigned char>(kB64[i]))] = i;
    std::string bytes;
    bytes.reserve(data.size() / 4 * 3);
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char ch : data) {
        if (ch == '=') break;
        const int v = val[static_cast<std::size_t>(static_cast<unsigned char>(ch))];
        if (v < 0) return std::nullopt;
        acc = acc << 6 | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; bytes.push_back(static_cast<char>(acc >> bits & 0xff)); }
    }
    const std::size_t want = static_cast<std::size_t>(n) * n * n * 3;
    if (bytes.size() != want * 2) return std::nullopt;
    Lut t{name, n, {}};
    t.rgb.reserve(want);
    for (std::size_t i = 0; i < want; ++i) {
        const std::uint16_t u = static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[i * 2]) | static_cast<std::uint8_t>(bytes[i * 2 + 1]) << 8);
        t.rgb.push_back(static_cast<float>(u) / 65535.f);
    }
    return t;
}

}  // namespace klimt::core::lut
