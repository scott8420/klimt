// core/PsdAdjust -- adjustment layers in Photoshop's blocks (s053). See
// core/PsdAdjust.hpp and docs/file-format.md "Photoshop adjustment layers".
// Every number big-endian, as the rest of the PSD.
#include "core/PsdAdjust.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace klimt::core::psd::adj {

namespace {

void be16(Bytes& b, unsigned v) { b.push_back(static_cast<std::uint8_t>(v >> 8)); b.push_back(static_cast<std::uint8_t>(v)); }
void bes16(Bytes& b, int v) { be16(b, static_cast<unsigned>(static_cast<std::uint16_t>(static_cast<std::int16_t>(v)))); }
void be32(Bytes& b, std::uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }
void put(Bytes& b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }

int byte_of(double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); }

// A bounds-checked cursor over one block.
struct Cur {
    const std::uint8_t* d;
    std::size_t n, at = 0;
    bool bad = false;
    bool need(std::size_t k) { if (bad || k > n - std::min(at, n)) bad = true; return !bad; }
    unsigned u8() { return need(1) ? d[at++] : 0; }
    unsigned u16() { if (!need(2)) return 0; const unsigned v = (unsigned(d[at]) << 8) | d[at + 1]; at += 2; return v; }
    int s16() { return static_cast<std::int16_t>(static_cast<std::uint16_t>(u16())); }
    std::uint32_t u32() { const std::uint32_t hi = u16(); return (hi << 16) | u16(); }
    std::string str(std::size_t k) { if (!need(k)) return {}; std::string s(reinterpret_cast<const char*>(d + at), k); at += k; return s; }
    double f64() {
        if (!need(8)) return 0;
        std::uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u = (u << 8) | d[at + static_cast<std::size_t>(i)];
        at += 8;
        double v;
        std::memcpy(&v, &u, 8);
        return v;
    }
    void skip(std::size_t k) { if (need(k)) at += k; }
};

// ── Levels ────────────────────────────────────────────────────────────────

constexpr int kLevelRecords = 29;

Bytes levels_block(const Adjust& a) {
    Bytes b;
    be16(b, 2);
    for (int i = 0; i < kLevelRecords; ++i) {
        LevelsCh c;   // past R G B: Photoshop's "does nothing"
        if (i < kAdjustChannels) c = a.levels[static_cast<std::size_t>(i)];
        const int in_lo = std::clamp(byte_of(c.in_black), 0, 253);
        const int in_hi = std::clamp(byte_of(c.in_white), in_lo + 2, 255);
        bes16(b, in_lo);
        bes16(b, in_hi);
        bes16(b, byte_of(c.out_black));
        bes16(b, byte_of(c.out_white));
        bes16(b, std::clamp(static_cast<int>(std::lround(c.gamma * 100)), 10, 999));
    }
    return b;
}

// ── Curves ────────────────────────────────────────────────────────────────

// A channel's points as Photoshop's integers: by input, no two at one input,
// at least the two ends. Fewer than two points (Klimt's straight line): the
// diagonal.
std::vector<std::pair<int, int>> curve_points(const std::vector<CurvePt>& pts) {
    std::vector<std::pair<int, int>> out;   // input, output
    for (const auto& p : pts) {
        const int x = byte_of(p.x), y = byte_of(p.y);
        if (!out.empty() && out.back().first >= x) continue;
        out.emplace_back(x, y);
    }
    if (out.size() < 2) out = {{0, 0}, {255, 255}};
    if (out.size() > 19) out.resize(19);   // Photoshop's most (Klimt holds 16)
    return out;
}
bool diagonal(const std::vector<std::pair<int, int>>& p) {
    return p.size() == 2 && p[0] == std::pair{0, 0} && p[1] == std::pair{255, 255};
}

Bytes curves_block(const Adjust& a) {
    std::vector<std::pair<int, std::vector<std::pair<int, int>>>> chans;   // channel, points
    for (int k = 0; k < kAdjustChannels; ++k) {
        auto p = curve_points(a.curves[static_cast<std::size_t>(k)]);
        if (k == 0 || !diagonal(p)) chans.emplace_back(k, std::move(p));
    }
    Bytes b;
    b.push_back(0);   // points, not a 256-entry map
    be16(b, 1);
    std::uint32_t bits = 0;
    for (const auto& [k, p] : chans) bits |= 1u << k;
    be32(b, bits);
    const auto points = [&](const std::vector<std::pair<int, int>>& p) {
        be16(b, static_cast<unsigned>(p.size()));
        for (const auto& [x, y] : p) { be16(b, static_cast<unsigned>(y)); be16(b, static_cast<unsigned>(x)); }   // output, input
    };
    for (const auto& [k, p] : chans) points(p);
    put(b, "Crv ");
    be16(b, 4);
    be32(b, static_cast<std::uint32_t>(chans.size()));
    for (const auto& [k, p] : chans) { be16(b, static_cast<unsigned>(k)); points(p); }
    return b;
}

// ── Hue / Saturation ──────────────────────────────────────────────────────

// Photoshop's six colour ranges (Reds .. Magentas): each a fall-off and an
// inner band, in degrees.
constexpr int kRanges[6][4] = {{315, 345, 15, 45},   {15, 45, 75, 105},   {75, 105, 135, 165},
                               {135, 165, 195, 225}, {195, 225, 255, 285}, {255, 285, 315, 345}};

Bytes huesat_block(const Adjust& a) {
    Bytes b;
    be16(b, 2);
    b.push_back(a.colorize ? 1 : 0);
    b.push_back(0);
    if (a.colorize) {   // Colorize's own values; the master does nothing
        bes16(b, a.hue); bes16(b, a.saturation); bes16(b, a.lightness);
        bes16(b, 0); bes16(b, 0); bes16(b, 0);
    } else {            // Colorize's defaults (Photoshop's), then the master
        bes16(b, 0); bes16(b, 25); bes16(b, 0);
        bes16(b, a.hue); bes16(b, a.saturation); bes16(b, a.lightness);
    }
    for (const auto& r : kRanges) {
        for (const int v : r) bes16(b, v);
        bes16(b, 0); bes16(b, 0); bes16(b, 0);
    }
    return b;
}

// ── Brightness / Contrast ─────────────────────────────────────────────────

// "brit": the old block (its brightness and contrast reach +-100) -- for
// readers older than CS3.
Bytes brit_block(const Adjust& a) {
    Bytes b;
    bes16(b, std::clamp(a.brightness, -100, 100));
    bes16(b, std::clamp(a.contrast, -100, 100));
    bes16(b, 127);   // the mean
    b.push_back(0);  // Lab only: no
    b.push_back(0);
    return b;
}

// An action descriptor's key: four characters as length 0 + the four, else
// length + the characters.
void desc_key(Bytes& b, std::string_view k) {
    be32(b, k.size() == 4 ? 0 : static_cast<std::uint32_t>(k.size()));
    put(b, k);
}

// "CgEd": the descriptor Photoshop CS3+ reads -- the modern maths, Klimt's.
Bytes cged_block(const Adjust& a) {
    Bytes b;
    be32(b, 16);                  // descriptor version
    be32(b, 1); be16(b, 0);       // its name: "" (one null character)
    desc_key(b, "null");          // its class
    be32(b, 7);
    const auto lng = [&](std::string_view k, int v) { desc_key(b, k); put(b, "long"); be32(b, static_cast<std::uint32_t>(v)); };
    const auto bln = [&](std::string_view k, bool v) { desc_key(b, k); put(b, "bool"); b.push_back(v ? 1 : 0); };
    lng("Vrsn", 1);
    lng("Brgh", a.brightness);
    lng("Cntr", a.contrast);
    lng("means", 127);
    bln("Lab ", false);
    bln("useLegacy", false);
    bln("Auto", false);
    return b;
}

// A descriptor's items, as far as their kinds are known: the integers,
// booleans and doubles by key (what Brightness / Contrast holds).
struct Desc { std::vector<std::pair<std::string, double>> nums; };
std::string desc_id(Cur& c) {
    const std::uint32_t len = c.u32();
    if (len > 1024) { c.bad = true; return {}; }
    return c.str(len == 0 ? 4 : len);
}
void desc_ustr(Cur& c) {
    const std::uint32_t k = c.u32();
    if (k > 1u << 20) { c.bad = true; return; }
    c.skip(static_cast<std::size_t>(k) * 2);
}
Desc read_desc(Cur& c) {
    Desc out;
    if (c.u32() != 16) { c.bad = true; return out; }
    desc_ustr(c);
    desc_id(c);
    const std::uint32_t n = c.u32();
    for (std::uint32_t i = 0; i < n && !c.bad && i < 256; ++i) {
        const std::string key = desc_id(c);
        const std::string type = c.str(4);
        if (c.bad) break;
        if (type == "long") out.nums.emplace_back(key, static_cast<std::int32_t>(c.u32()));
        else if (type == "bool") out.nums.emplace_back(key, c.u8() ? 1.0 : 0.0);
        else if (type == "doub") out.nums.emplace_back(key, c.f64());
        else if (type == "UntF") { c.skip(4); out.nums.emplace_back(key, c.f64()); }
        else if (type == "enum") { desc_id(c); desc_id(c); }
        else if (type == "TEXT") desc_ustr(c);
        else break;   // a kind not needed here: what came before stands
    }
    return out;
}

// ── s061: Invert, Posterize, Threshold, Exposure ──────────────────────────
// nvrt: no data. post / thrs: the level (2 bytes) and 2 bytes of padding.
// expA: version 1, then exposure, offset, gamma as 4-byte floats.
Bytes level_block(int v) { Bytes b; be16(b, static_cast<unsigned>(v)); be16(b, 0); return b; }
void bef32(Bytes& b, double v) {
    const float f = static_cast<float>(v);
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    be32(b, u);
}
Bytes expa_block(const Adjust& a) {
    Bytes b;
    be16(b, 1);
    bef32(b, a.exposure);
    bef32(b, a.offset);
    bef32(b, a.exp_gamma);
    return b;
}
float f32(Cur& c) {
    const std::uint32_t u = c.u32();
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

}  // namespace

std::vector<Block> blocks(const Adjust& a) {
    switch (a.kind) {
        case AdjustKind::Levels:    return {{"levl", levels_block(a)}};
        case AdjustKind::Curves:    return {{"curv", curves_block(a)}};
        case AdjustKind::HueSat:    return {{"hue2", huesat_block(a)}};
        case AdjustKind::BrightCon: return {{"brit", brit_block(a)}, {"CgEd", cged_block(a)}};
        case AdjustKind::Invert:    return {{"nvrt", {}}};
        case AdjustKind::Posterize: return {{"post", level_block(a.posterize)}};
        case AdjustKind::Threshold: return {{"thrs", level_block(a.threshold)}};
        case AdjustKind::Exposure:  return {{"expA", expa_block(a)}};
        case AdjustKind::None:      break;
    }
    return {};
}

bool Reading::feed(std::string_view key, const std::uint8_t* d, std::size_t n) {
    Cur c{d, n};
    if (key == "levl") {
        if (c.u16() != 2) return true;
        Adjust a = adjust::fresh(AdjustKind::Levels);
        for (int i = 0; i < kLevelRecords && !c.bad; ++i) {
            const int lo = c.s16(), hi = c.s16(), olo = c.s16(), ohi = c.s16(), g = c.s16();
            if (c.bad) break;
            const bool none = (lo == 0 && hi == 255 && olo == 0 && ohi == 255 && g == 100) ||
                              (lo == 0 && hi == 0 && olo == 0 && ohi == 0 && g == 0);
            if (i >= kAdjustChannels) { if (!none) m_channels = true; continue; }
            if (none) continue;
            auto& ch = a.levels[static_cast<std::size_t>(i)];
            ch.in_black = lo / 255.0;
            ch.in_white = hi / 255.0;
            ch.out_black = olo / 255.0;
            ch.out_white = ohi / 255.0;
            ch.gamma = g / 100.0;
        }
        if (!c.bad) m_a = adjust::normalised(a);
        return true;
    }
    if (key == "curv") {
        c.u8();   // a map rather than points: 256-entry curves aren't read
        const unsigned version = c.u16();
        const std::uint32_t bits = c.u32();
        if (c.bad || version != 1) return true;   // v4 carries no channel ids up front
        Adjust a = adjust::fresh(AdjustKind::Curves);
        const auto points = [&](std::vector<CurvePt>* into) {
            const unsigned k = c.u16();
            if (k < 2 || k > 19) { c.bad = true; return; }
            std::vector<CurvePt> v;
            for (unsigned i = 0; i < k && !c.bad; ++i) {
                const double y = c.u16() / 255.0, x = c.u16() / 255.0;
                v.push_back({x, y});
            }
            if (into) *into = std::move(v);
        };
        const auto keep = [&](int ch, std::vector<CurvePt>&& v) {
            if (ch < kAdjustChannels) a.curves[static_cast<std::size_t>(ch)] = std::move(v);
            else if (!(v.size() == 2 && v[0].x == 0 && v[0].y == 0 && v[1].x == 1 && v[1].y == 1)) m_channels = true;
        };
        for (int ch = 0; ch < 32 && !c.bad; ++ch) {
            if (!(bits & (1u << ch))) continue;
            std::vector<CurvePt> v;
            points(&v);
            if (!c.bad) keep(ch, std::move(v));
        }
        if (c.bad) return true;
        // "Crv " v4: the same curves with their channels named -- it wins.
        if (c.n - c.at >= 10 && c.str(4) == "Crv " && c.u16() == 4) {
            Adjust b = adjust::fresh(AdjustKind::Curves);
            std::swap(a, b);
            const bool had_channels = m_channels;
            m_channels = false;
            const std::uint32_t count = c.u32();
            for (std::uint32_t i = 0; i < count && i < 64 && !c.bad; ++i) {
                const int ch = static_cast<int>(c.u16());
                std::vector<CurvePt> v;
                points(&v);
                if (!c.bad) keep(ch, std::move(v));
            }
            if (c.bad) { std::swap(a, b); m_channels = had_channels; }   // a damaged tail: the first part stands
        }
        m_a = adjust::normalised(a);
        return true;
    }
    if (key == "hue2") {
        if (c.u16() != 2) return true;
        Adjust a = adjust::fresh(AdjustKind::HueSat);
        a.colorize = c.u8() != 0;
        c.u8();
        const int ch = c.s16(), cs = c.s16(), cl = c.s16();
        const int mh = c.s16(), ms = c.s16(), ml = c.s16();
        for (int r = 0; r < 6 && !c.bad; ++r) {
            c.skip(8);
            const int h = c.s16(), s = c.s16(), l = c.s16();
            if (!c.bad && (h || s || l)) m_ranges = true;
        }
        if (c.bad) return true;
        if (a.colorize) { a.hue = ch; a.saturation = cs; a.lightness = cl; }
        else            { a.hue = mh; a.saturation = ms; a.lightness = ml; }
        m_a = adjust::normalised(a);
        return true;
    }
    // s061
    if (key == "nvrt") { m_a = adjust::fresh(AdjustKind::Invert); return true; }
    if (key == "post" || key == "thrs") {
        const int v = static_cast<int>(c.u16());
        if (c.bad) return true;
        Adjust a = adjust::fresh(key == "post" ? AdjustKind::Posterize : AdjustKind::Threshold);
        (key == "post" ? a.posterize : a.threshold) = v;
        m_a = adjust::normalised(a);
        return true;
    }
    if (key == "expA") {
        c.u16();   // version
        Adjust a = adjust::fresh(AdjustKind::Exposure);
        a.exposure = f32(c);
        a.offset = f32(c);
        a.exp_gamma = f32(c);
        if (c.bad || !std::isfinite(a.exposure) || !std::isfinite(a.offset) || !std::isfinite(a.exp_gamma)) return true;
        // Four decimals at most, as Photoshop shows them (the floats' dust off).
        const auto round4 = [](double v) { return std::round(v * 10000) / 10000; };
        a.exposure = round4(a.exposure); a.offset = round4(a.offset); a.exp_gamma = round4(a.exp_gamma);
        m_a = adjust::normalised(a);
        return true;
    }
    if (key == "brit") {
        const int b = c.s16(), k = c.s16();
        if (c.bad) return true;
        m_brit = true;
        m_brit_b = b;
        m_brit_c = k;
        return true;
    }
    if (key == "CgEd") {
        const Desc dsc = read_desc(c);
        bool any = false;
        int b = 0, k = 0;
        bool legacy = false;
        for (const auto& [name, v] : dsc.nums) {
            if (name == "Brgh") { b = static_cast<int>(std::lround(v)); any = true; }
            else if (name == "Cntr") { k = static_cast<int>(std::lround(v)); any = true; }
            else if (name == "useLegacy") legacy = v != 0;
        }
        if (!any) return true;   // a CgEd that isn't Brightness / Contrast
        m_cged = true;
        m_legacy = legacy;
        m_brit_b = b;
        m_brit_c = k;
        return true;
    }
    return false;
}

std::optional<Adjust> Reading::result() const {
    if (m_a) return m_a;
    if (m_cged || m_brit) {
        Adjust a = adjust::fresh(AdjustKind::BrightCon);
        a.brightness = m_brit_b;
        a.contrast = m_brit_c;
        return adjust::normalised(a);
    }
    return std::nullopt;
}

std::vector<std::string> Reading::losses() const {
    std::vector<std::string> out;
    if (m_a && m_a->kind == AdjustKind::HueSat && m_ranges) out.emplace_back("colour ranges");
    if (!m_a && ((m_cged && m_legacy) || (m_brit && !m_cged))) out.emplace_back("legacy");
    if (m_a && m_channels) out.emplace_back("channels");
    return out;
}

}  // namespace klimt::core::psd::adj
