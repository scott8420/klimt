// klimt_selftest -- s053: ADJUSTMENT LAYERS in Photoshop's own blocks, both
// ways (levl, curv + "Crv ", hue2, brit + CgEd). The blocks round-trip each
// kind's settings; a document with all four saves as .psd and opens again
// with the four live -- settings, masks, opacity, blend, clipping, inside a
// group -- and the same look. Blocks as other programs write them (ranges in
// Hue/Saturation, the legacy brit alone, a CgEd saying useLegacy) are read and
// what didn't cross is said. (psd-tools read the same files independently --
// docs/file-format.md.)
#include "core/Composite.hpp"
#include "core/File.hpp"
#include "core/Psd.hpp"
#include "core/PsdAdjust.hpp"
#include "core/PsdStyles.hpp"
#include "pixel/Pixel.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

namespace P    = klimt::pixel;
namespace core = klimt::core;
namespace psd  = klimt::core::psd;
namespace adj  = klimt::core::psd::adj;
namespace fx   = klimt::core::psd::fx;
namespace fs   = std::filesystem;

using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {

using Bytes = std::vector<std::uint8_t>;
void be16(Bytes& b, unsigned v) { b.push_back(static_cast<std::uint8_t>(v >> 8)); b.push_back(static_cast<std::uint8_t>(v)); }
void bes16(Bytes& b, int v) { be16(b, static_cast<unsigned>(static_cast<std::uint16_t>(static_cast<std::int16_t>(v)))); }
void be32(Bytes& b, std::uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }
void put(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }

std::string at(const std::string& name) {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_psdadj_" + std::to_string(::getpid()));
        fs::create_directories(p);
        return p;
    }();
    return (d / name).string();
}

const core::Layer* named(const core::file::Contents& c, const std::string& n) {
    for (const auto& [id, L] : c.layers) if (L.name == n) return &L;
    return nullptr;
}

// Through the blocks and back.
std::optional<core::Adjust> round(const core::Adjust& a, std::vector<std::string>* losses = nullptr) {
    adj::Reading r;
    for (const auto& b : adj::blocks(a)) r.feed(b.key, b.data.data(), b.data.size());
    if (losses) *losses = r.losses();
    return r.result();
}

double v255(int v) { return v / 255.0; }

}  // namespace

void selftest_psd_adjust(const Check& check) {
    // ── s061: Invert, Posterize, Threshold, Exposure ───────────────────────
    {
        using K = core::AdjustKind;
        const auto inv = adj::blocks(core::adjust::fresh(K::Invert));
        check("psd adjust: Invert is an empty nvrt block", inv.size() == 1 && inv[0].key == "nvrt" && inv[0].data.empty(), "");
        check("psd adjust: Invert comes back", round(core::adjust::fresh(K::Invert)) == core::adjust::fresh(K::Invert), "");
        auto po = core::adjust::fresh(K::Posterize); po.posterize = 9;
        const auto pb = adj::blocks(po);
        check("psd adjust: Posterize is post -- 9, then 2 bytes padding", pb.size() == 1 && pb[0].key == "post" && pb[0].data == Bytes{0, 9, 0, 0}, "");
        check("psd adjust: Posterize comes back", round(po) == po, "");
        auto th = core::adjust::fresh(K::Threshold); th.threshold = 200;
        const auto tb = adj::blocks(th);
        check("psd adjust: Threshold is thrs -- 200, then padding", tb.size() == 1 && tb[0].key == "thrs" && tb[0].data == Bytes{0, 200, 0, 0}, "");
        check("psd adjust: Threshold comes back", round(th) == th, "");
        auto ex = core::adjust::fresh(K::Exposure); ex.exposure = 1.5; ex.offset = -0.0312; ex.exp_gamma = 0.85;
        const auto eb = adj::blocks(ex);
        check("psd adjust: Exposure is expA -- version 1, three floats (14 bytes)",
              eb.size() == 1 && eb[0].key == "expA" && eb[0].data.size() == 14 && eb[0].data[0] == 0 && eb[0].data[1] == 1, "");
        const auto er = round(ex);
        check("psd adjust: Exposure comes back, to four decimals", er && *er == ex, er ? std::to_string(er->offset) : "none");
        Bytes junk; be16(junk, 1); be32(junk, 0x7fc00000u); be32(junk, 0); be32(junk, 0x3f800000u);   // a NaN exposure
        adj::Reading r;
        r.feed("expA", junk.data(), junk.size());
        check("psd adjust: an Exposure with a NaN is not read", !r.result(), "");
    }
    // ── the blocks, kind by kind ───────────────────────────────────────────
    {
        core::Adjust lv = core::adjust::fresh(core::AdjustKind::Levels);
        lv.levels[0] = {v255(10), v255(240), 1.35, v255(5), v255(250)};
        lv.levels[2] = {v255(30), v255(200), 0.6, v255(0), v255(255)};
        const auto b = adj::blocks(lv);
        check("psd adjust: Levels is one levl block, version 2, 29 records (292 bytes)",
              b.size() == 1 && b[0].key == "levl" && b[0].data.size() == 292 && b[0].data[0] == 0 && b[0].data[1] == 2, "");
        const auto r = round(lv);
        check("psd adjust: Levels comes back -- RGB and Green, every value", r && *r == core::adjust::normalised(lv), "");
    }
    {
        core::Adjust cv = core::adjust::fresh(core::AdjustKind::Curves);
        cv.curves[0] = {{0, 0}, {v255(64), v255(90)}, {v255(190), v255(170)}, {1, 1}};
        cv.curves[3] = {{0, v255(20)}, {1, v255(230)}};
        const auto b = adj::blocks(cv);
        bool crv = false;
        if (b.size() == 1) {
            const std::string s(b[0].data.begin(), b[0].data.end());
            crv = s.find("Crv ") != std::string::npos;
        }
        check("psd adjust: Curves is one curv block (v1, then Crv v4 with channel ids)",
              b.size() == 1 && b[0].key == "curv" && b[0].data[0] == 0 && b[0].data[2] == 1 && crv, "");
        // The bitmap: RGB (bit 0) and Blue (bit 3); Red and Green straight, not written.
        check("psd adjust: Curves writes RGB and the bent channels only",
              b.size() == 1 && b[0].data[3] == 0 && b[0].data[4] == 0 && b[0].data[5] == 0 && b[0].data[6] == 0x09, "");
        const auto r = round(cv);
        check("psd adjust: Curves comes back -- RGB's four points and Blue's two",
              r && *r == core::adjust::normalised(cv), "");
        // A Crv tail with Red only, after a v1 part saying RGB: the tail wins.
        Bytes d;
        d.push_back(0); be16(d, 1); be32(d, 1);
        be16(d, 2); be16(d, 0); be16(d, 0); be16(d, 255); be16(d, 255);
        put(d, "Crv "); be16(d, 4); be32(d, 1);
        be16(d, 1); be16(d, 3); be16(d, 0); be16(d, 0); be16(d, 200); be16(d, 128); be16(d, 255); be16(d, 255);
        adj::Reading rd;
        rd.feed("curv", d.data(), d.size());
        const auto t = rd.result();
        check("psd adjust: Curves -- the Crv tail (channel-named) wins",
              t && t->curves[1].size() == 3 && std::abs(t->curves[1][1].x - v255(128)) < 1e-9 && std::abs(t->curves[1][1].y - v255(200)) < 1e-9, "");
    }
    {
        core::Adjust hs = core::adjust::fresh(core::AdjustKind::HueSat);
        hs.hue = -40; hs.saturation = 35; hs.lightness = -10;
        const auto b = adj::blocks(hs);
        check("psd adjust: Hue/Saturation is one hue2 block, version 2, 100 bytes",
              b.size() == 1 && b[0].key == "hue2" && b[0].data.size() == 100 && b[0].data[2] == 0, "");
        const auto r = round(hs);
        check("psd adjust: Hue/Saturation comes back -- the master hue, saturation, lightness", r && *r == hs, "");
        core::Adjust cz = hs;
        cz.colorize = true; cz.hue = 210; cz.saturation = 60; cz.lightness = 15;
        const auto rz = round(cz);
        check("psd adjust: Colorize comes back -- its own hue (0..360), saturation, lightness", rz && *rz == cz, "");
        // Photoshop's file with a colour range changed (Reds: hue +30).
        Bytes d = adj::blocks(hs)[0].data;
        d[2 + 2 + 12 + 8] = 0; d[2 + 2 + 12 + 9] = 30;
        adj::Reading rd;
        rd.feed("hue2", d.data(), d.size());
        const auto l = rd.losses();
        check("psd adjust: a colour range set in Photoshop -- the master kept, the range said",
              rd.result() && *rd.result() == hs && l.size() == 1 && l[0] == "colour ranges", "");
    }
    {
        core::Adjust bc = core::adjust::fresh(core::AdjustKind::BrightCon);
        bc.brightness = 120; bc.contrast = -30;
        const auto b = adj::blocks(bc);
        check("psd adjust: Brightness/Contrast is brit (old readers) and CgEd (Photoshop CS3+)",
              b.size() == 2 && b[0].key == "brit" && b[1].key == "CgEd" && b[0].data.size() == 8, "");
        std::vector<std::string> l;
        const auto r = round(bc, &l);
        check("psd adjust: Brightness/Contrast comes back from CgEd -- past brit's +-100, modern, nothing lost",
              r && *r == bc && l.empty(), "");
        // brit alone (Photoshop before CS3): read, said legacy.
        adj::Reading rb;
        rb.feed("brit", b[0].data.data(), b[0].data.size());
        const auto lb = rb.losses();
        check("psd adjust: brit alone -- read (brightness at its +-100), said legacy",
              rb.result() && rb.result()->brightness == 100 && rb.result()->contrast == -30 && lb.size() == 1 && lb[0] == "legacy", "");
        // CgEd saying useLegacy.
        Bytes cg = b[1].data;
        const std::string s(cg.begin(), cg.end());
        const auto k = s.find("useLegacybool");
        if (k != std::string::npos) cg[k + 13] = 1;
        adj::Reading rl;
        rl.feed("CgEd", cg.data(), cg.size());
        const auto ll = rl.losses();
        check("psd adjust: CgEd with useLegacy -- read, said legacy",
              k != std::string::npos && rl.result() && *rl.result() == bc && ll.size() == 1 && ll[0] == "legacy", "");
        // A CgEd that isn't Brightness / Contrast holds nothing here.
        Bytes other;
        be32(other, 16); be32(other, 1); be16(other, 0); be32(other, 0); put(other, "null"); be32(other, 1);
        be32(other, 0); put(other, "Vrsn"); put(other, "long"); be32(other, 1);
        adj::Reading ro;
        ro.feed("CgEd", other.data(), other.size());
        check("psd adjust: a CgEd without Brgh / Cntr makes no adjustment", !ro.result(), "");
    }
    {   // Damaged blocks: nothing, never a crash.
        bool fine = true;
        const char* keys[] = {"levl", "curv", "hue2", "brit", "CgEd"};
        for (const char* k : keys) {
            for (std::size_t n = 0; n < 40; n += 3) {
                Bytes junk(n, 0xC7);
                if (n >= 2) { junk[0] = 0; junk[1] = 2; }
                adj::Reading r;
                r.feed(k, junk.data(), junk.size());
                const auto a = r.result();
                if (a && !core::adjust::refusal(*a).empty()) fine = false;
            }
        }
        check("psd adjust: short and scrambled blocks -- nothing, or something in range; never a crash", fine, "");
    }

    // ── a document: all four, saved and opened again ─────────────────────
    {
        using L = core::Layer;
        const int W = 160, H = 120;
        const P::Format f8{P::Depth::U8, P::Mode::RGB, true};
        const P::Format g8{P::Depth::U8, P::Mode::Gray, false};
        core::Image im;
        im.width = W; im.height = H; im.format = f8; im.dpi = 72;
        // A picture with something to adjust: a ramp, and colour.
        P::TileMap ramp(f8, W, H);
        for (int x = 0; x < W; x += 8)
            ramp = ramp.fill({x, 0, 8, H}, P::rgba(x / float(W), 0.3f + 0.4f * x / float(W), 1.f - x / float(W)));
        ramp = ramp.fill({60, 40, 40, 40}, P::rgba(0.9f, 0.2f, 0.1f));
        L paper; paper.name = "Ramp"; paper.pos = 1; paper.pixels = std::make_shared<const P::TileMap>(ramp);
        L spot; spot.name = "Spot"; spot.pos = 2;
        spot.pixels = std::make_shared<const P::TileMap>(P::TileMap(f8, W, H).fill({20, 70, 50, 30}, P::rgba(0.2f, 0.8f, 0.3f)));

        L lv; lv.kind = core::Kind::Adjustment; lv.name = "Levels 1"; lv.pos = 3; lv.opacity = 70;
        lv.adjust = core::adjust::fresh(core::AdjustKind::Levels);
        lv.adjust.levels[0] = {v255(20), v255(220), 1.4, v255(0), v255(255)};
        lv.adjust.levels[1] = {v255(0), v255(255), 0.8, v255(10), v255(245)};
        {   // its mask hides a square (Klimt's mask: what's HIDDEN)
            P::Px one{}; one.c[0] = 1.f;
            lv.mask = std::make_shared<const P::TileMap>(P::TileMap(g8, W, H).fill({100, 10, 40, 30}, one));
        }
        L cv; cv.kind = core::Kind::Adjustment; cv.name = "Curves on Spot"; cv.pos = 2.5; cv.clipped = true;
        cv.adjust = core::adjust::fresh(core::AdjustKind::Curves);
        cv.adjust.curves[0] = {{0, 0}, {v255(80), v255(130)}, {1, 1}};
        cv.mask = std::make_shared<const P::TileMap>(P::TileMap(g8, W, H));
        L grp; grp.kind = core::Kind::Group; grp.name = "Looks"; grp.pos = 4; grp.blend = core::Blend::PassThrough;
        L hs; hs.kind = core::Kind::Adjustment; hs.name = "Hue/Saturation 1"; hs.parent = "G"; hs.pos = 1;
        hs.adjust = core::adjust::fresh(core::AdjustKind::HueSat);
        hs.adjust.hue = 45; hs.adjust.saturation = -25; hs.adjust.lightness = 5;
        hs.blend = core::Blend::Luminosity; hs.fill = 80;
        hs.mask = std::make_shared<const P::TileMap>(P::TileMap(g8, W, H));
        L bc; bc.kind = core::Kind::Adjustment; bc.name = "Brightness/Contrast 1"; bc.parent = "G"; bc.pos = 2;
        bc.adjust = core::adjust::fresh(core::AdjustKind::BrightCon);
        bc.adjust.brightness = 40; bc.adjust.contrast = 25;
        bc.mask = std::make_shared<const P::TileMap>(P::TileMap(g8, W, H));
        L off; off.kind = core::Kind::Adjustment; off.name = "Colorize off"; off.pos = 5; off.visible = false;
        off.adjust = core::adjust::fresh(core::AdjustKind::HueSat);
        off.adjust.colorize = true; off.adjust.hue = 30; off.adjust.saturation = 50;
        off.mask = std::make_shared<const P::TileMap>(P::TileMap(g8, W, H));
        off.mask_on = false;
        const std::map<std::string, L> ls{{"L1", paper}, {"L2", spot}, {"L3", lv}, {"L4", cv}, {"G", grp},
                                          {"L5", hs}, {"L6", bc}, {"L7", off}};

        const auto pl = psd::plan(im, ls);
        check("psd adjust write: the plan -- five adjustment layers kept live, nothing left out",
              pl.kept.size() == 1 && pl.kept[0].find("5 adjustment layers live") == 0 && pl.left_out.empty() && pl.baked.empty(),
              pl.kept.empty() ? std::string("(none)") : pl.kept[0]);
        const std::string f = at("adjust8.psd");
        std::string err;
        std::vector<std::string> notes;
        const bool ok = psd::save(f, im, ls, err, notes);
        check("psd adjust write: saves, nothing to say after", ok && notes.empty(), err);
        std::string all;
        {
            std::ifstream in(f, std::ios::binary);
            all.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        check("psd adjust write: the file carries levl, curv, hue2, brit and CgEd",
              all.find("8BIMlevl") != std::string::npos && all.find("8BIMcurv") != std::string::npos &&
                  all.find("8BIMhue2") != std::string::npos && all.find("8BIMbrit") != std::string::npos &&
                  all.find("8BIMCgEd") != std::string::npos, "");
        const auto c = ok ? core::file::open(f, err) : std::nullopt;
        check("psd adjust write: Klimt opens it again, nothing said left out", c && c->notes.empty(),
              c && !c->notes.empty() ? c->notes[0] : err);
        if (c) {
            const auto* Lv = named(*c, "Levels 1");
            const auto* Cv = named(*c, "Curves on Spot");
            const auto* Hs = named(*c, "Hue/Saturation 1");
            const auto* Bc = named(*c, "Brightness/Contrast 1");
            const auto* Of = named(*c, "Colorize off");
            const auto* G = named(*c, "Looks");
            const bool all5 = Lv && Cv && Hs && Bc && Of && G;
            check("psd adjust write: the five come back as adjustment layers", all5 && Lv->kind == core::Kind::Adjustment &&
                  Cv->kind == core::Kind::Adjustment && Hs->kind == core::Kind::Adjustment && Bc->kind == core::Kind::Adjustment &&
                  Of->kind == core::Kind::Adjustment && c->layers.size() == ls.size(), std::to_string(c->layers.size()));
            if (all5) {
                check("psd adjust write: Levels -- its settings, 70 %, its mask hiding the square",
                      Lv->adjust == core::adjust::normalised(lv.adjust) && Lv->opacity == 70 && Lv->mask && Lv->mask_on &&
                          Lv->mask->get(110, 20).c[0] > 0.99f && Lv->mask->get(10, 100).c[0] < 0.01f, "");
                check("psd adjust write: Curves -- its points, clipped to Spot", Cv->adjust == cv.adjust && Cv->clipped, "");
                std::string gid;
                for (const auto& [id, L2] : c->layers) if (&L2 == G) gid = id;
                check("psd adjust write: Hue/Saturation and Brightness/Contrast inside the Pass Through group, in order",
                      G->blend == core::Blend::PassThrough && Hs->parent == gid && Bc->parent == gid && Hs->pos < Bc->pos, "");
                check("psd adjust write: Hue/Saturation -- its settings, Luminosity, Fill 80",
                      Hs->adjust == hs.adjust && Hs->blend == core::Blend::Luminosity && Hs->fill == 80, "");
                check("psd adjust write: Brightness/Contrast -- its settings", Bc->adjust == bc.adjust, "");
                check("psd adjust write: hidden Colorize, its mask switched off",
                      Of->adjust == off.adjust && !Of->visible && Of->mask && !Of->mask_on, "");
            }
            // The look: Klimt's composite of the opened file against the one saved.
            const auto a = core::file::flatten(im, ls, {});
            const auto b = core::file::flatten(c->image, c->layers, {});
            double worst = 1;
            if (a && b) {
                worst = 0;
                for (int y = 0; y < H; y += 3)
                    for (int x = 0; x < W; x += 3)
                        for (int k = 0; k < 4; ++k) worst = std::max(worst, std::abs(double(a->get(x, y).c[k]) - b->get(x, y).c[k]));
            }
            check("psd adjust write: the same look opened again -- every pixel within 2 levels", worst <= 2.0 / 255 + 1e-6,
                  std::to_string(worst * 255));
        }
        // 16-bit: the same blocks inside Lr16.
        {
            core::Image im16 = im;
            im16.format = P::Format{P::Depth::U16, P::Mode::RGB, true};
            const P::Format g16{P::Depth::U16, P::Mode::Gray, false};
            std::map<std::string, L> two;
            L px = paper; px.pixels = std::make_shared<const P::TileMap>(P::TileMap(im16.format, W, H).fill({0, 0, W, H}, P::rgba(0.5f, 0.5f, 0.5f)));
            L cu = cv; cu.clipped = false; cu.pos = 2; cu.mask = std::make_shared<const P::TileMap>(P::TileMap(g16, W, H));
            two = {{"L1", px}, {"L2", cu}};
            const std::string f6 = at("adjust16.psd");
            std::vector<std::string> n2;
            const bool ok16 = psd::save(f6, im16, two, err, n2);
            const auto c16 = ok16 ? core::file::open(f6, err) : std::nullopt;
            const auto* C = c16 ? named(*c16, "Curves on Spot") : nullptr;
            check("psd adjust write 16: Curves through Lr16 comes back live",
                  C && C->kind == core::Kind::Adjustment && C->adjust == cv.adjust && C->mask && C->mask->format() == g16, err);
        }
    }
}

// ── s053 m2: layer styles (lfx2) ─────────────────────────────────────────────
namespace {
core::Styles every_style() {
    using B = P::Blend;
    core::Styles s;
    s.drop = {true, {0.25f, 0.f, 0.5f}, 60, 135, 12, 10, 9, B::Multiply};
    s.inner = {true, {0.f, 0.f, 0.25f}, 40, -30, 4, 20, 6, B::Overlay};
    s.outer = {true, {1.f, 0.75f, 0.f}, 80, 15, 14, B::Screen};
    s.inner_glow = {true, {0.f, 1.f, 0.5f}, 55, 25, 7, true, B::LinearDodge};
    s.bevel = {true, 1, 250, false, 11, 3, 60, 40, {1.f, 1.f, 0.75f}, 65, {0.25f, 0.f, 0.f}, 85, B::ColorDodge, B::LinearBurn};
    s.satin = {true, {0.5f, 0.f, 0.25f}, 45, -100, 20, 30, false, B::SoftLight};
    s.overlay = {true, {0.f, 0.5f, 1.f}, 35, B::Color};
    s.gradient = {true, {1.f, 0.f, 0.f}, {0.f, 0.f, 1.f}, 70, -45, 1, 120, true, B::HardLight};
    s.stroke = {true, {0.f, 0.25f, 0.f}, 90, 6, 2, B::Normal};
    return s;
}
std::optional<core::Styles> round_fx(const Bytes& b, std::vector<std::string>* lost = nullptr, const char* key = "lfx2",
                                     int ga = 120, int galt = 30) {
    fx::Reading r;
    r.feed(key, b.data(), b.size());
    if (lost) *lost = r.losses();
    return r.result(ga, galt);
}
std::size_t find(const Bytes& b, const std::string& s, std::size_t from = 0) {
    const std::string all(b.begin(), b.end());
    return all.find(s, from);
}
}  // namespace

void selftest_psd_styles(const Check& check) {
    const core::Styles all = every_style();
    const Bytes b = fx::block(all);
    {
        check("psd styles: lfx2 opens with version 0, descriptor 16",
              b.size() > 8 && b[3] == 0 && b[7] == 16 && find(b, "masterFXSwitch") != std::string::npos, "");
        bool every = true;
        for (const char* k : {"DrShObjc", "IrShObjc", "OrGlObjc", "IrGlObjc", "ebblObjc", "ChFXObjc", "SoFiObjc", "GrFlObjc", "FrFXObjc"})
            every = every && find(b, k) != std::string::npos;
        check("psd styles: all nine effects written (DrSh IrSh OrGl IrGl ebbl ChFX SoFi GrFl FrFX)", every, "");
        std::vector<std::string> lost;
        const auto r = round_fx(b, &lost);
        check("psd styles: every effect comes back -- colours, opacities, angles, sizes, modes", r && *r == all && lost.empty(),
              lost.empty() ? "" : lost[0]);
        if (r && !(*r == all)) {
            check("psd styles: (which) drop", r->drop == all.drop, "");
            check("psd styles: (which) inner", r->inner == all.inner, "");
            check("psd styles: (which) outer", r->outer == all.outer, "");
            check("psd styles: (which) inner glow", r->inner_glow == all.inner_glow, "");
            check("psd styles: (which) bevel", r->bevel == all.bevel, "");
            check("psd styles: (which) satin", r->satin == all.satin, "");
            check("psd styles: (which) overlay", r->overlay == all.overlay, "");
            check("psd styles: (which) gradient", r->gradient == all.gradient, "");
            check("psd styles: (which) stroke", r->stroke == all.stroke, "");
        }
        core::Styles some;
        some.drop.on = true;
        some.stroke = all.stroke;
        some.stroke.on = false;   // set, switched off: written, comes back off
        const auto rs = round_fx(fx::block(some));
        check("psd styles: only what's set is written; a switched-off effect keeps its settings",
              rs && *rs == some && find(fx::block(some), "OrGlObjc") == std::string::npos, "");
    }
    {   // The master switch off in Photoshop: each kept, off, said.
        Bytes m = b;
        const auto k = find(m, "masterFXSwitchbool");
        if (k != std::string::npos) m[k + 18] = 0;
        std::vector<std::string> lost;
        const auto r = round_fx(m, &lost);
        check("psd styles: the master switch off -- the effects kept, switched off, said",
              k != std::string::npos && r && !r->any() && r->drop.size == all.drop.size && lost.size() == 1 && lost[0] == "off", "");
    }
    {   // "Use Global Light": the document's angle and altitude.
        Bytes g = b;
        const auto d = find(g, "uglgbool");                       // the drop shadow's
        const auto e = find(g, "uglgbool", find(g, "ebblObjc"));  // the bevel's
        if (d != std::string::npos) g[d + 8] = 1;
        if (e != std::string::npos) g[e + 8] = 1;
        const auto r = round_fx(g, nullptr, "lfx2", 77, 51);
        check("psd styles: Use Global Light -- the drop shadow and bevel take the document's angle, the bevel its altitude",
              r && r->drop.angle == 77 && r->bevel.angle == 77 && r->bevel.altitude == 51 && r->inner.angle == all.inner.angle, "");
    }
    {   // lmfx: a second inner shadow turned into a second drop shadow -- the first kept.
        Bytes m = b;
        const auto k = find(m, "IrShObjc");
        if (k != std::string::npos) for (int i = 0; i < 4; ++i) m[k + 8 + 6 + 4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>("DrSh"[i]);
        std::vector<std::string> lost;
        const auto r = round_fx(m, &lost, "lmfx");
        check("psd styles: an effect used twice (lmfx) -- the first kept, said",
              k != std::string::npos && r && r->drop == all.drop && !r->inner.on && lost.size() == 1 && lost[0] == "copies",
              lost.empty() ? "" : lost[0]);
    }
    {   // Pattern overlay can't go live.
        core::Styles p = all;
        p.pattern.on = true;
        check("psd styles: a pattern overlay can't cross live; without one everything can", !fx::crosses(p) && fx::crosses(all), "");
    }
    {   // Damaged: nothing, or something; never a crash.
        bool fine = true;
        for (std::size_t cut = 0; cut < b.size(); cut += 37) {
            Bytes t(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(cut));
            (void)round_fx(t);
            Bytes x = b;
            for (std::size_t i = cut; i < x.size(); i += 11) x[i] = static_cast<std::uint8_t>(x[i] * 7 + 3);
            const auto r = round_fx(x);
            if (r && (r->drop.size < 0 || r->drop.size > 250 || r->stroke.size < 1 || r->bevel.depth < 1)) fine = false;
        }
        check("psd styles: cut short and scrambled lfx2 -- nothing, or styles in range; never a crash", fine, "");
    }

    // ── a document: styles on a layer and a group, saved and opened again ─
    {
        using L = core::Layer;
        const int W = 140, H = 100;
        const P::Format f8{P::Depth::U8, P::Mode::RGB, true};
        core::Image im;
        im.width = W; im.height = H; im.format = f8; im.dpi = 72;
        L paper; paper.name = "Paper"; paper.pos = 1;
        paper.pixels = std::make_shared<const P::TileMap>(P::TileMap(f8, W, H).fill({0, 0, W, H}, P::rgba(0.9f, 0.9f, 0.85f)));
        L box; box.name = "Box"; box.pos = 2; box.fill = 60;
        box.pixels = std::make_shared<const P::TileMap>(P::TileMap(f8, W, H).fill({30, 25, 40, 30}, P::rgba(0.2f, 0.4f, 0.8f)));
        box.styles = all;
        box.styles.outer.on = false;   // an off one rides along
        L grp; grp.kind = core::Kind::Group; grp.name = "Framed"; grp.pos = 3; grp.blend = core::Blend::Normal;
        grp.styles.stroke = {true, {0.5f, 0.f, 0.f}, 100, 3, 0, P::Blend::Normal};
        L dot; dot.name = "Dot"; dot.parent = "G"; dot.pos = 1;
        dot.pixels = std::make_shared<const P::TileMap>(P::TileMap(f8, W, H).fill({90, 50, 20, 20}, P::rgba(0.f, 0.6f, 0.f)));
        const std::map<std::string, L> ls{{"L1", paper}, {"L2", box}, {"G", grp}, {"L3", dot}};
        const auto pl = psd::plan(im, ls);
        check("psd styles write: the plan -- two layers' styles live, nothing baked or left out",
              pl.kept.size() == 1 && pl.kept[0].find("2 layers' styles live") == 0 && pl.baked.empty() && pl.left_out.empty(),
              pl.kept.empty() ? "" : pl.kept[0]);
        const std::string f = at("styles8.psd");
        std::string err;
        std::vector<std::string> notes;
        const bool ok = psd::save(f, im, ls, err, notes);
        const auto c = ok ? core::file::open(f, err) : std::nullopt;
        check("psd styles write: saves, opens again, nothing said", c && notes.empty() && c->notes.empty(),
              c && !c->notes.empty() ? c->notes[0] : err);
        if (c) {
            const auto* Bx = named(*c, "Box");
            const auto* G = named(*c, "Framed");
            check("psd styles write: the layer's nine effects back live (one off), Fill 60, its pixels plain",
                  Bx && Bx->styles == box.styles && Bx->fill == 60 && Bx->pixels->get(10, 10).c[3] == 0, "");
            check("psd styles write: the group's stroke back live", G && G->kind == core::Kind::Group && G->styles == grp.styles, "");
            const auto a = core::file::flatten(im, ls, {});
            const auto r = core::file::flatten(c->image, c->layers, {});
            double worst = 1;
            if (a && r) {
                worst = 0;
                for (int y = 0; y < H; y += 2)
                    for (int x = 0; x < W; x += 2)
                        for (int k = 0; k < 4; ++k) worst = std::max(worst, std::abs(double(a->get(x, y).c[k]) - r->get(x, y).c[k]));
            }
            check("psd styles write: the same look opened again -- every pixel within 2 levels", worst <= 2.0 / 255 + 1e-6,
                  std::to_string(worst * 255));
        }
        // A pattern overlay: baked as before, said why.
        {
            std::map<std::string, L> pat = ls;
            pat["L2"].styles.pattern.on = true;
            const auto pp = psd::plan(im, pat);
            check("psd styles write: a pattern overlay -- that layer's styles baked, said why",
                  pp.baked.size() == 1 && pp.baked[0].find("pattern overlay") != std::string::npos && pp.kept.size() == 1 &&
                      pp.kept[0].find("1 layer's styles live") == 0,
                  pp.baked.empty() ? "" : pp.baked[0]);
        }
    }
}
