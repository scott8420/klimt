// klimt_selftest -- s050: Photoshop documents in. A small PSD WRITER lives
// here (test-only) so every corner the reader handles is built on purpose:
// raw / RLE / ZIP / ZIP-with-prediction channels, 8 and 16 bit, 16-bit layers
// in an Lr16 block (as Photoshop writes them), groups (open, closed, Pass
// Through), clipping, masks (default colour, disabled), Fill (iOpa), Unicode
// names (luni), and the blocks Klimt leaves out or reads as pixels.
// (Checked by hand against psd-tools' and ImageMagick's PSDs too: the
// composite agrees within a level -- docs/file-format.md.)
#include "core/Composite.hpp"
#include "core/File.hpp"
#include "core/Psd.hpp"
#include "formats/Formats.hpp"
#include "pixel/Pixel.hpp"

#include <zlib.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

namespace P    = klimt::pixel;
namespace Fm   = klimt::formats;
namespace core = klimt::core;
namespace psd  = klimt::core::psd;
namespace fs   = std::filesystem;

using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
fs::path scratch() {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_psd_" + std::to_string(::getpid()));
        fs::create_directories(p);
        return p;
    }();
    return d;
}
std::string at(const std::string& name) { return (scratch() / name).string(); }

using Bytes = std::vector<std::uint8_t>;
void be16(Bytes& b, unsigned v) { b.push_back(static_cast<std::uint8_t>(v >> 8)); b.push_back(static_cast<std::uint8_t>(v)); }
void be32(Bytes& b, std::uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }
void put(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
void put(Bytes& b, const Bytes& s) { b.insert(b.end(), s.begin(), s.end()); }

struct Box { int top = 0, left = 0, bottom = 0, right = 0; int w() const { return right - left; } int h() const { return bottom - top; } };

// One plane: samples 0..max (255 or 65535), row-major over its box.
using Plane = std::vector<unsigned>;

// A plane's channel data, compression word first.
Bytes channel(const Plane& pl, int w, int h, int bps, int comp) {
    Bytes raw;
    for (const unsigned v : pl) { if (bps == 2) be16(raw, v); else raw.push_back(static_cast<std::uint8_t>(v)); }
    Bytes out;
    be16(out, static_cast<unsigned>(comp));
    const std::size_t row = static_cast<std::size_t>(w) * static_cast<std::size_t>(bps);
    if (comp == 0) { put(out, raw); return out; }
    if (comp == 1) {   // PackBits: a run where 3+ repeat, else literals
        std::vector<Bytes> rows;
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* r = raw.data() + row * static_cast<std::size_t>(y);
            Bytes e;
            std::size_t i = 0;
            while (i < row) {
                std::size_t run = 1;
                while (i + run < row && run < 128 && r[i + run] == r[i]) ++run;
                if (run >= 3) { e.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(1 - static_cast<int>(run)))); e.push_back(r[i]); i += run; continue; }
                std::size_t lit = 0;
                while (i + lit < row && lit < 128 && !(i + lit + 2 < row && r[i + lit] == r[i + lit + 1] && r[i + lit] == r[i + lit + 2])) ++lit;
                e.push_back(static_cast<std::uint8_t>(lit - 1));
                e.insert(e.end(), r + i, r + i + lit);
                i += lit;
            }
            rows.push_back(e);
        }
        for (const auto& e : rows) be16(out, static_cast<unsigned>(e.size()));
        for (const auto& e : rows) put(out, e);
        return out;
    }
    if (comp == 3) {   // the difference from the sample before, per row
        for (int y = 0; y < h; ++y) {
            std::uint8_t* r = raw.data() + row * static_cast<std::size_t>(y);
            if (bps == 1) for (int x = w - 1; x > 0; --x) r[x] = static_cast<std::uint8_t>(r[x] - r[x - 1]);
            else
                for (int x = w - 1; x > 0; --x) {
                    const unsigned a = (r[2 * x] << 8) | r[2 * x + 1], b = (r[2 * x - 2] << 8) | r[2 * x - 1];
                    const unsigned d = (a - b) & 0xFFFF;
                    r[2 * x] = static_cast<std::uint8_t>(d >> 8);
                    r[2 * x + 1] = static_cast<std::uint8_t>(d);
                }
        }
    }
    uLongf n = compressBound(static_cast<uLong>(raw.size()));
    Bytes z(n);
    compress(z.data(), &n, raw.data(), static_cast<uLong>(raw.size()));
    z.resize(n);
    put(out, z);
    return out;
}

struct WLayer {
    Box box;
    Plane r, g, b, a;          // empty a: no alpha channel
    std::string name, uname;   // uname: a luni block
    std::string blend = "norm";
    int opacity = 255, clipping = 0, flags = 0;
    int section = 0;           // lsct
    std::string section_blend;
    int fill = -1;             // iOpa
    bool mask = false;
    Box mbox;
    Plane m;
    int mdefault = 0, mflags = 0;
    std::vector<std::string> blocks;   // empty tagged blocks: "levl", "lfx2", "TySh"
    int comp = 1;
};

WLayer solid(Box b, unsigned r, unsigned g, unsigned bl, unsigned a, unsigned max) {
    WLayer L;
    L.box = b;
    const std::size_t n = static_cast<std::size_t>(b.w()) * static_cast<std::size_t>(b.h());
    L.r.assign(n, r * max / 255); L.g.assign(n, g * max / 255); L.b.assign(n, bl * max / 255); L.a.assign(n, a * max / 255);
    return L;
}

Bytes layer_info(const std::vector<WLayer>& layers, int bps) {
    Bytes recs, data;
    be16(recs, static_cast<unsigned>(layers.size()));
    for (const auto& L : layers) {
        be32(recs, static_cast<std::uint32_t>(L.box.top)); be32(recs, static_cast<std::uint32_t>(L.box.left));
        be32(recs, static_cast<std::uint32_t>(L.box.bottom)); be32(recs, static_cast<std::uint32_t>(L.box.right));
        std::vector<std::pair<int, Bytes>> chans;
        const int w = std::max(0, L.box.w()), h = std::max(0, L.box.h());
        if (L.section == 0) {
            if (!L.a.empty()) chans.emplace_back(-1, channel(L.a, w, h, bps, L.comp));
            chans.emplace_back(0, channel(L.r, w, h, bps, L.comp));
            chans.emplace_back(1, channel(L.g, w, h, bps, L.comp));
            chans.emplace_back(2, channel(L.b, w, h, bps, L.comp));
        } else {
            for (const int id : {-1, 0, 1, 2}) chans.emplace_back(id, channel({}, 0, 0, bps, 0));
        }
        if (L.mask) chans.emplace_back(-2, channel(L.m, L.mbox.w(), L.mbox.h(), bps, L.comp));
        be16(recs, static_cast<unsigned>(chans.size()));
        for (const auto& [id, d] : chans) { be16(recs, static_cast<unsigned>(id) & 0xFFFF); be32(recs, static_cast<std::uint32_t>(d.size())); put(data, d); }
        put(recs, "8BIM");
        put(recs, L.blend);
        recs.push_back(static_cast<std::uint8_t>(L.opacity));
        recs.push_back(static_cast<std::uint8_t>(L.clipping));
        recs.push_back(static_cast<std::uint8_t>(L.flags));
        recs.push_back(0);
        Bytes extra;
        if (L.mask) {
            be32(extra, 20);
            be32(extra, static_cast<std::uint32_t>(L.mbox.top)); be32(extra, static_cast<std::uint32_t>(L.mbox.left));
            be32(extra, static_cast<std::uint32_t>(L.mbox.bottom)); be32(extra, static_cast<std::uint32_t>(L.mbox.right));
            extra.push_back(static_cast<std::uint8_t>(L.mdefault));
            extra.push_back(static_cast<std::uint8_t>(L.mflags));
            be16(extra, 0);
        } else {
            be32(extra, 0);
        }
        be32(extra, 0);   // blending ranges
        const std::size_t name_at = extra.size();
        extra.push_back(static_cast<std::uint8_t>(L.name.size()));
        put(extra, L.name);
        while ((extra.size() - name_at) % 4) extra.push_back(0);
        const auto block = [&](const std::string& key, const Bytes& d) {
            put(extra, "8BIM"); put(extra, key);
            Bytes dd = d;
            while (dd.size() % 2) dd.push_back(0);
            be32(extra, static_cast<std::uint32_t>(dd.size()));
            put(extra, dd);
        };
        if (!L.uname.empty()) {   // ASCII test names: UTF-16BE is a zero then the byte
            Bytes u;
            be32(u, static_cast<std::uint32_t>(L.uname.size()));
            for (const char c : L.uname) be16(u, static_cast<unsigned char>(c));
            block("luni", u);
        }
        if (L.section) {
            Bytes s;
            be32(s, static_cast<std::uint32_t>(L.section));
            if (!L.section_blend.empty()) { put(s, "8BIM"); put(s, L.section_blend); }
            block("lsct", s);
        }
        if (L.fill >= 0) block("iOpa", {static_cast<std::uint8_t>(L.fill), 0, 0, 0});
        for (const auto& k : L.blocks) block(k, Bytes(4, 0));
        be32(recs, static_cast<std::uint32_t>(extra.size()));
        put(recs, extra);
    }
    Bytes li = recs;
    put(li, data);
    if (li.size() % 2) li.push_back(0);
    return li;
}

struct WDoc {
    int W = 64, H = 48, bps = 1, mode = 3, version = 1;
    bool lr16 = false;       // 16-bit layers in Lr16 (Photoshop's way)
    double dpi = 0;          // 0: no ResolutionInfo
    Bytes icc;
    std::vector<WLayer> layers;
    std::vector<Plane> merged;   // RGB(+extra) planes for a file with no layers
    int merged_comp = 1;
};

Bytes psd_bytes(const WDoc& d) {
    Bytes f;
    put(f, "8BPS");
    be16(f, static_cast<unsigned>(d.version));
    f.insert(f.end(), 6, 0);
    be16(f, d.merged.empty() ? 3 : static_cast<unsigned>(d.merged.size()));
    be32(f, static_cast<std::uint32_t>(d.H));
    be32(f, static_cast<std::uint32_t>(d.W));
    be16(f, static_cast<unsigned>(d.bps * 8));
    be16(f, static_cast<unsigned>(d.mode));
    be32(f, 0);   // colour mode data
    Bytes res;
    const auto resource = [&](unsigned id, const Bytes& data) {
        put(res, "8BIM"); be16(res, id); be16(res, 0);   // empty name, padded
        be32(res, static_cast<std::uint32_t>(data.size()));
        put(res, data);
        if (data.size() % 2) res.push_back(0);
    };
    if (d.dpi > 0) {
        Bytes r;
        be32(r, static_cast<std::uint32_t>(std::lround(d.dpi * 65536))); be16(r, 1); be16(r, 1);
        be32(r, static_cast<std::uint32_t>(std::lround(d.dpi * 65536))); be16(r, 1); be16(r, 1);
        resource(1005, r);
    }
    if (!d.icc.empty()) resource(1039, d.icc);
    be32(f, static_cast<std::uint32_t>(res.size()));
    put(f, res);
    Bytes lm;
    if (!d.layers.empty()) {
        const Bytes li = layer_info(d.layers, d.bps);
        if (d.lr16) {
            be32(lm, 0);   // empty layer info
            be32(lm, 0);   // global mask
            put(lm, "8BIM"); put(lm, "Lr16");
            Bytes l4 = li;
            while (l4.size() % 4) l4.push_back(0);
            be32(lm, static_cast<std::uint32_t>(l4.size()));
            put(lm, l4);
        } else {
            be32(lm, static_cast<std::uint32_t>(li.size()));
            put(lm, li);
            be32(lm, 0);
        }
    }
    be32(f, static_cast<std::uint32_t>(lm.size()));
    put(f, lm);
    // The merged picture.
    be16(f, static_cast<unsigned>(d.merged.empty() ? 0 : d.merged_comp));
    if (d.merged.empty()) {
        f.insert(f.end(), static_cast<std::size_t>(d.W) * static_cast<std::size_t>(d.H) * static_cast<std::size_t>(d.bps) * 3, 0);
    } else if (d.merged_comp == 1) {
        Bytes counts, rows;
        for (const auto& pl : d.merged) {
            const Bytes c = channel(pl, d.W, d.H, d.bps, 1);   // its own counts then rows
            counts.insert(counts.end(), c.begin() + 2, c.begin() + 2 + 2 * d.H);
            rows.insert(rows.end(), c.begin() + 2 + 2 * d.H, c.end());
        }
        put(f, counts);
        put(f, rows);
    } else {
        for (const auto& pl : d.merged) { const Bytes c = channel(pl, d.W, d.H, d.bps, 0); f.insert(f.end(), c.begin() + 2, c.end()); }
    }
    return f;
}

bool write(const std::string& path, const Bytes& b) {
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(o);
}

const core::Layer* named(const core::file::Contents& c, const std::string& n) {
    for (const auto& [id, L] : c.layers) if (L.name == n) return &L;
    return nullptr;
}
std::string id_of(const core::file::Contents& c, const std::string& n) {
    for (const auto& [id, L] : c.layers) if (L.name == n) return id;
    return "";
}
std::string all_notes(const core::file::Contents& c) {
    std::string s;
    for (const auto& n : c.notes) s += n + "; ";
    return s;
}
bool near(double a, double b, double tol = 1.0 / 255 + 1e-6) { return std::abs(a - b) <= tol; }
}  // namespace

void selftest_psd(const Check& check) {
    // ── The blend keys, every Klimt mode both ways.
    {
        bool all = true;
        for (const auto& m : core::blend_modes()) {
            const auto k = psd::key_of_blend(m.mode);
            all = all && k.size() == 4 && psd::blend_of_key(k) == m.mode;
        }
        check("psd: every blend mode has its Photoshop key, and back", all && psd::blend_of_key("pass") == core::Blend::PassThrough &&
                                                                       !psd::blend_of_key("zzzz"), "");
    }

    // ── 8-bit: a tree with everything.
    {
        const unsigned M = 255;
        WDoc d;
        d.W = 64; d.H = 48; d.dpi = 300;
        d.icc = Bytes{1, 2, 3, 4, 5};
        WLayer bg = solid({0, 0, 48, 64}, 250, 240, 220, 255, M);
        bg.name = "Paper"; bg.comp = 0;   // raw
        WLayer end; end.section = 3; end.name = "</Layer group>"; end.box = {}; end.comp = 0;
        WLayer inner = solid({8, 4, 40, 30}, 10, 200, 30, 255, M);   // crosses nothing; RLE
        inner.name = "Green";
        inner.mask = true; inner.mbox = {10, 6, 20, 16}; inner.mdefault = 255;
        inner.m.assign(static_cast<std::size_t>(inner.mbox.w() * inner.mbox.h()), 0);   // black inside: hidden there
        WLayer clip = solid({0, 0, 48, 64}, 200, 20, 20, 255, M);
        clip.name = "Red"; clip.clipping = 1; clip.comp = 2;   // ZIP
        WLayer folder; folder.section = 2; folder.section_blend = "pass"; folder.blend = "pass"; folder.name = "Folder";
        folder.uname = "Folder U"; folder.opacity = 128; folder.comp = 0;
        WLayer hidden = solid({-10, 50, 20, 80}, 0, 0, 255, 128, M);   // beyond the canvas; ZIP with prediction
        hidden.name = "Blue"; hidden.flags = 2; hidden.blend = "mul "; hidden.fill = 102; hidden.comp = 3;
        hidden.mask = true; hidden.mbox = {0, 50, 10, 60}; hidden.mflags = 2;   // a disabled mask
        hidden.m.assign(100, 255);
        WLayer adj; adj.name = "Levels 1"; adj.blocks = {"levl"}; adj.box = {}; adj.comp = 0;
        adj.r.clear(); adj.a.clear();
        WLayer text = solid({30, 30, 40, 60}, 0, 0, 0, 255, M);
        text.name = "Title"; text.blocks = {"TySh", "lfx2"}; text.blend = "scrn";
        d.layers = {bg, end, inner, clip, folder, hidden, adj, text};   // bottom first
        const std::string f = at("tree8.psd");
        write(f, psd_bytes(d));
        check("psd: sniffs as Photoshop", Fm::sniff(f) == Fm::Kind::PSD, "");
        std::string err;
        const auto c = core::file::open(f, err);
        check("psd 8: opens", c.has_value(), err);
        if (c) {
            const auto* P0 = named(*c, "Paper");
            const auto* G = named(*c, "Folder U");
            const auto* Gr = named(*c, "Green");
            const auto* R = named(*c, "Red");
            const auto* B = named(*c, "Blue");
            const auto* T = named(*c, "Title");
            check("psd 8: the layers and the group, the adjustment left out",
                  P0 && G && Gr && R && B && T && !named(*c, "Levels 1") && c->layers.size() == 6, std::to_string(c->layers.size()));
            check("psd 8: 8-bit RGB with alpha, 300 dpi, the profile",
                  c->image.format == P::Format{P::Depth::U8, P::Mode::RGB, true} && near(c->image.dpi, 300, 0.01) && c->image.icc &&
                      *c->image.icc == Bytes({1, 2, 3, 4, 5}), std::to_string(c->image.dpi));
            if (P0 && G && Gr && R && B && T) {
                const std::string gid = id_of(*c, "Folder U");
                check("psd 8: the group -- Unicode name, Pass Through, opacity 50, its children inside, in order",
                      G->kind == core::Kind::Group && G->blend == core::Blend::PassThrough && G->opacity == 50 &&
                          Gr->parent == gid && R->parent == gid && Gr->pos < R->pos && G->parent.empty(), "");
                check("psd 8: the top level bottom -> top: Paper, Folder, Blue, Title",
                      P0->pos < G->pos && G->pos < B->pos && B->pos < T->pos, "");
                check("psd 8: clipping kept", R->clipped && !Gr->clipped, "");
                check("psd 8: hidden, Multiply, Fill 40", !B->visible && B->blend == core::Blend::Multiply && B->fill == 40, std::to_string(B->fill));
                check("psd 8: Screen", T->blend == core::Blend::Screen, "");
                // Pixels at their places (raw, RLE, ZIP, ZIP + prediction).
                const auto px = [](const core::Layer* L, int x, int y) { return L->pixels->get(x, y); };
                check("psd 8: raw channels", near(px(P0, 3, 3).c[0], 250 / 255.0) && near(px(P0, 63, 47).c[2], 220 / 255.0), "");
                check("psd 8: RLE channels at the layer's place, nothing outside it",
                      near(px(Gr, 4, 8).c[1], 200 / 255.0) && near(px(Gr, 29, 39).c[3], 1) && px(Gr, 3, 8).c[3] == 0 && px(Gr, 4, 40).c[3] == 0, "");
                check("psd 8: ZIP channels", near(px(R, 40, 40).c[0], 200 / 255.0), "");
                check("psd 8: ZIP-with-prediction channels, alpha kept, cut at the edge",
                      near(px(B, 55, 5).c[2], 1) && near(px(B, 55, 5).c[3], 128 / 255.0) && px(B, 49, 5).c[3] == 0, "");
                // Masks: Klimt's holds what is HIDDEN.
                check("psd 8: a mask -- black inside its box hides, its default (white) outside shows",
                      Gr->mask && Gr->mask_on && near(Gr->mask->get(8, 12).c[0], 1) && near(Gr->mask->get(30, 30).c[0], 0), "");
                check("psd 8: a disabled mask comes in switched off", B->mask && !B->mask_on, "");
                // The composite: Green is hidden by its mask at (8, 12); Red clipped to Green shows where Green does.
                std::vector<float> t;
                core::composite_tile(c->image, c->layers, 0, 0, t);
                const auto at_px = [&](int x, int y, int k) { return t[(static_cast<std::size_t>(y) * 256 + static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(k)]; };
                check("psd 8: the composite -- clipped Red over Green at half the group's opacity, Paper where the mask hides",
                      near(at_px(20, 30, 0), 0.5 * 200 / 255.0 + 0.5 * 250 / 255.0, 0.01) && near(at_px(8, 12, 0), 250 / 255.0, 0.01) &&
                          near(at_px(40, 40, 0), 250 / 255.0, 0.01),
                      std::to_string(at_px(20, 30, 0)) + " " + std::to_string(at_px(8, 12, 0)));
            }
            const std::string said = all_notes(*c);
            check("psd 8: the toast says what didn't cross",
                  said.find("1 adjustment / fill layer left out") != std::string::npos &&
                      said.find("1 layer's styles left out") != std::string::npos &&
                      said.find("1 text layer read as pixels") != std::string::npos &&
                      said.find("1 layer cut at the canvas edge") != std::string::npos, said);
        }
    }

    // ── 16-bit, its layers in Lr16 as Photoshop writes them.
    {
        const unsigned M = 65535;
        for (const int comp : {1, 3}) {
            WDoc d;
            d.W = 40; d.H = 30; d.bps = 2; d.lr16 = true;
            WLayer a = solid({0, 0, 30, 40}, 0, 0, 0, 255, M);
            a.name = "Fine";
            a.comp = comp;
            for (std::size_t i = 0; i < a.r.size(); ++i) a.r[i] = static_cast<unsigned>(i * 37 % 65536);   // every level differs
            d.layers = {a};
            const std::string f = at("deep" + std::to_string(comp) + ".psd");
            write(f, psd_bytes(d));
            std::string err;
            const auto c = core::file::open(f, err);
            bool exact = c && named(*c, "Fine");
            if (exact) {
                const auto* L = named(*c, "Fine");
                for (int y = 0; exact && y < 30; ++y)
                    for (int x = 0; exact && x < 40; ++x)
                        exact = std::abs(L->pixels->get(x, y).c[0] * 65535.0 - static_cast<double>((y * 40 + x) * 37 % 65536)) < 0.6;
            }
            check(std::string("psd 16: Lr16 layers, 16-bit kept exactly (") + (comp == 1 ? "RLE" : "ZIP + prediction") + ")",
                  exact && c->image.format.depth == P::Depth::U16, err);
        }
    }

    // ── No layers: the merged picture.
    {
        WDoc d;
        d.W = 20; d.H = 10;
        const std::size_t n = 200;
        Plane r(n), g(n, 128), b(n, 7), extra(n, 255);
        for (std::size_t i = 0; i < n; ++i) r[i] = static_cast<unsigned>(i % 20 * 12);
        d.merged = {r, g, b, extra};
        for (const int comp : {1, 0}) {
            d.merged_comp = comp;
            const std::string f = at("flat" + std::to_string(comp) + ".psd");
            write(f, psd_bytes(d));
            std::string err;
            const auto c = core::file::open(f, err);
            const auto* L = c ? named(*c, "Background") : nullptr;
            check(std::string("psd: no layers -> the merged picture as Background (") + (comp ? "RLE" : "raw") + "), the extra channel said",
                  L && c->layers.size() == 1 && near(L->pixels->get(19, 9).c[0], 228 / 255.0) && near(L->pixels->get(5, 5).c[1], 128 / 255.0) &&
                      near(L->pixels->get(5, 5).c[3], 1) && all_notes(*c).find("1 extra channel left out") != std::string::npos,
                  err + all_notes(c ? *c : core::file::Contents{}));
        }
    }

    // ── Refused in words, never a crash.
    {
        const auto refuses = [&](const std::string& what, const Bytes& b, const std::string& word) {
            const std::string f = at("bad.psd");
            write(f, b);
            std::string err;
            const auto c = core::file::open(f, err);
            check("psd: " + what + " is refused in words", !c && err.find(word) != std::string::npos, err);
        };
        WDoc d;
        d.layers = {solid({0, 0, 48, 64}, 1, 2, 3, 255, 255)};
        d.layers[0].name = "x";
        WDoc psb = d; psb.version = 2;
        refuses("a PSB", psd_bytes(psb), "PSB");
        WDoc cmyk = d; cmyk.mode = 4;
        refuses("CMYK", psd_bytes(cmyk), "CMYK");
        Bytes cut = psd_bytes(d);
        cut.resize(200);   // inside the layer section (a cut in the merged picture is harmless: layers are read)
        refuses("a file cut in half", cut, "damaged");
        Bytes noise = psd_bytes(d);
        for (std::size_t i = 60; i < noise.size(); i += 7) noise[i] ^= 0x5A;
        const std::string f = at("noise.psd");
        write(f, noise);
        std::string err;
        const auto c = core::file::open(f, err);
        check("psd: scrambled bytes -- opened or refused, never a crash", c.has_value() || !err.empty(), err);
    }
}
