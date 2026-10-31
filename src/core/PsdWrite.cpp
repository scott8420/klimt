// core/PsdWrite -- Photoshop documents out (s051; REQUIREMENTS "Added" 3).
// See core/Psd.hpp and docs/file-format.md "Photoshop (.psd)".
//
// Written as Photoshop writes them, so Photoshop, GIMP, Krita, Affinity,
// Photopea and psd-tools all read the layers -- and anything that reads only
// the merged picture (Composix, file managers) sees the whole look:
//   header | no colour data | resources (resolution, profile) |
//   layer and mask info | the merged picture (RLE, matted with white)
// 8-bit layers in the layer info; 16-bit in an "Lr16" block after an empty
// one (Photoshop's way). Layer records BOTTOM first; a group is a "section
// divider" record (lsct 3) below its children and the group's own record
// (lsct 1) above them. Every channel PackBits (RLE), row by row.
#include "core/Composite.hpp"
#include "core/Live.hpp"
#include "core/Psd.hpp"
#include "core/Transfer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

namespace klimt::core::psd {

namespace P = pixel;

namespace {

using Bytes = std::vector<std::uint8_t>;
void be16(Bytes& b, unsigned v) { b.push_back(static_cast<std::uint8_t>(v >> 8)); b.push_back(static_cast<std::uint8_t>(v)); }
void be32(Bytes& b, std::uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }
void put(Bytes& b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }
void put(Bytes& b, const Bytes& s) { b.insert(b.end(), s.begin(), s.end()); }
void pad_to(Bytes& b, std::size_t from, std::size_t k) { while ((b.size() - from) % k) b.push_back(0); }
// A length word written ahead of its data, filled in once the data is there.
std::size_t hole(Bytes& b) { const std::size_t at = b.size(); be32(b, 0); return at; }
void fill(Bytes& b, std::size_t at) {
    const auto n = static_cast<std::uint32_t>(b.size() - at - 4);
    b[at] = static_cast<std::uint8_t>(n >> 24); b[at + 1] = static_cast<std::uint8_t>(n >> 16);
    b[at + 2] = static_cast<std::uint8_t>(n >> 8); b[at + 3] = static_cast<std::uint8_t>(n);
}

constexpr int kMaxSide = 30000;   // Photoshop's PSD limit (PSB goes past it)

std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// UTF-8 -> code points (a bad byte as U+FFFD).
std::vector<char32_t> code_points(const std::string& s) {
    std::vector<char32_t> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        char32_t cp = n <= 0 ? c : n == 1 ? (c & 0x1Fu) : n == 2 ? (c & 0x0Fu) : (c & 0x07u);
        bool ok = n >= 0 && i + static_cast<std::size_t>(n) < s.size();
        for (int k = 1; ok && k <= n; ++k) {
            const auto d = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((d >> 6) != 2) ok = false;
            else cp = (cp << 6) | (d & 0x3Fu);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        out.push_back(cp);
        i += static_cast<std::size_t>(n) + 1;
    }
    return out;
}

// Mac Roman's upper half (0x80..0xFF), as the reader's table -- the Pascal
// name Photoshop shows when there is no "luni". Anything else: '?'.
constexpr char16_t kMacRoman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7};
std::string macroman_of(const std::vector<char32_t>& cps) {
    std::string out;
    for (const char32_t cp : cps) {
        if (out.size() >= 255) break;
        if (cp < 0x80) { out.push_back(static_cast<char>(cp)); continue; }
        char c = '?';
        for (int k = 0; k < 128; ++k)
            if (kMacRoman[k] == cp) { c = static_cast<char>(0x80 + k); break; }
        out.push_back(c);
    }
    return out;
}

// A tagged block: "8BIM", key, length, data padded to an even length.
void block(Bytes& b, std::string_view key, const Bytes& data) {
    put(b, "8BIM");
    put(b, key);
    Bytes d = data;
    if (d.size() % 2) d.push_back(0);
    be32(b, static_cast<std::uint32_t>(d.size()));
    put(b, d);
}

// PackBits, one row of bytes.
void packbits(const std::uint8_t* r, std::size_t n, Bytes& e) {
    std::size_t i = 0;
    while (i < n) {
        std::size_t run = 1;
        while (i + run < n && run < 128 && r[i + run] == r[i]) ++run;
        if (run >= 3) {
            e.push_back(static_cast<std::uint8_t>(257 - run));   // 1 - run as a signed byte
            e.push_back(r[i]);
            i += run;
            continue;
        }
        std::size_t lit = 0;
        while (i + lit < n && lit < 128 &&
               !(i + lit + 2 < n && r[i + lit] == r[i + lit + 1] && r[i + lit] == r[i + lit + 2])) ++lit;
        e.push_back(static_cast<std::uint8_t>(lit - 1));
        e.insert(e.end(), r + i, r + i + lit);
        i += lit;
    }
}

// One channel's planar samples (big-endian, w x h of bps bytes) as RLE:
// every row's byte count, then the rows. `with_word`: the compression word
// (1) first, as a layer's channel has; the merged picture writes one word
// for all its channels and its counts together (merged_rle).
Bytes rle(const Bytes& plane, int w, int h, int bps, bool with_word) {
    Bytes out;
    if (with_word) be16(out, 1);
    const std::size_t row = static_cast<std::size_t>(w) * static_cast<std::size_t>(bps);
    std::vector<Bytes> rows(static_cast<std::size_t>(h));
    for (int y = 0; y < h; ++y) packbits(plane.data() + row * static_cast<std::size_t>(y), row, rows[static_cast<std::size_t>(y)]);
    for (const auto& e : rows) be16(out, static_cast<unsigned>(e.size()));
    for (const auto& e : rows) put(out, e);
    return out;
}

struct Box { int top = 0, left = 0, bottom = 0, right = 0; int w() const { return right - left; } int h() const { return bottom - top; } };

// A map's rows split into planes over a box: channel k of each pixel ->
// planes[k], big-endian. `invert` (masks): v -> max - v. The map is W x H at
// 8 or 16 bits, interleaved, native-endian.
std::vector<Bytes> planes_of(const P::TileMap& m, const Box& b, int bps, bool invert) {
    const int ch = m.format().channels();
    const auto px_bytes = static_cast<std::size_t>(m.format().bytes_per_pixel());
    std::vector<Bytes> out(static_cast<std::size_t>(ch));
    const std::size_t n = static_cast<std::size_t>(std::max(0, b.w())) * static_cast<std::size_t>(std::max(0, b.h())) * static_cast<std::size_t>(bps);
    for (auto& pl : out) pl.resize(n);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(m.width()) * px_bytes);
    std::size_t at = 0;
    for (int y = b.top; y < b.bottom; ++y) {
        m.row_bytes(y, row.data());
        for (int x = b.left; x < b.right; ++x) {
            const std::uint8_t* px = row.data() + static_cast<std::size_t>(x) * px_bytes;
            for (int k = 0; k < ch; ++k) {
                auto& pl = out[static_cast<std::size_t>(k)];
                if (bps == 1) {
                    const std::uint8_t v = px[k];
                    pl[at] = invert ? static_cast<std::uint8_t>(255 - v) : v;
                } else {
                    std::uint16_t v;
                    std::memcpy(&v, px + 2 * k, 2);
                    if (invert) v = static_cast<std::uint16_t>(65535 - v);
                    pl[at] = static_cast<std::uint8_t>(v >> 8);
                    pl[at + 1] = static_cast<std::uint8_t>(v);
                }
            }
            at += static_cast<std::size_t>(bps);
        }
    }
    return out;
}

// The smallest box holding every pixel whose channel `k` isn't `none` (as
// stored: 8 or 16 bits, native). Empty (w 0) when there is none.
Box tight(const P::TileMap& m, int k, unsigned none) {
    const int W = m.width(), H = m.height();
    const int bps = m.format().bytes_per_sample();
    const auto px_bytes = static_cast<std::size_t>(m.format().bytes_per_pixel());
    std::vector<std::uint8_t> row(static_cast<std::size_t>(W) * px_bytes);
    Box b{H, W, -1, -1};
    for (int y = 0; y < H; ++y) {
        // A row's tiles all empty / uniform-transparent: skip it without reading.
        bool any_tile = false;
        const int tr = y / P::kTile;
        for (int c = 0; c < m.cols() && !any_tile; ++c) any_tile = m.tile(c, tr) != nullptr;
        if (!any_tile) {
            if (none == 0) continue;   // absent tiles are 0: nothing here
        }
        m.row_bytes(y, row.data());
        int x0 = -1, x1 = -1;
        for (int x = 0; x < W; ++x) {
            const std::uint8_t* s = row.data() + static_cast<std::size_t>(x) * px_bytes + static_cast<std::size_t>(k * bps);
            unsigned v = s[0];
            if (bps == 2) { std::uint16_t u; std::memcpy(&u, s, 2); v = u; }
            if (v != none) { if (x0 < 0) x0 = x; x1 = x; }
        }
        if (x0 < 0) continue;
        b.top = std::min(b.top, y); b.bottom = std::max(b.bottom, y + 1);
        b.left = std::min(b.left, x0); b.right = std::max(b.right, x1 + 1);
    }
    if (b.bottom < 0) return Box{};
    return b;
}

// What a layer becomes in the file, made on a worker.
struct Rec {
    int  section = 0;                // 0 a layer, 1 a group's record, 3 its divider
    std::string name;
    std::string blend = "norm", section_blend;
    int  opacity = 255, fill = 255, clipping = 0, flags = 0;
    Box  box;
    std::vector<std::pair<int, Bytes>> channels;   // id (-1 A, 0 R, 1 G, 2 B, -2 mask) -> its data, word first
    bool mask = false;
    Box  mbox;
    int  mdefault = 255, mflags = 0;
    // To make it (on a worker): the layer and how.
    const Layer* layer = nullptr;
    bool baked = false;
    std::string id;
};

int u8_of(int percent) { return static_cast<int>(std::lround(std::clamp(percent, 0, 100) * 2.55)); }

// An empty channel set (dividers, empty layers): four channels, no data.
void empty_channels(Rec& r) {
    for (const int id : {-1, 0, 1, 2}) {
        Bytes d;
        be16(d, 0);
        r.channels.emplace_back(id, std::move(d));
    }
}

// The rules -- what bakes, what's left out -- in one place, for plan() and
// save() alike, so the dialog and the toast never disagree.
struct Rules {
    int n_adjust = 0, n_filterlayer = 0, n_paths = 0, n_group_fx = 0, n_filters = 0, n_styles = 0, n_text = 0, n_edges = 0;
    int n_outside = 0;
    bool rgb = true, f32 = false;
};
bool bakes(const Layer& L, bool rgb) {
    return !rgb || L.kind == Kind::Text || !L.filters.empty() || L.styles.any();
}
Rules rules(const Image& im, const std::map<Document::Id, Layer>& layers) {
    Rules r;
    r.rgb = im.format.mode == P::Mode::RGB;
    r.f32 = im.format.depth == P::Depth::F32;
    for (const auto& [id, L] : layers) {
        if (id.empty() || id[0] == '@') continue;
        if (L.kind == Kind::Group) {
            if (L.styles.any()) ++r.n_group_fx;
            continue;
        }
        if (L.kind == Kind::Adjustment) { ++r.n_adjust; continue; }
        if (L.kind == Kind::Filter) { ++r.n_filterlayer; continue; }
        if (!draws_pixels(L.kind)) { ++r.n_paths; continue; }
        if (L.kind == Kind::Text) ++r.n_text;
        else {
            if (!L.filters.empty()) ++r.n_filters;
            if (L.styles.any()) ++r.n_styles;
        }
        if (!bakes(L, r.rgb) && L.mask && L.edges.any()) ++r.n_edges;
        if (L.pixels && L.pixels->has_outside()) ++r.n_outside;
    }
    return r;
}

}  // namespace

Plan plan(const Image& im, const std::map<Document::Id, Layer>& layers) {
    const Rules r = rules(im, layers);
    Plan pl;
    if (r.n_text) pl.baked.push_back(plural(r.n_text, "text layer", "text layers") + " as pixels (named, not live type)");
    if (r.n_filters) pl.baked.push_back(plural(r.n_filters, "layer's live filters", "layers' live filters"));
    if (r.n_styles) pl.baked.push_back(plural(r.n_styles, "layer's styles", "layers' styles"));
    if (r.n_edges) pl.baked.push_back(plural(r.n_edges, "mask's Density / Feather", "masks' Density / Feather") + " into the mask");
    if (r.n_adjust) pl.left_out.push_back(plural(r.n_adjust, "adjustment layer", "adjustment layers") + " left out");
    if (r.n_filterlayer) pl.left_out.push_back(plural(r.n_filterlayer, "filter layer", "filter layers") + " left out");
    if (r.n_paths) pl.left_out.push_back(plural(r.n_paths, "path layer", "path layers") + " left out");
    if (r.n_group_fx) pl.left_out.push_back(plural(r.n_group_fx, "group's styles", "groups' styles") + " left out");
    if (r.n_outside) pl.left_out.push_back(plural(r.n_outside, "layer's pixels", "layers' pixels") + " beyond the canvas cut off");
    if (!r.rgb) pl.left_out.push_back("written as RGB (every layer drawn as RGB)");
    if (r.f32) pl.left_out.push_back("32-bit written as 16-bit");
    return pl;
}

bool save(const std::string& path, const Image& im, const std::map<Document::Id, Layer>& layers,
          std::string& error, std::vector<std::string>& notes, const formats::Progress& p) {
    const int W = im.width, H = im.height;
    if (W < 1 || H < 1) { error = "an empty image"; return false; }
    if (W > kMaxSide || H > kMaxSide) {
        error = "larger than Photoshop's 30,000 px a side (" + std::to_string(W) + " x " + std::to_string(H) +
                ") -- keep it as .klimt, or export a TIFF";
        return false;
    }
    const bool deep = im.format.depth != P::Depth::U8;
    const int bps = deep ? 2 : 1;
    const P::Format fmt{deep ? P::Depth::U16 : P::Depth::U8, P::Mode::RGB, true};
    const P::Format mfmt{fmt.depth, P::Mode::Gray, false};
    const bool rgb = im.format.mode == P::Mode::RGB;
    formats::Progress cancel_only;
    cancel_only.cancelled = p.cancelled;

    // The tree, bottom first.
    std::map<std::string, std::vector<std::pair<double, std::string>>> kids;
    for (const auto& [id, L] : layers)
        if (!id.empty() && id[0] != '@') kids[L.parent].emplace_back(L.pos, id);
    for (auto& [par, v] : kids) std::sort(v.begin(), v.end());

    std::vector<Rec> recs;
    std::function<void(const std::string&)> walk = [&](const std::string& parent) {
        const auto it = kids.find(parent);
        if (it == kids.end()) return;
        for (const auto& [pos, id] : it->second) {
            const Layer& L = layers.at(id);
            if (L.kind == Kind::Group) {
                Rec div;
                div.section = 3;
                div.name = "</Layer group>";
                empty_channels(div);
                recs.push_back(std::move(div));
                walk(id);
                Rec g;
                g.section = 1;
                g.name = L.name;
                g.blend = L.blend == Blend::PassThrough ? "pass" : std::string(key_of_blend(L.blend));
                g.section_blend = g.blend;
                g.opacity = u8_of(L.opacity);
                g.clipping = L.clipped ? 1 : 0;
                g.flags = L.visible ? 0 : 2;
                g.layer = &L;
                g.id = id;
                recs.push_back(std::move(g));
                continue;
            }
            if (!draws_pixels(L.kind)) continue;   // adjustment / filter / path: plan() says
            Rec r;
            r.name = L.name;
            r.blend = std::string(key_of_blend(L.blend == Blend::PassThrough ? Blend::Normal : L.blend));
            r.opacity = u8_of(L.opacity);
            r.clipping = L.clipped ? 1 : 0;
            r.flags = L.visible ? 0 : 2;
            r.layer = &L;
            r.baked = bakes(L, rgb);
            r.fill = r.baked ? 255 : u8_of(L.fill);
            r.id = id;
            recs.push_back(std::move(r));
        }
    };
    walk("");
    if (recs.empty()) {   // nothing drawable: one empty layer, so it opens layered
        Rec r;
        r.name = "Layer 1";
        empty_channels(r);
        recs.push_back(std::move(r));
    }

    // Each layer's channels, on the cores: drawn (baked) or read, cut to its
    // pixels' box, its mask's, RLE.
    std::atomic<bool> failed{false};
    std::atomic<int> done{0};
    std::vector<std::size_t> todo;
    for (std::size_t i = 0; i < recs.size(); ++i)
        if (recs[i].layer && (recs[i].section == 0 || (recs[i].section == 1 && recs[i].layer->mask))) todo.push_back(i);
    const auto make = [&](Rec& r) {
        const Layer& L = *r.layer;
        // The mask (groups too), Photoshop's way: white shows.
        if (L.mask && !r.baked) {
            auto em = L.kind == Kind::Group ? L.mask : live::edged_mask(L.mask, L.edges);
            P::TileMap m = em->without_outside();
            if (!(m.format() == mfmt)) m = transfer::converted(m, mfmt);
            // Klimt's mask holds what's HIDDEN; where it hides nothing, Photoshop's default (white).
            Box b = tight(m, 0, 0);
            r.mask = true;
            r.mdefault = 255;
            r.mflags = L.mask_on ? 0 : 2;
            if (b.w() <= 0) b = Box{0, 0, H, W};   // a mask that hides nothing: kept, whole
            r.mbox = b;
            auto pl = planes_of(m, b, bps, true);
            r.channels.emplace_back(-2, rle(pl[0], b.w(), b.h(), bps, true));
        }
        if (r.section != 0) {
            empty_channels(r);
            // The mask goes after the colour channels, as Photoshop orders them.
            std::rotate(r.channels.begin(), r.channels.begin() + 1, r.channels.end());
            return;
        }
        P::TileMap m;
        if (r.baked) {
            Layer solo = L;
            solo.parent = "";
            solo.pos = 1;
            solo.visible = true;
            solo.opacity = 100;
            solo.blend = Blend::Normal;
            solo.clipped = false;
            const std::map<Document::Id, Layer> alone{{r.id, solo}};
            auto flat = file::flatten(im, alone, cancel_only);
            if (!flat) { failed = true; return; }
            m = flat->format() == fmt ? std::move(*flat) : transfer::converted(*flat, fmt);
        } else if (L.pixels) {
            const auto px = (L.mask && L.edges.defringe > 0) ? live::defringed_pixels(L) : L.pixels;
            m = px->without_outside();
            if (!(m.format() == fmt)) m = transfer::converted(m, fmt);
        }
        Box b = m.width() > 0 ? tight(m, 3, 0) : Box{};
        Bytes mask_ch;
        if (r.mask) { mask_ch = std::move(r.channels.back().second); r.channels.clear(); }
        if (b.w() <= 0) {
            r.box = Box{};
            empty_channels(r);
        } else {
            r.box = b;
            auto pl = planes_of(m, b, bps, false);
            // R G B A in the map -> Photoshop's order: A (-1), R, G, B.
            r.channels.emplace_back(-1, rle(pl[3], b.w(), b.h(), bps, true));
            for (int k = 0; k < 3; ++k) r.channels.emplace_back(k, rle(pl[static_cast<std::size_t>(k)], b.w(), b.h(), bps, true));
        }
        if (r.mask) r.channels.emplace_back(-2, std::move(mask_ch));
    };
    {
        std::atomic<std::size_t> next{0};
        const unsigned nt = static_cast<unsigned>(std::min<std::size_t>(std::max<std::size_t>(todo.size(), 1),
                                                                        std::max(1u, std::thread::hardware_concurrency())));
        const auto work = [&] {
            for (;;) {
                const std::size_t k = next++;
                if (k >= todo.size() || failed || p.stop()) return;
                make(recs[todo[k]]);
                ++done;
            }
        };
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < nt; ++t) pool.emplace_back(work);
        // This thread works too, and says how far along it is.
        for (;;) {
            const std::size_t k = next++;
            if (k >= todo.size() || failed || p.stop()) break;
            make(recs[todo[k]]);
            ++done;
            p.at(0.7 * done / static_cast<double>(std::max<std::size_t>(todo.size(), 1)));
        }
        for (auto& t : pool) t.join();
    }
    if (failed || p.stop()) { error = "cancelled"; return false; }

    // ── the layer info ──────────────────────────────────────────────────────
    Bytes info;
    // Negative count: the merged picture's first extra channel is its transparency.
    be16(info, static_cast<unsigned>(static_cast<std::uint16_t>(-static_cast<int>(recs.size()))));
    for (const auto& r : recs) {
        be32(info, static_cast<std::uint32_t>(r.box.top)); be32(info, static_cast<std::uint32_t>(r.box.left));
        be32(info, static_cast<std::uint32_t>(r.box.bottom)); be32(info, static_cast<std::uint32_t>(r.box.right));
        be16(info, static_cast<unsigned>(r.channels.size()));
        for (const auto& [cid, data] : r.channels) {
            be16(info, static_cast<unsigned>(static_cast<std::uint16_t>(cid)));
            be32(info, static_cast<std::uint32_t>(data.size()));
        }
        put(info, "8BIM");
        put(info, r.blend.size() == 4 ? r.blend : std::string("norm"));
        info.push_back(static_cast<std::uint8_t>(r.opacity));
        info.push_back(static_cast<std::uint8_t>(r.clipping));
        // Bit 3 says bit 4 is meaningful; bit 4: pixel data irrelevant to the look (a divider).
        info.push_back(static_cast<std::uint8_t>(r.flags | 0x08 | (r.section == 3 ? 0x10 : 0)));
        info.push_back(0);
        const std::size_t extra = hole(info);
        if (r.mask) {
            be32(info, 20);
            be32(info, static_cast<std::uint32_t>(r.mbox.top)); be32(info, static_cast<std::uint32_t>(r.mbox.left));
            be32(info, static_cast<std::uint32_t>(r.mbox.bottom)); be32(info, static_cast<std::uint32_t>(r.mbox.right));
            info.push_back(static_cast<std::uint8_t>(r.mdefault));
            info.push_back(static_cast<std::uint8_t>(r.mflags));
            be16(info, 0);
        } else {
            be32(info, 0);
        }
        // Blending ranges: gray and the three channels, all passing (Photoshop's defaults).
        be32(info, 40);
        for (int k = 0; k < 5; ++k) { be16(info, 0x0000); be16(info, 0xFFFF); be16(info, 0x0000); be16(info, 0xFFFF); }
        const auto cps = code_points(r.name);
        {
            const std::string pas = macroman_of(cps);
            const std::size_t at = info.size();
            info.push_back(static_cast<std::uint8_t>(pas.size()));
            put(info, pas);
            pad_to(info, at, 4);
        }
        {   // luni: the real name, UTF-16BE
            Bytes u;
            std::vector<char16_t> w16;
            for (const char32_t cp : cps) {
                if (cp >= 0x10000) {
                    const char32_t v = cp - 0x10000;
                    w16.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
                    w16.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
                } else {
                    w16.push_back(static_cast<char16_t>(cp));
                }
            }
            be32(u, static_cast<std::uint32_t>(w16.size()));
            for (const char16_t c : w16) be16(u, c);
            while (u.size() % 4) u.push_back(0);
            block(info, "luni", u);
        }
        if (r.section) {
            Bytes s;
            be32(s, static_cast<std::uint32_t>(r.section));
            if (r.section == 1) { put(s, "8BIM"); put(s, r.section_blend.size() == 4 ? r.section_blend : std::string("pass")); }
            block(info, "lsct", s);
        }
        if (r.section == 0 && r.fill != 255) {
            Bytes f{static_cast<std::uint8_t>(r.fill), 0, 0, 0};
            block(info, "iOpa", f);
        }
        fill(info, extra);
    }
    for (const auto& r : recs)
        for (const auto& [cid, data] : r.channels) put(info, data);
    pad_to(info, 0, 4);
    p.at(0.75);

    // ── the merged picture: what the eye sees, matted with white where it's
    // transparent (what Photoshop writes; readers take the matte out) ──────
    auto flat = file::flatten(im, layers, cancel_only);
    if (!flat) { error = "cancelled"; return false; }
    P::TileMap merged = flat->format() == fmt ? std::move(*flat) : transfer::converted(*flat, fmt);
    auto mp = planes_of(merged, Box{0, 0, H, W}, bps, false);
    {
        const std::size_t n = static_cast<std::size_t>(W) * static_cast<std::size_t>(H);
        for (std::size_t i = 0; i < n; ++i) {
            if (bps == 1) {
                const unsigned a = mp[3][i];
                if (a == 255) continue;
                for (int k = 0; k < 3; ++k) {
                    auto& v = mp[static_cast<std::size_t>(k)][i];
                    v = static_cast<std::uint8_t>((v * a + 255 * (255 - a) + 127) / 255);
                }
            } else {
                const unsigned a = (mp[3][2 * i] << 8) | mp[3][2 * i + 1];
                if (a == 65535) continue;
                for (int k = 0; k < 3; ++k) {
                    auto& pl = mp[static_cast<std::size_t>(k)];
                    const std::uint64_t v = (pl[2 * i] << 8) | pl[2 * i + 1];
                    const auto m = static_cast<unsigned>((v * a + 65535ull * (65535 - a) + 32767) / 65535);
                    pl[2 * i] = static_cast<std::uint8_t>(m >> 8);
                    pl[2 * i + 1] = static_cast<std::uint8_t>(m);
                }
            }
        }
    }
    p.at(0.85);

    // ── the file ────────────────────────────────────────────────────────────
    Bytes out;
    out.reserve(info.size() + mp[0].size() * 2 + 1024);
    put(out, "8BPS");
    be16(out, 1);
    out.insert(out.end(), 6, 0);
    be16(out, 4);   // R G B + the merged transparency
    be32(out, static_cast<std::uint32_t>(H));
    be32(out, static_cast<std::uint32_t>(W));
    be16(out, static_cast<unsigned>(bps * 8));
    be16(out, 3);   // RGB
    be32(out, 0);   // colour mode data
    {
        const std::size_t res = hole(out);
        const auto resource = [&](int id, const Bytes& data) {
            put(out, "8BIM");
            be16(out, static_cast<unsigned>(id));
            be16(out, 0);   // an empty name, padded
            be32(out, static_cast<std::uint32_t>(data.size()));
            put(out, data);
            if (data.size() % 2) out.push_back(0);
        };
        Bytes ri;   // ResolutionInfo: h / v res as 16.16 pixels per inch, shown as inches
        const auto fixed = static_cast<std::uint32_t>(std::lround(std::clamp(im.dpi, 1.0, 10000.0) * 65536.0));
        be32(ri, fixed); be16(ri, 1); be16(ri, 1);
        be32(ri, fixed); be16(ri, 1); be16(ri, 1);
        resource(1005, ri);
        if (im.icc && !im.icc->empty() && rgb) resource(1039, Bytes(im.icc->begin(), im.icc->end()));
        fill(out, res);
    }
    {
        const std::size_t lm = hole(out);
        if (!deep) {
            be32(out, static_cast<std::uint32_t>(info.size()));
            put(out, info);
            be32(out, 0);   // the global layer mask: none
        } else {
            be32(out, 0);   // 16-bit: an empty layer info here, the layers in Lr16
            be32(out, 0);
            put(out, "8BIM");
            put(out, "Lr16");
            be32(out, static_cast<std::uint32_t>(info.size()));
            put(out, info);
        }
        fill(out, lm);
    }
    // The merged picture: one compression word, every row's count for all
    // four channels, then all the rows.
    be16(out, 1);
    {
        std::vector<Bytes> chans;
        for (int k = 0; k < 4; ++k) chans.push_back(rle(mp[static_cast<std::size_t>(k)], W, H, bps, false));
        // Each `chans[k]` is its counts then its rows: counts of all first.
        const std::size_t counts = static_cast<std::size_t>(H) * 2;
        for (const auto& c : chans) out.insert(out.end(), c.begin(), c.begin() + static_cast<std::ptrdiff_t>(counts));
        for (const auto& c : chans) out.insert(out.end(), c.begin() + static_cast<std::ptrdiff_t>(counts), c.end());
    }
    if (p.stop()) { error = "cancelled"; return false; }
    p.at(0.95);

    const std::string tmp = path + ".partial";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { error = "can't be written"; return false; }
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!f) { std::remove(tmp.c_str()); error = "can't be written (the disk full?)"; return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::remove(tmp.c_str()); error = "can't be written: " + ec.message(); return false; }
    for (auto& n : plan(im, layers).left_out) notes.push_back(std::move(n));
    p.at(1);
    return true;
}

}  // namespace klimt::core::psd
