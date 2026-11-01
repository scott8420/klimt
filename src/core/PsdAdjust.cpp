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

// s064: the six colour ranges are core::kHueBands (Photoshop's), now per Adjust.

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
    for (const auto& r : a.ranges) {   // s064: the ranges as they are
        for (const int v : r.band) bes16(b, v);
        bes16(b, r.hue); bes16(b, r.saturation); bes16(b, r.lightness);
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
// s062: an object inside (Black & White's tint colour) -- its items by
// "outer.inner" key.
bool read_items(Cur& c, Desc& out, const std::string& prefix, int depth) {
    const std::uint32_t n = c.u32();
    for (std::uint32_t i = 0; i < n && !c.bad && i < 256; ++i) {
        const std::string key = prefix + desc_id(c);
        const std::string type = c.str(4);
        if (c.bad) return false;
        if (type == "long") out.nums.emplace_back(key, static_cast<std::int32_t>(c.u32()));
        else if (type == "bool") out.nums.emplace_back(key, c.u8() ? 1.0 : 0.0);
        else if (type == "doub") out.nums.emplace_back(key, c.f64());
        else if (type == "UntF") { c.skip(4); out.nums.emplace_back(key, c.f64()); }
        else if (type == "enum") { desc_id(c); desc_id(c); }
        else if (type == "TEXT") desc_ustr(c);
        else if (type == "Objc" && depth < 4) {
            desc_ustr(c);
            desc_id(c);
            if (!read_items(c, out, key + ".", depth + 1)) return false;
        }
        else return false;   // a kind not needed here: what came before stands
    }
    return !c.bad;
}
Desc read_desc(Cur& c) {
    Desc out;
    if (c.u32() != 16) { c.bad = true; return out; }
    desc_ustr(c);
    desc_id(c);
    read_items(c, out, "", 0);
    return out;
}
double desc_num(const Desc& d, std::string_view key, double dflt) {
    for (const auto& [k, v] : d.nums) if (k == key) return v;
    return dflt;
}
bool desc_has(const Desc& d, std::string_view key) {
    for (const auto& [k, v] : d.nums) if (k == key) return true;
    return false;
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
// ── s062: Vibrance, Black & White, Photo Filter ───────────────────────────
// vibA / blwh: a descriptor (version 16). phfl: version 2 -- an RGB colour
// record (space 0, four 16-bit components), density (4), Preserve
// Luminosity (1). Version 3 holds the colour as XYZ, whose scale the spec
// doesn't give: read for its density and luminosity, the colour left at
// Klimt's (a loss the toast names).
struct DescW {
    Bytes b;
    explicit DescW(std::uint32_t count, std::string_view cls = "null") {
        be32(b, 16);
        be32(b, 1); be16(b, 0);
        desc_key(b, cls);
        be32(b, count);
    }
    void lng(std::string_view k, int v) { desc_key(b, k); put(b, "long"); be32(b, static_cast<std::uint32_t>(v)); }
    void bln(std::string_view k, bool v) { desc_key(b, k); put(b, "bool"); b.push_back(v ? 1 : 0); }
    void dbl(std::string_view k, double v) {
        desc_key(b, k); put(b, "doub");
        std::uint64_t u;
        std::memcpy(&u, &v, 8);
        be32(b, static_cast<std::uint32_t>(u >> 32)); be32(b, static_cast<std::uint32_t>(u));
    }
};
Bytes viba_block(const Adjust& a) {
    DescW d(2);
    d.lng("vibrance", a.vibrance);
    d.lng("Strt", a.saturation);
    return d.b;
}
const char* const kBwKeys[6] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
Bytes blwh_block(const Adjust& a) {
    DescW d(9);
    for (std::size_t i = 0; i < 6; ++i) d.lng(kBwKeys[i], a.bw[i]);
    d.bln("useTint", a.bw_tint);
    // The tint as Photoshop keeps it: an RGB colour (Klimt's hue and
    // saturation at half lightness).
    double r, g, bl;
    adjust::from_hsl(a.tint_hue / 360.0, a.tint_sat / 100.0, 0.5, r, g, bl);
    desc_key(d.b, "tintColor"); put(d.b, "Objc");
    be32(d.b, 1); be16(d.b, 0);
    desc_key(d.b, "RGBC");
    be32(d.b, 3);
    d.dbl("Rd  ", r * 255); d.dbl("Grn ", g * 255); d.dbl("Bl  ", bl * 255);
    d.lng("bwPresetKind", 1);
    return d.b;
}
Bytes phfl_block(const Adjust& a) {
    Bytes b;
    be16(b, 2);
    be16(b, 0);   // RGB
    for (int i = 0; i < 3; ++i) be16(b, static_cast<unsigned>(std::lround(std::clamp(a.filter[static_cast<std::size_t>(i)], 0.0, 1.0) * 65535)));
    be16(b, 0);
    be32(b, static_cast<std::uint32_t>(a.density));
    b.push_back(a.keep_luma ? 1 : 0);
    return b;
}

// ── s063: Colour Balance ──────────────────────────────────────────────────
// blnc: shadows, midtones, highlights -- each cyan-red, magenta-green,
// yellow-blue (signed 2 bytes, -100..100) -- then Preserve Luminosity (1).
Bytes blnc_block(const Adjust& a) {
    Bytes b;
    for (const auto& t : a.balance) for (const int v : t) bes16(b, v);
    b.push_back(a.keep_luma ? 1 : 0);
    return b;
}

// ── s063: Channel Mixer ───────────────────────────────────────────────────
// mixr: version 1, Monochrome (2), then four outputs (R, G, B, and CMYK's
// black, unused in RGB) of five signed shorts: red, green, blue, black,
// constant. Monochrome: the grey in all four (where Photoshop keeps its
// grey isn't in the spec; read from the first).
Bytes mixr_block(const Adjust& a) {
    Bytes b;
    be16(b, 1);
    be16(b, a.monochrome ? 1 : 0);
    for (int o = 0; o < 4; ++o) {
        const std::array<int, 4> m = a.monochrome ? a.mono : o < 3 ? a.mixer[static_cast<std::size_t>(o)] : std::array<int, 4>{0, 0, 0, 0};
        bes16(b, m[0]); bes16(b, m[1]); bes16(b, m[2]);
        bes16(b, !a.monochrome && o == 3 ? 100 : 0);   // black in: only the black output's own
        bes16(b, m[3]);
    }
    return b;
}

// ── s063: Selective Colour ─────────────────────────────────────────────────
// selc: version 1, method (0 Relative, 1 Absolute), then ten records of
// cyan, magenta, yellow, black (signed shorts) -- the first unused, then
// reds .. magentas, whites, neutrals, blacks.
Bytes selc_block(const Adjust& a) {
    Bytes b;
    be16(b, 1);
    be16(b, a.selective_absolute ? 1 : 0);
    for (int i = 0; i < 4; ++i) bes16(b, 0);
    for (const auto& c : a.selective) for (const int v : c) bes16(b, v);
    return b;
}

// ── s063: Gradient Map ────────────────────────────────────────────────────
// grdm: version 1, reversed (1), dithered (1), the gradient's name (a
// Unicode string), the colour stops -- location 0..4096, midpoint 0..100,
// a colour record (space, four 16-bit components) and 2 bytes more (what
// psd-tools reads; the spec says 18 bytes a stop, so the reader tries
// both) -- the transparency stops (location, midpoint, opacity), then the
// noise-gradient settings Photoshop keeps for every gradient.
constexpr int kGrdmLoc = 4096;
Bytes grdm_block(const Adjust& a) {
    Bytes b;
    be16(b, 1);
    b.push_back(a.grad_reverse ? 1 : 0);
    b.push_back(a.grad_dither ? 1 : 0);
    const std::u16string name = u"Custom";
    be32(b, static_cast<std::uint32_t>(name.size() + 1));
    for (const char16_t ch : name) be16(b, ch);
    be16(b, 0);
    be16(b, static_cast<unsigned>(a.grad.size()));
    for (const auto& s : a.grad) {
        be32(b, static_cast<std::uint32_t>(std::lround(s.at * kGrdmLoc)));
        be32(b, static_cast<std::uint32_t>(std::lround(s.mid * 100)));
        be16(b, 0);   // RGB
        for (const double v : s.rgb) be16(b, static_cast<unsigned>(std::lround(std::clamp(v, 0.0, 1.0) * 65535)));
        be16(b, 0);
        be16(b, 0);
    }
    be16(b, 2);       // opaque at both ends (a Gradient Map ignores it)
    for (const int at : {0, kGrdmLoc}) { be32(b, static_cast<std::uint32_t>(at)); be32(b, 50); be16(b, 255); }
    be16(b, 2);       // expansion count
    be16(b, kGrdmLoc);// interpolation (smoothness 100 %)
    be16(b, 32);      // length
    be16(b, 0);       // mode
    be32(b, 0);       // random seed
    be16(b, 0);       // show transparency
    be16(b, 0);       // vector colour
    be32(b, 2048);    // roughness
    be16(b, 0);       // colour model: RGB
    for (int i = 0; i < 4; ++i) be16(b, 0);     // minimum
    for (int i = 0; i < 4; ++i) be16(b, 100);   // maximum
    be16(b, 0);
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
        case AdjustKind::Vibrance:  return {{"vibA", viba_block(a)}};
        case AdjustKind::BlackWhite:return {{"blwh", blwh_block(a)}};
        case AdjustKind::PhotoFilter:return {{"phfl", phfl_block(a)}};
        case AdjustKind::ColourBalance:return {{"blnc", blnc_block(a)}};   // s063
        case AdjustKind::ChannelMixer:return {{"mixr", mixr_block(a)}};
        case AdjustKind::SelectiveColour:return {{"selc", selc_block(a)}};
        case AdjustKind::GradientMap:return {{"grdm", grdm_block(a)}};
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
        for (std::size_t r = 0; r < 6 && !c.bad; ++r) {   // s064: read, not lost
            auto& rg = a.ranges[r];
            for (int& v : rg.band) v = c.s16();
            rg.hue = c.s16(); rg.saturation = c.s16(); rg.lightness = c.s16();
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
    // s062
    if (key == "vibA") {
        const Desc d = read_desc(c);
        if (!desc_has(d, "vibrance") && !desc_has(d, "Strt")) return true;
        Adjust a = adjust::fresh(AdjustKind::Vibrance);
        a.vibrance = static_cast<int>(std::lround(desc_num(d, "vibrance", 0)));
        a.saturation = static_cast<int>(std::lround(desc_num(d, "Strt", 0)));
        m_a = adjust::normalised(a);
        return true;
    }
    if (key == "blwh") {
        const Desc d = read_desc(c);
        if (!desc_has(d, "Rd  ")) return true;
        Adjust a = adjust::fresh(AdjustKind::BlackWhite);
        for (std::size_t i = 0; i < 6; ++i) a.bw[i] = static_cast<int>(std::lround(desc_num(d, kBwKeys[i], a.bw[i])));
        a.bw_tint = desc_num(d, "useTint", 0) != 0;
        if (desc_has(d, "tintColor.Rd  ")) {
            double h, s, l;
            adjust::to_hsl(desc_num(d, "tintColor.Rd  ", 0) / 255, desc_num(d, "tintColor.Grn ", 0) / 255, desc_num(d, "tintColor.Bl  ", 0) / 255, h, s, l);
            a.tint_hue = static_cast<int>(std::lround(h * 360)) % 360;
            a.tint_sat = static_cast<int>(std::lround(s * 100));
        }
        m_a = adjust::normalised(a);
        return true;
    }
    if (key == "phfl") {
        const unsigned version = c.u16();
        Adjust a = adjust::fresh(AdjustKind::PhotoFilter);
        if (version == 2) {
            const unsigned space = c.u16();
            double comp[4];
            for (double& v : comp) v = c.u16() / 65535.0;
            if (space == 0) a.filter = {comp[0], comp[1], comp[2]};
            else m_filter_colour = true;
        } else if (version == 3) {
            c.skip(12);
            m_filter_colour = true;
        } else return true;
        a.density = static_cast<int>(c.u32());
        a.keep_luma = c.u8() != 0;
        if (c.bad) { m_filter_colour = false; return true; }
        m_a = adjust::normalised(a);
        return true;
    }
    // s063
    if (key == "blnc") {
        Adjust a = adjust::fresh(AdjustKind::ColourBalance);
        for (auto& t : a.balance) for (int& v : t) v = c.s16();
        a.keep_luma = c.u8() != 0;
        if (c.bad) return true;
        m_a = adjust::normalised(a);
        return true;
    }
    if (key == "mixr") {
        if (c.u16() != 1) return true;
        Adjust a = adjust::fresh(AdjustKind::ChannelMixer);
        a.monochrome = c.u16() != 0;
        for (std::size_t o = 0; o < 4 && !c.bad; ++o) {
            const int r = c.s16(), g = c.s16(), b = c.s16();
            c.s16();   // black in
            const int k = c.s16();
            if (o < 3) a.mixer[o] = {r, g, b, k};
            if (o == 0 && a.monochrome) a.mono = {r, g, b, k};
        }
        if (c.bad) return true;
        if (a.monochrome) a.mixer = adjust::fresh(AdjustKind::ChannelMixer).mixer;
        m_a = adjust::normalised(a);
        return true;
    }
    if (key == "selc") {
        if (c.u16() != 1) return true;
        Adjust a = adjust::fresh(AdjustKind::SelectiveColour);
        a.selective_absolute = c.u16() != 0;
        c.skip(8);
        for (auto& col : a.selective) for (int& v : col) v = c.s16();
        if (c.bad) return true;
        m_a = adjust::normalised(a);
        return true;
    }
    if (key == "grdm") {
        if (c.u16() != 1) return true;
        Adjust a = adjust::fresh(AdjustKind::GradientMap);
        a.grad_reverse = c.u8() != 0;
        a.grad_dither = c.u8() != 0;
        desc_ustr(c);   // the name
        const unsigned n = c.u16();
        if (c.bad || n < 2 || n > 256) return true;
        const std::size_t start = c.at;
        // A stop is 20 bytes (psd-tools) or 18 (the spec): the one whose tail
        // reads as Photoshop writes it (expansion 2, length 32) wins.
        for (const std::size_t stride : {std::size_t{20}, std::size_t{18}}) {
            Cur t = c;
            t.at = start;
            std::vector<GradStop> stops;
            bool colour_lost = false;
            for (unsigned i = 0; i < n && !t.bad; ++i) {
                const std::size_t s0 = t.at;
                GradStop s;
                s.at = t.u32() / static_cast<double>(kGrdmLoc);
                s.mid = t.u32() / 100.0;
                const unsigned space = t.u16();
                double comp[4];
                for (double& v : comp) v = t.u16() / 65535.0;
                if (space == 0) s.rgb = {comp[0], comp[1], comp[2]};
                else if (space == 8) { const double g = 1 - std::min(1.0, comp[0] * 65535 / 10000); s.rgb = {g, g, g}; }   // greyscale: 0..10000 of black
                else { s.rgb = {s.at, s.at, s.at}; colour_lost = true; }
                t.at = s0 + stride;
                if (!t.need(0)) break;
                stops.push_back(s);
            }
            const unsigned tn = t.u16();
            if (t.bad || tn > 256) continue;
            t.skip(static_cast<std::size_t>(tn) * 10);
            const unsigned expansion = t.u16();
            t.u16();
            const unsigned length = t.u16();
            if (t.bad || expansion != 2 || length != 32) continue;
            a.grad = std::move(stops);
            m_gradient_colour = colour_lost;
            if (a.grad.size() > kGradStops) { a.grad.resize(kGradStops); m_gradient_colour = true; }
            m_a = adjust::normalised(a);
            return true;
        }
        return true;   // neither reads: not taken
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
    if (m_a && m_filter_colour) out.emplace_back("filter colour");   // s062
    if (m_a && m_gradient_colour) out.emplace_back("gradient colours");   // s063
    return out;
}

}  // namespace klimt::core::psd::adj
