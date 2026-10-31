// core/Psd -- Photoshop documents in (s050). See core/Psd.hpp and
// docs/file-format.md "Photoshop (.psd)".
//
// The file, as Adobe's "Photoshop File Formats Specification" lays it out:
//   header (26) | colour mode data | image resources | layer and mask info
//   | the merged image
// Every number is big-endian. Layer records run BOTTOM FIRST; a group is a
// "section divider" record (lsct type 3) below its children and the group's
// own record (type 1 open / 2 closed) above them. 16-bit files keep their
// layers in an "Lr16" block at the end of the layer section instead.
#include "core/Psd.hpp"

#include "core/Transfer.hpp"
#include "formats/Formats.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace klimt::core::psd {

namespace P = pixel;

namespace {

struct KeyBlend { char key[5]; Blend b; };
constexpr KeyBlend kKeys[] = {
    {"pass", Blend::PassThrough}, {"norm", Blend::Normal},       {"diss", Blend::Dissolve},
    {"dark", Blend::Darken},      {"mul ", Blend::Multiply},     {"idiv", Blend::ColorBurn},
    {"lbrn", Blend::LinearBurn},  {"dkCl", Blend::DarkerColor},  {"lite", Blend::Lighten},
    {"scrn", Blend::Screen},      {"div ", Blend::ColorDodge},   {"lddg", Blend::LinearDodge},
    {"lgCl", Blend::LighterColor},{"over", Blend::Overlay},      {"sLit", Blend::SoftLight},
    {"hLit", Blend::HardLight},   {"vLit", Blend::VividLight},   {"lLit", Blend::LinearLight},
    {"pLit", Blend::PinLight},    {"hMix", Blend::HardMix},      {"diff", Blend::Difference},
    {"smud", Blend::Exclusion},   {"fsub", Blend::Subtract},     {"fdiv", Blend::Divide},
    {"hue ", Blend::Hue},         {"sat ", Blend::Saturation},   {"colr", Blend::Color},
    {"lum ", Blend::Luminosity},
};

// Adjustment and fill layers: a look Klimt would have to rebuild, not pixels.
const std::set<std::string> kAdjustKeys = {
    "SoCo", "GdFl", "PtFl", "brit", "levl", "curv", "expA", "vibA", "hue ", "hue2", "blnc",
    "blwh", "phfl", "mixr", "clrL", "nvrt", "post", "thrs", "grdm", "selc"};

std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// A bounds-checked big-endian cursor over the whole file.
struct Reader {
    const std::uint8_t* d = nullptr;
    std::size_t n = 0, at = 0;
    bool bad = false;
    bool need(std::size_t k) {
        if (bad || k > n - std::min(at, n)) { bad = true; return false; }
        return true;
    }
    std::uint8_t u8() { return need(1) ? d[at++] : 0; }
    std::uint16_t u16() {
        if (!need(2)) return 0;
        const auto v = static_cast<std::uint16_t>((d[at] << 8) | d[at + 1]);
        at += 2;
        return v;
    }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        const std::uint32_t v = (std::uint32_t(d[at]) << 24) | (std::uint32_t(d[at + 1]) << 16) |
                                (std::uint32_t(d[at + 2]) << 8) | std::uint32_t(d[at + 3]);
        at += 4;
        return v;
    }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::string str(std::size_t k) {
        if (!need(k)) return {};
        std::string s(reinterpret_cast<const char*>(d + at), k);
        at += k;
        return s;
    }
    void skip(std::size_t k) { if (need(k)) at += k; }
    void seek(std::size_t to) { if (to > n) bad = true; else at = to; }
};

std::string utf8_of_utf16be(const std::uint8_t* s, std::size_t count) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t c = (std::uint32_t(s[2 * i]) << 8) | s[2 * i + 1];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < count) {
            const std::uint32_t lo = (std::uint32_t(s[2 * i + 2]) << 8) | s[2 * i + 3];
            if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); ++i; }
        }
        if (c == 0) break;
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63)); }
        else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 63));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        }
    }
    return out;
}

// Photoshop's Pascal names are Mac Roman (what Photoshop and psd-tools
// write); the "luni" block, when present, has the real name and wins.
constexpr std::uint16_t kMacRoman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};
std::string utf8_of_macroman(const std::string& s) {
    std::string out;
    for (const char ch : s) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 0x80) { out += ch; continue; }
        const std::uint32_t c = kMacRoman[u - 0x80];
        if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 63)); }
        else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 63));
            out += static_cast<char>(0x80 | (c & 63));
        }
    }
    return out;
}

struct Rect {
    std::int32_t top = 0, left = 0, bottom = 0, right = 0;
    int w() const { return std::max(0, right - left); }
    int h() const { return std::max(0, bottom - top); }
};

struct Channel {
    std::int16_t  id = 0;
    std::uint32_t len = 0;
    std::vector<std::uint8_t> plane;   // decoded, big-endian samples
};

struct Record {
    Rect          box;
    std::vector<Channel> channels;
    std::string   blend_key = "norm";
    std::uint8_t  opacity = 255, clipping = 0, flags = 0;
    Rect          mask_box;
    bool          has_mask = false;
    std::uint8_t  mask_default = 0, mask_flags = 0;
    std::string   name;
    int           section = 0;         // lsct: 0 a layer, 1 / 2 a group (open / closed), 3 a group's end
    std::string   section_blend;       // lsct's own blend key, when it has one
    int           fill = 255;          // iOpa
    bool          adjust = false, text = false, smart = false, effects = false, vector_mask = false;
};

// PackBits, one row: `out` must be filled exactly.
bool unpack_row(const std::uint8_t*& s, const std::uint8_t* end, std::uint8_t* out, std::size_t want) {
    std::size_t got = 0;
    while (got < want) {
        if (s >= end) return false;
        const int h = static_cast<std::int8_t>(*s++);
        if (h >= 0) {
            const std::size_t k = static_cast<std::size_t>(h) + 1;
            if (k > static_cast<std::size_t>(end - s) || got + k > want) return false;
            std::memcpy(out + got, s, k);
            s += k;
            got += k;
        } else if (h != -128) {
            const std::size_t k = static_cast<std::size_t>(1 - h);
            if (s >= end || got + k > want) return false;
            std::memset(out + got, *s++, k);
            got += k;
        }
    }
    return true;
}

// One channel's data (its compression word first) into `plane`: w x h
// samples of `bps` bytes, big-endian.
bool decode(const std::uint8_t* s, std::size_t len, int w, int h, int bps, std::vector<std::uint8_t>& plane,
            std::string& why) {
    const std::size_t row = static_cast<std::size_t>(w) * static_cast<std::size_t>(bps);
    const std::size_t want = row * static_cast<std::size_t>(h);
    plane.assign(want, 0);
    if (len < 2) { why = "a channel with no compression word"; return false; }
    const int comp = (s[0] << 8) | s[1];
    s += 2;
    len -= 2;
    if (want == 0) return true;
    const std::uint8_t* end = s + len;
    switch (comp) {
        case 0:
            if (len < want) { why = "a short raw channel"; return false; }
            std::memcpy(plane.data(), s, want);
            return true;
        case 1: {
            const std::size_t counts = static_cast<std::size_t>(h) * 2;
            if (len < counts) { why = "a short RLE channel"; return false; }
            const std::uint8_t* q = s + counts;
            for (int y = 0; y < h; ++y) {
                const std::size_t k = (static_cast<std::size_t>(s[2 * y]) << 8) | s[2 * y + 1];
                if (k > static_cast<std::size_t>(end - q)) { why = "a damaged RLE channel"; return false; }
                const std::uint8_t* r = q;
                if (!unpack_row(r, q + k, plane.data() + row * static_cast<std::size_t>(y), row)) {
                    why = "a damaged RLE channel";
                    return false;
                }
                q += k;
            }
            return true;
        }
        case 2:
        case 3: {
            uLongf out = static_cast<uLongf>(want);
            if (uncompress(plane.data(), &out, s, static_cast<uLong>(len)) != Z_OK || out != want) {
                why = "a damaged ZIP channel";
                return false;
            }
            if (comp == 3) {   // each row: a sample is the difference from the one before it
                for (int y = 0; y < h; ++y) {
                    std::uint8_t* r = plane.data() + row * static_cast<std::size_t>(y);
                    if (bps == 1) {
                        for (int x = 1; x < w; ++x) r[x] = static_cast<std::uint8_t>(r[x] + r[x - 1]);
                    } else if (bps == 2) {
                        std::uint16_t prev = static_cast<std::uint16_t>((r[0] << 8) | r[1]);
                        for (int x = 1; x < w; ++x) {
                            const auto v = static_cast<std::uint16_t>(((r[2 * x] << 8) | r[2 * x + 1]) + prev);
                            r[2 * x] = static_cast<std::uint8_t>(v >> 8);
                            r[2 * x + 1] = static_cast<std::uint8_t>(v & 255);
                            prev = v;
                        }
                    }
                }
            }
            return true;
        }
        default: break;
    }
    why = "a channel compressed in a way Klimt doesn't know (" + std::to_string(comp) + ")";
    return false;
}

struct LayerInfo {
    std::vector<Record> records;
    bool ok = false;
};

// The layer info block, [r.at, end): the records, then their channels' data.
LayerInfo read_layer_info(Reader& r, std::size_t end, int bps, std::string& why, const formats::Progress& p) {
    LayerInfo li;
    int count = r.i16();
    if (count < 0) count = -count;   // negative: the first alpha is the merged picture's transparency
    li.records.resize(static_cast<std::size_t>(count));
    for (auto& rec : li.records) {
        rec.box.top = r.i32(); rec.box.left = r.i32(); rec.box.bottom = r.i32(); rec.box.right = r.i32();
        const int nch = r.u16();
        if (nch > 56) { why = "a damaged layer record"; return li; }
        rec.channels.resize(static_cast<std::size_t>(nch));
        for (auto& c : rec.channels) { c.id = r.i16(); c.len = r.u32(); }
        const std::string sig = r.str(4);
        if (r.bad || (sig != "8BIM" && sig != "8B64")) { why = "a damaged layer record"; return li; }
        rec.blend_key = r.str(4);
        rec.opacity = r.u8();
        rec.clipping = r.u8();
        rec.flags = r.u8();
        r.skip(1);
        const std::uint32_t extra = r.u32();
        const std::size_t extra_end = r.at + extra;
        if (r.bad || extra_end > end) { why = "a damaged layer record"; return li; }
        // The layer mask.
        const std::uint32_t mlen = r.u32();
        const std::size_t mask_end = r.at + mlen;
        if (mlen >= 18) {
            rec.has_mask = true;
            rec.mask_box.top = r.i32(); rec.mask_box.left = r.i32(); rec.mask_box.bottom = r.i32(); rec.mask_box.right = r.i32();
            rec.mask_default = r.u8();
            rec.mask_flags = r.u8();
        }
        r.seek(mask_end);
        // Blending ranges ("Blend If") -- not read.
        const std::uint32_t blen = r.u32();
        r.skip(blen);
        // The Pascal name, padded to 4 with its length byte.
        const std::uint8_t nlen = r.u8();
        rec.name = utf8_of_macroman(r.str(nlen));
        r.skip((4 - (1 + nlen) % 4) % 4);
        // Additional layer information: tagged blocks.
        while (!r.bad && r.at + 12 <= extra_end) {
            const std::string bsig = r.str(4);
            if (bsig != "8BIM" && bsig != "8B64") break;
            const std::string key = r.str(4);
            const std::uint32_t len = r.u32();
            const std::size_t at = r.at, next = at + len;
            if (next > extra_end) break;
            if (key == "luni" && len >= 4) {
                const std::uint32_t k = r.u32();
                if (k <= (len - 4) / 2) rec.name = utf8_of_utf16be(r.d + r.at, k);
            } else if ((key == "lsct" || key == "lsdk") && len >= 4) {
                rec.section = static_cast<int>(r.u32());
                if (len >= 12 && r.str(4) == "8BIM") rec.section_blend = r.str(4);
            } else if (key == "iOpa" && len >= 1) {
                rec.fill = r.u8();
            } else if (key == "TySh" || key == "tySh") {
                rec.text = true;
            } else if (key == "SoLd" || key == "PlLd" || key == "SoLE" || key == "plLd") {
                rec.smart = true;
            } else if (key == "lfx2" || key == "lrFX" || key == "lmfx") {
                rec.effects = true;
            } else if (key == "vmsk" || key == "vsms") {
                rec.vector_mask = true;
            } else if (kAdjustKeys.count(key)) {
                rec.adjust = true;
            }
            r.seek(next);
        }
        r.seek(extra_end);
        if (r.bad) { why = "a damaged layer record"; return li; }
    }
    // The channels' pixels, record by record.
    std::size_t n = 0, done = 0;
    for (const auto& rec : li.records) n += rec.channels.size();
    for (auto& rec : li.records) {
        for (auto& c : rec.channels) {
            if (p.stop()) { why = "cancelled"; return li; }
            if (c.len > end - std::min(r.at, end)) { why = "a damaged layer's pixels"; return li; }
            const std::uint8_t* s = r.d + r.at;
            r.at += c.len;
            const bool masky = c.id == -2 || c.id == -3;
            // A pixel layer: its colours, alpha and mask; a group: its mask only.
            const bool wanted = (rec.section == 0 && !rec.adjust) ? (c.id >= -2 && c.id <= 2)
                                                                   : (rec.section != 3 && !rec.adjust && c.id == -2);
            if (!wanted) continue;
            const Rect& b = masky ? rec.mask_box : rec.box;
            const std::uint64_t area = static_cast<std::uint64_t>(b.w()) * static_cast<std::uint64_t>(b.h());
            if (area > formats::kMaxPixels) { why = "a layer too big to hold"; return li; }
            if (!decode(s, c.len, b.w(), b.h(), bps, c.plane, why)) return li;
            p.at(0.05 + 0.85 * static_cast<double>(++done) / static_cast<double>(std::max<std::size_t>(1, n)));
        }
    }
    li.ok = true;
    return li;
}

double sample(const std::vector<std::uint8_t>& plane, std::size_t i, int bps) {
    if (bps == 1) return plane[i] / 255.0;
    return ((plane[2 * i] << 8) | plane[2 * i + 1]) / 65535.0;
}

// Write one sample (0..1) as the doc's stored bytes, native-endian.
void put(std::uint8_t* d, double v, int bps) {
    if (bps == 1) { *d = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); return; }
    const auto u = static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535));
    std::memcpy(d, &u, 2);
}

// A layer's channels as a W x H canvas, straight RGBA.
P::TileMap pixels_of(const Record& rec, const P::Format& f, int W, int H, int bps) {
    const Channel* ch[4] = {};   // R G B A
    for (const auto& c : rec.channels) {
        if (c.id >= 0 && c.id <= 2) ch[c.id] = &c;
        else if (c.id == -1) ch[3] = &c;
    }
    const int ss = bps;   // stored sample bytes: the doc's depth is the file's
    formats::BandBuilder band(f, W, H);
    const Rect& b = rec.box;
    const int x0 = std::max(0, b.left), x1 = std::min(W, b.right);
    for (int y = 0; y < H; ++y) {
        std::uint8_t* d = band.row(y);
        std::memset(d, 0, band.row_bytes());
        if (y >= b.top && y < b.bottom && x1 > x0) {
            const std::size_t base = static_cast<std::size_t>(y - b.top) * static_cast<std::size_t>(b.w());
            for (int x = x0; x < x1; ++x) {
                const std::size_t i = base + static_cast<std::size_t>(x - b.left);
                std::uint8_t* px = d + static_cast<std::size_t>(x) * 4 * static_cast<std::size_t>(ss);
                for (int k = 0; k < 4; ++k) {
                    const Channel* c = ch[k];
                    const double v = (c && c->plane.size() >= (i + 1) * static_cast<std::size_t>(bps)) ? sample(c->plane, i, bps)
                                                                                                        : (k == 3 ? 1.0 : 0.0);
                    put(px + k * ss, v, bps);
                }
            }
        }
        band.done(y);
    }
    return band.take();
}

// The layer mask as a W x H Gray map at the doc's depth. Photoshop's white
// shows; Klimt's mask holds the HIDE amount (0 shows) -- so it is inverted.
P::TileMap mask_of(const Record& rec, int W, int H, int bps, P::Depth depth) {
    const P::Format f{depth, P::Mode::Gray, false};
    const Channel* m = nullptr;
    for (const auto& c : rec.channels) if (c.id == -2) m = &c;
    formats::BandBuilder band(f, W, H);
    const Rect& b = rec.mask_box;
    const double outside = rec.mask_default / 255.0;
    for (int y = 0; y < H; ++y) {
        std::uint8_t* d = band.row(y);
        for (int x = 0; x < W; ++x) {
            double v = outside;
            if (m && y >= b.top && y < b.bottom && x >= b.left && x < b.right) {
                const std::size_t i = static_cast<std::size_t>(y - b.top) * static_cast<std::size_t>(b.w()) +
                                      static_cast<std::size_t>(x - b.left);
                if (m->plane.size() >= (i + 1) * static_cast<std::size_t>(bps)) v = sample(m->plane, i, bps);
            }
            put(d + static_cast<std::size_t>(x) * static_cast<std::size_t>(bps), 1.0 - v, bps);
        }
        band.done(y);
    }
    return band.take();
}

int percent(int v255) { return static_cast<int>(std::lround(std::clamp(v255, 0, 255) * 100.0 / 255.0)); }

std::string check_tree(const file::Contents& c) {
    Document d;
    Document::Loading load(d);
    d.replace(c.image, c.layers, c.next_id);
    return d.check();
}

}  // namespace

std::optional<Blend> blend_of_key(std::string_view key) {
    for (const auto& k : kKeys) if (key == k.key) return k.b;
    return std::nullopt;
}

std::string_view key_of_blend(Blend b) {
    for (const auto& k : kKeys) if (k.b == b) return k.key;
    return "norm";
}

std::optional<file::Contents> open(const std::string& path, std::string& error, const formats::Progress& p) {
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) { error = "can't be read"; return std::nullopt; }
        const auto size = static_cast<std::streamoff>(in.tellg());
        if (size < 26) { error = "not a Photoshop document"; return std::nullopt; }
        bytes.resize(static_cast<std::size_t>(size));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(bytes.data()), size);
        if (!in) { error = "can't be read"; return std::nullopt; }
    }
    Reader r{bytes.data(), bytes.size()};
    if (r.str(4) != "8BPS") { error = "not a Photoshop document"; return std::nullopt; }
    const int version = r.u16();
    if (version == 2) { error = "a PSB (large document) -- Klimt reads PSD only"; return std::nullopt; }
    if (version != 1) { error = "a Photoshop version Klimt doesn't know"; return std::nullopt; }
    r.skip(6);
    const int nch = r.u16();
    const std::uint32_t H = r.u32(), W = r.u32();
    const int depth = r.u16(), mode = r.u16();
    static const char* const kModes[] = {"Bitmap", "Grayscale", "Indexed", "RGB", "CMYK", "?", "?", "Multichannel", "Duotone", "Lab"};
    if (mode != 3) {
        error = std::string("a ") + (mode >= 0 && mode <= 9 ? kModes[mode] : "?") +
                " document -- Klimt reads RGB Photoshop files (convert it to RGB in Photoshop)";
        return std::nullopt;
    }
    if (depth != 8 && depth != 16) {
        error = std::to_string(depth) + "-bit -- Klimt reads 8- and 16-bit Photoshop files";
        return std::nullopt;
    }
    if (W < 1 || H < 1 || W > formats::kMaxSide || H > formats::kMaxSide ||
        static_cast<std::uint64_t>(W) * H > formats::kMaxPixels || nch < 3) {
        error = "an image size Klimt can't hold (" + std::to_string(W) + " x " + std::to_string(H) + ")";
        return std::nullopt;
    }
    const int bps = depth / 8;
    const int Wi = static_cast<int>(W), Hi = static_cast<int>(H);

    file::Contents c;
    c.kind = formats::Kind::PSD;
    c.image.width = Wi;
    c.image.height = Hi;
    c.image.format = P::Format{depth == 16 ? P::Depth::U16 : P::Depth::U8, P::Mode::RGB, true};

    // Colour mode data: nothing for RGB.
    r.skip(r.u32());
    // Image resources: the resolution and the profile.
    {
        const std::uint32_t len = r.u32();
        const std::size_t end = r.at + len;
        while (!r.bad && r.at + 12 <= end) {
            if (r.str(4) != "8BIM") break;
            const int id = r.u16();
            const std::uint8_t nl = r.u8();
            r.skip(nl + ((1 + nl) % 2));
            const std::uint32_t sz = r.u32();
            const std::size_t at = r.at;
            if (at + sz > end) break;
            if (id == 1005 && sz >= 16) {   // ResolutionInfo: hRes (16.16), its unit (1 ppi, 2 ppcm)
                const double res = r.u32() / 65536.0;
                const int unit = r.u16();
                const double dpi = unit == 2 ? res * 2.54 : res;
                if (dpi >= 1 && dpi <= 10000) c.image.dpi = dpi;
            } else if (id == 1039 && sz > 0) {   // the ICC profile
                c.image.icc = std::make_shared<const std::vector<std::uint8_t>>(r.d + at, r.d + at + sz);
            }
            r.seek(at + sz + (sz % 2));
        }
        r.seek(end);
    }
    if (r.bad) { error = "a damaged Photoshop file (its header)"; return std::nullopt; }
    p.at(0.03);

    // Layer and mask information.
    LayerInfo li;
    std::string why;
    {
        const std::uint32_t len = r.u32();
        const std::size_t end = r.at + len;
        if (end > r.n) { error = "a damaged Photoshop file (its layers)"; return std::nullopt; }
        if (len > 0) {
            const std::uint32_t ilen = r.u32();
            const std::size_t iend = r.at + ilen;
            if (ilen > 0) {
                if (iend > end) { error = "a damaged Photoshop file (its layers)"; return std::nullopt; }
                li = read_layer_info(r, iend, bps, why, p);
                if (!li.ok) { error = why == "cancelled" ? why : "a damaged Photoshop file: " + why; return std::nullopt; }
            }
            r.seek(iend);
            // The global layer mask, then tagged blocks: 16-bit layers live in Lr16.
            const std::uint32_t glen = r.u32();
            r.skip(glen);
            while (!li.ok && !r.bad && r.at + 12 <= end) {
                const std::string sig = r.str(4);
                if (sig != "8BIM" && sig != "8B64") break;
                const std::string key = r.str(4);
                const std::uint32_t blen = r.u32();
                const std::size_t next = r.at + blen;
                if (next > end) break;
                if (key == "Lr16" || key == "Lr32") {
                    li = read_layer_info(r, next, bps, why, p);
                    if (!li.ok) { error = why == "cancelled" ? why : "a damaged Photoshop file: " + why; return std::nullopt; }
                }
                r.seek(next);
            }
        }
        r.seek(end);
    }

    int next = 1;
    int n_adjust = 0, n_effects = 0, n_vector = 0, n_text = 0, n_smart = 0, n_unknown_blend = 0, n_beyond = 0;
    // Records bottom first. A context is a group being filled: its children,
    // bottom first; a type-3 divider opens one, the group's own record closes it.
    struct Ctx { std::string id; std::vector<std::string> kids; };
    std::vector<Ctx> stack{{"", {}}};
    for (auto& rec : li.records) {
        if (p.stop()) { error = "cancelled"; return std::nullopt; }
        if (rec.section == 3) { stack.push_back({"L" + std::to_string(next++), {}}); continue; }
        Layer L;
        L.name = rec.name;
        L.visible = !(rec.flags & 2);
        L.opacity = percent(rec.opacity);
        if (rec.effects) ++n_effects;
        if (rec.vector_mask) ++n_vector;
        auto blend = blend_of_key(rec.blend_key);
        if (rec.section == 1 || rec.section == 2) {
            Ctx ctx = stack.size() > 1 ? std::move(stack.back()) : Ctx{"L" + std::to_string(next++), {}};
            if (stack.size() > 1) stack.pop_back();
            L.kind = Kind::Group;
            if (L.name.empty()) L.name = "Group";
            const auto sb = rec.section_blend.empty() ? blend : blend_of_key(rec.section_blend);
            if (!sb) ++n_unknown_blend;
            L.blend = sb.value_or(Blend::PassThrough);
            L.clipped = rec.clipping == 1;
            if (rec.has_mask && rec.mask_box.w() > 0 && rec.mask_box.h() > 0) {
                L.mask = std::make_shared<const P::TileMap>(mask_of(rec, Wi, Hi, bps, c.image.format.depth));
                L.mask_on = !(rec.mask_flags & 2);
            }
            for (std::size_t i = 0; i < ctx.kids.size(); ++i) {
                auto& kid = c.layers[ctx.kids[i]];
                kid.parent = ctx.id;
                kid.pos = static_cast<double>(i + 1);
            }
            c.layers[ctx.id] = std::move(L);
            stack.back().kids.push_back(ctx.id);
            continue;
        }
        if (rec.adjust) { ++n_adjust; continue; }
        if (rec.text) ++n_text;
        if (rec.smart) ++n_smart;
        if (!blend || *blend == Blend::PassThrough) { if (!blend) ++n_unknown_blend; blend = Blend::Normal; }
        L.kind = Kind::Pixel;
        if (L.name.empty()) L.name = "Layer " + std::to_string(next);
        L.blend = *blend;
        L.fill = percent(rec.fill);
        L.clipped = rec.clipping == 1;
        if (rec.box.left < 0 || rec.box.top < 0 || rec.box.right > Wi || rec.box.bottom > Hi) ++n_beyond;
        L.pixels = std::make_shared<const P::TileMap>(pixels_of(rec, c.image.format, Wi, Hi, bps));
        if (rec.has_mask && rec.mask_box.w() > 0 && rec.mask_box.h() > 0) {
            L.mask = std::make_shared<const P::TileMap>(mask_of(rec, Wi, Hi, bps, c.image.format.depth));
            L.mask_on = !(rec.mask_flags & 2);
        }
        rec.channels.clear();   // the planes: done with
        const std::string id = "L" + std::to_string(next++);
        c.layers[id] = std::move(L);
        stack.back().kids.push_back(id);
    }
    // Groups never closed (a damaged file): their layers join the level above.
    while (stack.size() > 1) {
        Ctx ctx = std::move(stack.back());
        stack.pop_back();
        for (auto& k : ctx.kids) stack.back().kids.push_back(k);
    }
    for (std::size_t i = 0; i < stack[0].kids.size(); ++i) {
        auto& L = c.layers[stack[0].kids[i]];
        L.parent = "";
        L.pos = static_cast<double>(i + 1);
    }
    // A clipped layer needs a base below it among its siblings.
    {
        std::map<std::string, std::vector<std::pair<double, std::string>>> by;
        for (const auto& [id, L] : c.layers) by[L.parent].emplace_back(L.pos, id);
        for (auto& [par, v] : by) {
            std::sort(v.begin(), v.end());
            if (!v.empty()) c.layers[v[0].second].clipped = false;
        }
    }

    if (c.layers.empty() && li.records.empty()) {
        // No layers: the merged picture, after the layer section.
        const int comp = r.u16();
        const std::size_t plane = static_cast<std::size_t>(Wi) * static_cast<std::size_t>(Hi) * static_cast<std::size_t>(bps);
        Record rec;
        rec.box = {0, 0, Hi, Wi};
        rec.channels.resize(3);
        if (comp == 1) {
            const std::size_t counts_at = r.at;
            const std::size_t counts = static_cast<std::size_t>(nch) * static_cast<std::size_t>(Hi) * 2;
            if (!r.need(counts)) { error = "a damaged Photoshop file (its picture)"; return std::nullopt; }
            std::size_t data = counts_at + counts;
            for (int ch = 0; ch < 3; ++ch) {
                // Rebuild a one-channel RLE block: its row counts, then its rows.
                std::size_t total = 0;
                std::vector<std::uint8_t> block{0, 1};
                for (int y = 0; y < Hi; ++y) {
                    const std::size_t at = counts_at + 2 * (static_cast<std::size_t>(ch) * static_cast<std::size_t>(Hi) + static_cast<std::size_t>(y));
                    block.push_back(r.d[at]);
                    block.push_back(r.d[at + 1]);
                    total += (static_cast<std::size_t>(r.d[at]) << 8) | r.d[at + 1];
                }
                if (data + total > r.n) { error = "a damaged Photoshop file (its picture)"; return std::nullopt; }
                block.insert(block.end(), r.d + data, r.d + data + total);
                data += total;
                if (!decode(block.data(), block.size(), Wi, Hi, bps, rec.channels[static_cast<std::size_t>(ch)].plane, why)) {
                    error = "a damaged Photoshop file: " + why;
                    return std::nullopt;
                }
                rec.channels[static_cast<std::size_t>(ch)].id = static_cast<std::int16_t>(ch);
                p.at(0.1 + 0.25 * (ch + 1));
            }
        } else if (comp == 0) {
            if (!r.need(plane * 3)) { error = "a damaged Photoshop file (its picture)"; return std::nullopt; }
            for (int ch = 0; ch < 3; ++ch) {
                auto& c3 = rec.channels[static_cast<std::size_t>(ch)];
                c3.id = static_cast<std::int16_t>(ch);
                c3.plane.assign(r.d + r.at + plane * static_cast<std::size_t>(ch), r.d + r.at + plane * static_cast<std::size_t>(ch + 1));
            }
        } else {
            error = "a merged picture compressed in a way Klimt doesn't read (" + std::to_string(comp) + ")";
            return std::nullopt;
        }
        Layer bg;
        bg.name = "Background";
        bg.pos = 1;
        bg.pixels = std::make_shared<const P::TileMap>(pixels_of(rec, c.image.format, Wi, Hi, bps));
        c.layers["L1"] = std::move(bg);
        next = 2;
        if (nch > 3) c.notes.push_back(plural(nch - 3, "extra channel", "extra channels") + " left out");
    } else if (c.layers.empty()) {   // only adjustment layers: one empty layer to paint on
        Layer L;
        L.name = "Layer 1";
        L.pos = 1;
        L.pixels = std::make_shared<const P::TileMap>(P::TileMap(c.image.format, Wi, Hi));
        c.layers["L" + std::to_string(next)] = std::move(L);
        ++next;
    }
    c.next_id = next;
    if (n_adjust) c.notes.push_back(plural(n_adjust, "adjustment / fill layer", "adjustment / fill layers") + " left out");
    if (n_effects) c.notes.push_back(plural(n_effects, "layer's styles", "layers' styles") + " left out");
    if (n_vector) c.notes.push_back(plural(n_vector, "vector mask", "vector masks") + " left out");
    if (n_text) c.notes.push_back(plural(n_text, "text layer", "text layers") + " read as pixels");
    if (n_smart) c.notes.push_back(plural(n_smart, "smart object", "smart objects") + " read as pixels");
    if (n_unknown_blend) c.notes.push_back(plural(n_unknown_blend, "unknown blend mode", "unknown blend modes") + " shown as Normal");
    if (n_beyond) c.notes.push_back(plural(n_beyond, "layer", "layers") + " cut at the canvas edge");
    const std::string bad = check_tree(c);
    if (!bad.empty()) { error = "a layer tree Klimt can't hold: " + bad; return std::nullopt; }
    p.at(1);
    return c;
}

}  // namespace klimt::core::psd
