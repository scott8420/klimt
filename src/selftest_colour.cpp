// s067 (WEAKNESSES 14): colour management -- libs/tint, and a Gray / CMYK /
// document shown and painted through its profile. docs/colour.md.
#include "core/Composite.hpp"
#include "core/File.hpp"
#include "core/Document.hpp"
#include "core/Profile.hpp"
#include "core/Live.hpp"
#include "core/Styles.hpp"
#include "core/Text.hpp"
#include "tint/Tint.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <array>
#include <cstdint>
#include <vector>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace T = klimt::tint;

namespace {
using Check = std::function<void(const std::string&, bool, const std::string&)>;
bool near(double a, double b, double eps = 0.01) { return std::abs(a - b) <= eps; }
std::string num(double v) { return std::to_string(v); }

// A 64 x 64 document of mode `m`, its background `px` (straight, the mode's channels).
core::file::Contents doc(P::Mode m, const P::Px& px, std::shared_ptr<const T::Icc> icc) {
    core::file::NewSpec s;
    s.width = s.height = 64;
    s.format = P::Format{P::Depth::U8, m, true};
    s.background = core::file::Background::Colour;
    s.colour = px;
    auto c = core::file::make_new(s);
    c.image.icc = std::move(icc);
    return c;
}
P::Px ink(float c, float m, float y, float k) { P::Px p; p.c = {c, m, y, k, 1}; return p; }
}  // namespace

void selftest_colour(const Check& check) {
    std::cout << "\n-- colour management: libs/tint (s067) --\n";
    const auto cmyk = std::make_shared<const T::Icc>(T::plain_cmyk());
    const auto adobe = std::make_shared<const T::Icc>(T::adobe_rgb());
    const auto g18 = std::make_shared<const T::Icc>(T::gray(1.8));
    {
        const auto ic = T::inspect(*cmyk), ia = T::inspect(*adobe), ig = T::inspect(*g18), is = T::inspect(T::srgb());
        check("tint: profiles made here read back -- CMYK, RGB, Gray, sRGB, with their names",
              ic.ok && ic.space == T::Space::CMYK && ia.ok && ia.space == T::Space::RGB && ia.name.find("Adobe RGB") == 0 &&
              ig.ok && ig.space == T::Space::Gray && is.ok && is.space == T::Space::RGB, ic.name + " / " + ia.name);
        const T::Icc junk{1, 2, 3, 4, 5};
        check("tint: junk isn't a profile; a profile of another space gives no link",
              !T::inspect(junk).ok && !T::to_srgb(junk, T::Space::CMYK) && !T::to_srgb(*adobe, T::Space::CMYK) &&
              !T::to_srgb(T::Icc{}, T::Space::Gray), "");
        check("tint: sRGB is sRGB, Adobe RGB isn't; no profile is taken as sRGB",
              T::is_srgb(T::srgb()) && !T::is_srgb(*adobe) && T::is_srgb(T::Icc{}), "");
        const auto l = T::to_srgb(cmyk, T::Space::CMYK);
        float in[8] = {0, 0, 0, 0, 0, 1, 1, 0}, out[6] = {};
        if (l) l->run(in, out, 2);
        check("tint: plain CMYK -- paper is white, magenta + yellow red", l && near(out[0], 1) && near(out[1], 1) && near(out[2], 1) &&
              near(out[3], 1, 0.02) && near(out[4], 0, 0.02) && near(out[5], 0, 0.02), num(out[3]) + "," + num(out[4]));
        check("tint: links found by the profile shared -- the same link twice", l && l == T::to_srgb(cmyk, T::Space::CMYK), "");
        float g[3] = {0, 1, 0}, o[3] = {};
        T::to_srgb(adobe, T::Space::RGB)->run(g, o, 1);
        float m[3] = {0.5f, 0.5f, 0.5f}, mo[3] = {};
        T::to_srgb(adobe, T::Space::RGB)->run(m, mo, 1);
        check("tint: Adobe RGB's green past sRGB's (clipped green); its mid grey stays grey",
              near(o[0], 0, 0.02) && near(o[1], 1, 0.01) && near(mo[0], mo[1], 0.004) && near(mo[1], mo[2], 0.004), num(o[0]) + " " + num(mo[0]));
    }

    std::cout << "\n-- colour management: documents through their profile (s067) --\n";
    {
        const auto red = doc(P::Mode::CMYK, ink(0, 1, 1, 0), cmyk);
        const auto px = core::sample_composite(red.image, red.layers, 10, 10);
        check("CMYK + profile: shown through it -- M + Y full is red; managed", core::managed(red.image) &&
              near(px.r, 1, 0.02) && near(px.g, 0, 0.02) && near(px.b, 0, 0.02), num(px.r) + "," + num(px.g));
        const auto rich = doc(P::Mode::CMYK, ink(0.2f, 0.6f, 0.1f, 0.3f), cmyk), bare = doc(P::Mode::CMYK, ink(0.2f, 0.6f, 0.1f, 0.3f), nullptr);
        const auto a = core::sample_composite(rich.image, rich.layers, 5, 5), b = core::sample_composite(bare.image, bare.layers, 5, 5);
        check("CMYK without a profile: the plain conversion as before, unmanaged; with the plain profile near it",
              !core::managed(bare.image) && near(b.r, 0.8 * 0.7, 0.003) && near(a.r, b.r, 0.04) && near(a.g, b.g, 0.04), num(a.r) + " vs " + num(b.r));
        // A colour picked in sRGB, made into the document's inks, shows as itself.
        const auto p = core::from_srgb(red.image, 0.2f, 0.5f, 0.8f);
        const auto back = doc(P::Mode::CMYK, p, cmyk);
        const auto s = core::sample_composite(back.image, back.layers, 3, 3);
        check("CMYK + profile: a picked colour painted shows as itself (within the table's reach)",
              near(s.r, 0.2, 0.03) && near(s.g, 0.5, 0.03) && near(s.b, 0.8, 0.03), num(s.r) + "," + num(s.g) + "," + num(s.b));
        // Gray: a 1.8 gamma grey is lighter at 50 % than sRGB's curve.
        P::Px half_a; half_a.c = {0.5f, 1, 0, 0, 0};
        const auto gm = doc(P::Mode::Gray, half_a, g18), gb = doc(P::Mode::Gray, half_a, nullptr);
        const auto gs = core::sample_composite(gm.image, gm.layers, 1, 1), gn = core::sample_composite(gb.image, gb.layers, 1, 1);
        check("Gray + Gamma 1.8: 50 % shows lighter than as sRGB grey; no profile: 50 % is 50 %", core::managed(gm.image) &&
              gs.r > 0.53 && near(gs.r, gs.g, 0.004) && near(gn.r, 0.5, 0.003), num(gs.r) + " vs " + num(gn.r));
        // Wrong or broken profiles: unmanaged, said in the status bar.
        const auto wrong = doc(P::Mode::CMYK, ink(0, 1, 1, 0), adobe);
        const auto broke = doc(P::Mode::CMYK, ink(0, 1, 1, 0), std::make_shared<const T::Icc>(T::Icc{9, 9, 9}));
        check("a profile of another space, or junk: unmanaged (the plain conversion); the status bar says which",
              !core::managed(wrong.image) && !core::managed(broke.image) && core::profile_name(broke.image) == "(unreadable profile)" &&
              core::profile_name(red.image).find("Plain CMYK") == 0 && core::profile_name(bare.image).empty(), core::profile_name(red.image));
        check("an RGB document's profile isn't used on the pixels (the compositor works in its space)",
              !core::managed(doc(P::Mode::RGB, P::rgba(0, 1, 0), adobe).image), "");
    }

    std::cout << "\n-- colour management: RGB profiles on the screen (s067 m2) --\n";
    {
        const auto ad = doc(P::Mode::RGB, P::rgba(0.5f, 0.3f, 0.2f), adobe);
        const auto sr = doc(P::Mode::RGB, P::rgba(0.5f, 0.3f, 0.2f), std::make_shared<const T::Icc>(T::srgb()));
        check("display: Adobe RGB is turned for the screen; sRGB, or none, isn't",
              core::display_managed(ad.image) && !core::display_managed(sr.image) &&
              !core::display_managed(doc(P::Mode::RGB, P::rgba(0, 0, 0), nullptr).image) && !T::display(nullptr), "");
        std::vector<std::uint32_t> px{0xff808080u, 0xff804020u, 0x00000000u, 0x80402010u};
        auto same = px;
        core::shown_argb(sr.image, same);
        core::shown_argb(ad.image, px);
        const auto ch = [](std::uint32_t v, int s) { return static_cast<int>(v >> s & 0xff); };
        check("display: an sRGB document's tiles untouched", same == std::vector<std::uint32_t>{0xff808080u, 0xff804020u, 0x00000000u, 0x80402010u}, "");
        check("display: Adobe RGB -- grey stays grey; a warm brown comes out redder for sRGB (its wider gamut)",
              std::abs(ch(px[0], 16) - ch(px[0], 8)) <= 1 && std::abs(ch(px[0], 8) - ch(px[0], 0)) <= 1 &&
              ch(px[1], 16) > 0x80 && ch(px[1], 0) < 0x20 + 2, std::to_string(ch(px[1], 16)) + "," + std::to_string(ch(px[1], 0)));
        check("display: transparent stays transparent; half-covered stays premultiplied (colour within its alpha)",
              px[2] == 0 && (px[3] >> 24) == 0x80 && ch(px[3], 16) <= 0x80 && ch(px[3], 16) > ch(px[1], 16) / 2 - 3, std::to_string(ch(px[3], 16)));
        // Composited in its own space: the numbers are the document's.
        const auto raw = core::sample_composite(ad.image, ad.layers, 4, 4);
        check("Adobe RGB: the compositor keeps the document's numbers (histograms, Levels see them)", near(raw.r, 0.5, 0.003) && near(raw.g, 0.3, 0.003), num(raw.r));
        const auto pick = core::shown(ad.image, raw);
        check("Adobe RGB: the Eyedropper hands the colour pair what the screen shows (redder than the numbers)",
              pick.r > raw.r + 0.02 && pick.a == raw.a, num(pick.r));
        const auto in = core::from_srgb(ad.image, 0.2f, 0.6f, 0.4f);
        const auto back = doc(P::Mode::RGB, in, adobe);
        const auto seen = core::shown(back.image, core::sample_composite(back.image, back.layers, 2, 2));
        check("Adobe RGB: a picked colour painted shows as itself; its numbers are Adobe RGB's (not the picked ones)",
              near(seen.r, 0.2, 0.01) && near(seen.g, 0.6, 0.01) && near(seen.b, 0.4, 0.01) && !near(in.c[0], 0.2, 0.01),
              num(seen.r) + "," + num(seen.g) + " / " + num(in.c[0]));
        const auto keep = core::from_srgb(sr.image, 0.2f, 0.6f, 0.4f);
        check("sRGB document: a picked colour goes in as it is", near(keep.c[0], 0.2, 1e-6) && near(keep.c[1], 0.6, 1e-6), "");
    }

    std::cout << "\n-- colour management: Assign / Convert to Profile (s068) --\n";
    {
        namespace Pr = klimt::core::profile;
        const auto fresh = [] {
            auto d = std::make_unique<core::Document>();
            auto j = std::make_unique<core::DocJournal>(*d);
            d->set_door([jp = j.get()] { return jp->open(); });
            core::layers::load_fixture(*d);   // white; a red square at (100, 100) 200 x 150
            return std::make_pair(std::move(d), std::move(j));
        };
        const auto seen = [](const core::Document& d, int x, int y) { return core::shown(d.image(), core::sample_composite(d.image(), d.snapshot(), x, y)); };
        {
            auto [d, j] = fresh();
            const auto before = d->snapshot().begin()->second.pixels;
            check("assign: a CMYK profile on an RGB picture refused; junk refused; Adobe RGB fits",
                  !Pr::assign_refusal(d->image(), cmyk).empty() && !Pr::assign_refusal(d->image(), std::make_shared<const T::Icc>(T::Icc{1, 2})).empty() &&
                  Pr::assign_refusal(d->image(), adobe).empty() && !Pr::assign(*j, *d, cmyk), "");
            const auto n = j->size();
            const bool ok = Pr::assign(*j, *d, adobe);
            const auto red = core::sample_composite(d->image(), d->snapshot(), 150, 150);
            const auto look = seen(*d, 150, 150);
            check("assign: Adobe RGB -- one step; the numbers kept (red 1, 0, 0); the look changes (red past sRGB's: clipped red, a little green gone)",
                  ok && j->size() == n + 1 && d->image().icc == adobe && near(red.r, 1, 1e-6) && near(red.g, 0, 1e-6) &&
                  near(look.r, 1, 0.01) && near(look.g, 0, 0.01) && core::display_managed(d->image()), num(look.r));
            j->undo();
            check("assign: undone -- no profile again", !d->image().icc, "");
        }
        {
            auto [d, j] = fresh();
            const auto look0 = seen(*d, 150, 150), white0 = seen(*d, 20, 20);
            const bool ok = Pr::convert(*j, *d, adobe);
            const auto raw = core::sample_composite(d->image(), d->snapshot(), 150, 150);
            const auto look = seen(*d, 150, 150), white = seen(*d, 20, 20);
            check("convert to Adobe RGB: the numbers change (sRGB red is less than full Adobe red), the look stays",
                  ok && d->image().icc == adobe && d->image().format.mode == P::Mode::RGB && raw.r < 0.9f && raw.r > 0.8f &&
                  near(look.r, look0.r, 0.01) && near(look.g, look0.g, 0.01) && near(white.r, white0.r, 0.005), num(raw.r) + " -> " + num(look.r));
            j->undo();
            check("convert: undone -- the numbers and no profile back", !d->image().icc && near(core::sample_composite(d->image(), d->snapshot(), 150, 150).r, 1, 1e-6), "");
        }
        {
            auto [d, j] = fresh();
            klimt::text::Spec ts;
            ts.text = "Klimt";
            ts.colour = {0, 0, 1, 1};
            const auto tid = core::layers::add_text(*j, *d, ts);
            const bool ok = Pr::convert(*j, *d, cmyk);
            const auto& im = d->image();
            const auto ink = core::sample_composite(im, d->snapshot(), 150, 150);   // shown: through the profile
            bool formats = true;
            for (const auto& [id, l] : d->snapshot()) if (l.pixels && !(l.pixels->format() == im.format)) formats = false;
            check("convert to CMYK: Image > Mode -- the document CMYK, every layer (the text set again) in CMYK, the red square still red",
                  ok && im.format.mode == P::Mode::CMYK && formats && !tid.empty() && near(ink.r, 1, 0.03) && near(ink.g, 0, 0.03) && near(ink.b, 0, 0.03),
                  num(ink.r) + "," + num(ink.g));
            const auto gray = std::make_shared<const T::Icc>(T::gray(2.2));
            check("convert again, CMYK to Gray Gamma 2.2: Gray; the red square a mid-dark grey",
                  Pr::convert(*j, *d, gray) && d->image().format.mode == P::Mode::Gray &&
                  [&] { const auto g = core::sample_composite(d->image(), d->snapshot(), 150, 150); return g.r > 0.3 && g.r < 0.65 && near(g.r, g.b, 0.004); }(), "");
            j->undo(); j->undo();
            check("convert: both undone -- RGB, no profile", d->image().format.mode == P::Mode::RGB && !d->image().icc, "");
        }
    }

    std::cout << "\n-- colour management: the profile lists (s068 m2) --\n";
    {
        namespace Pr = klimt::core::profile;
        namespace fs = std::filesystem;
        const auto dir = fs::temp_directory_path() / "klimt_selftest_icc";
        fs::remove_all(dir);
        fs::create_directories(dir / "sub");
        { std::ofstream f(dir / "sub" / "wide.ICM", std::ios::binary); const auto b = T::rgb(std::array<double, 6>{0.7347, 0.2653, 0.1596, 0.8404, 0.0366, 0.0001}.data(), 1.8, "Wide Test RGB"); f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); }
        { std::ofstream f(dir / "junk.icc"); f << "not a profile"; }
        { std::ofstream f(dir / "notes.txt"); f << "x"; }
        const auto cat = Pr::catalog({dir.string()});
        bool wide = false;
        for (const auto& c : cat) wide = wide || (c.name == "Wide Test RGB" && c.mode == P::Mode::RGB && !c.path.empty());
        check("catalog: Klimt's six first (sRGB, Adobe RGB, two greys, Plain CMYK, Lab), then the folder's -- .icm found in a sub-folder, junk and other files left out",
              cat.size() == 7 && cat[0].name == "sRGB IEC61966-2.1" && cat[4].mode == P::Mode::CMYK && cat[5].mode == P::Mode::Lab && wide,
              std::to_string(cat.size()) + " " + cat[0].name);
        std::string why;
        check("load: junk refused with the reason; a real one read with its name and mode",
              !Pr::load((dir / "junk.icc").string(), why) && !why.empty() && Pr::load((dir / "sub" / "wide.ICM").string(), why).has_value(), why);
        check("Image > Mode's profiles: sRGB, Gray Gamma 2.2, Plain CMYK, Lab D50 -- each of its mode",
              Pr::mode_of(*Pr::builtin(P::Mode::RGB)) == P::Mode::RGB && Pr::mode_of(*Pr::builtin(P::Mode::Gray)) == P::Mode::Gray &&
              Pr::mode_of(*Pr::builtin(P::Mode::CMYK)) == P::Mode::CMYK && Pr::mode_of(*Pr::builtin(P::Mode::Lab)) == P::Mode::Lab &&
              T::inspect(*Pr::builtin(P::Mode::Lab)).name == "Lab D50", "");
        fs::remove_all(dir);
    }

    std::cout << "\n-- colour management: Proof Colors / Gamut Warning (s069) --\n";
    {
        namespace Pr = klimt::core::profile;
        // A "press" with a small gamut: an RGB of pale primaries (any space can be proofed to).
        const auto small = std::make_shared<const T::Icc>(T::rgb(std::array<double, 6>{0.45, 0.33, 0.32, 0.45, 0.25, 0.22}.data(), 2.2, "Small Test Press"));
        const auto ch = [](std::uint32_t v, int s) { return static_cast<int>(v >> s & 0xff); };
        const auto run = [](const std::shared_ptr<const T::Display>& d, std::vector<std::uint32_t> px) { if (d) d->run(px.data(), px.size()); return px; };
        check("proof: nothing asked, no press, or a junk press -- no transform",
              !T::proof(nullptr, small, false, false, false) && !T::proof(nullptr, nullptr, true, false, true) &&
              !T::proof(nullptr, std::make_shared<const T::Icc>(T::Icc{1, 2, 3}), true, false, true) &&
              T::proof(nullptr, small, true, false, false) && T::proof(nullptr, small, false, false, true), "");
        check("proof: made once, shared (the same press and flags)", T::proof(nullptr, small, true, false, false) == T::proof(nullptr, small, true, false, false) &&
              T::proof(nullptr, small, true, false, false) != T::proof(nullptr, small, true, false, true), "");
        const std::vector<std::uint32_t> in{0xff808080u, 0xffff0000u, 0xff806050u, 0x00000000u, 0x80800000u};
        const auto pc = run(T::proof(nullptr, small, true, false, false), in);
        check("Proof Colors: grey stays grey; sRGB's red shown as the small press prints it -- duller (green / blue rise)",
              std::abs(ch(pc[0], 16) - ch(pc[0], 8)) <= 2 && std::abs(ch(pc[0], 8) - ch(pc[0], 0)) <= 2 &&
              ch(pc[1], 8) > 40 && ch(pc[1], 0) > 40, std::to_string(ch(pc[1], 16)) + "," + std::to_string(ch(pc[1], 8)) + "," + std::to_string(ch(pc[1], 0)));
        check("Proof Colors: transparent stays transparent; half-covered keeps its alpha", pc[3] == 0 && (pc[4] >> 24) == 0x80, "");
        const auto gw = run(T::proof(nullptr, small, false, false, true), in);
        const std::uint32_t grey = 0xff000000u | (T::kGamutGrey << 16) | (T::kGamutGrey << 8) | T::kGamutGrey;
        check("Gamut Warning: sRGB's red (out of the small press) marked grey; a soft brown inside it shown as itself",
              gw[1] == grey && std::abs(ch(gw[2], 16) - 0x80) <= 2 && std::abs(ch(gw[2], 8) - 0x60) <= 2 && std::abs(ch(gw[2], 0) - 0x50) <= 2,
              std::to_string(gw[1]) + " / " + std::to_string(ch(gw[2], 16)));
        {
            std::vector<std::uint32_t> greys;
            for (std::uint32_t i = 0; i < 256; ++i) greys.push_back(0xff000000u | i << 16 | i << 8 | i);
            const auto gs = run(T::proof(nullptr, small, false, false, true), greys), gc = run(T::proof(nullptr, cmyk, false, false, true), greys);
            check("Gamut Warning: no grey, black to white, marked (black point compensated, as the proof does)", gs == greys && gc == greys, "");
        }
        const auto both = run(T::proof(nullptr, small, true, false, true), in);
        check("both: red grey, the brown proofed", both[1] == grey && both[2] == pc[2], "");
        // An Adobe RGB document's greens are past CMYK; its browns are not.
        const auto ag = run(T::proof(adobe, cmyk, false, false, true), {0xff00ff00u, 0xff806050u});
        check("Gamut Warning: Adobe RGB's pure green past Plain CMYK -- grey; its brown printable", ag[0] == grey && ag[1] != grey, std::to_string(ag[1]));
        check("Simulate Paper Colour: a transform (absolute back to the screen)", T::proof(nullptr, cmyk, true, true, false) != nullptr, "");
        // core: the document's tiles through it.
        const auto sd = doc(P::Mode::RGB, P::rgba(1, 0, 0), nullptr);
        std::vector<std::uint32_t> a{0xffff0000u}, b{0xffff0000u};
        core::shown_argb(sd.image, a, core::Proofing{});
        core::shown_argb(sd.image, b, core::Proofing{small, true, false, false});
        check("core: proofing off -- the tiles as before; on -- through the press", a[0] == 0xffff0000u && b[0] == pc[1], std::to_string(b[0]));
        std::vector<std::uint32_t> c{0xffff0000u};
        core::shown_argb(sd.image, c, core::Proofing{small, false, false, true});
        check("core: Gamut Warning on an untagged RGB document (taken as sRGB)", c[0] == grey, "");
        const auto cd = doc(P::Mode::CMYK, ink(0, 1, 1, 0), cmyk);
        std::vector<std::uint32_t> d{0xffee1c24u}, e{0xffee1c24u};
        core::shown_argb(cd.image, d, core::Proofing{cmyk, true, false, true});
        core::shown_argb(cd.image, e);
        check("core: a CMYK document proofed to its own profile -- shown as itself, nothing marked", d == e, "");
        const auto ad = doc(P::Mode::RGB, P::rgba(0, 1, 0), adobe);
        std::vector<std::uint32_t> f{0xff00ff00u};
        core::shown_argb(ad.image, f, core::Proofing{cmyk, false, false, true});
        check("core: an Adobe RGB document's green proofed from Adobe RGB (not sRGB) -- marked", f[0] == grey, std::to_string(f[0]));
        // Proof Setup's press, remembered.
        const auto none = std::vector<std::string>{"/nonexistent-klimt-icc"};
        const auto def = Pr::proof_choice("", none);
        check("Proof Setup: no system SWOP -- Plain CMYK the default", def.mode == P::Mode::CMYK && def.name == "Plain CMYK (no dot gain)" && def.path.empty(), def.name);
        const auto lab = Pr::proof_choice("Lab D50", none);
        check("Proof Setup: one of Klimt's own remembered by name; a gone file falls back to the default",
              lab.name == "Lab D50" && Pr::remembered_as(lab) == "Lab D50" && Pr::proof_choice("/gone/press.icc", none).name == def.name, lab.name);
        namespace fs = std::filesystem;
        const auto file = fs::temp_directory_path() / "klimt_selftest_press.icc";
        { std::ofstream o(file, std::ios::binary); o.write(reinterpret_cast<const char*>(small->data()), static_cast<std::streamsize>(small->size())); }
        const auto loaded = Pr::proof_choice(file.string(), none);
        check("Proof Setup: a loaded file remembered by its path", loaded.name == "Small Test Press" && Pr::remembered_as(loaded) == file.string(), loaded.name);
        fs::remove(file);
    }

    std::cout << "\n-- colour management: Colour Settings -- intents, working spaces (s070) --\n";
    {
        namespace Pr = klimt::core::profile;
        T::Intent i{};
        bool keys = true;
        for (const auto k : {T::Intent::Perceptual, T::Intent::Relative, T::Intent::Saturation, T::Intent::Absolute})
            keys = keys && T::intent_from(T::intent_key(k), i) && i == k && std::string(T::intent_label(k)).size() > 5;
        check("intents: the four keys round trip; junk refused", keys && !T::intent_from("vivid", i), "");
        const float green[3] = {0.f, 1.f, 0.f};
        bool all = true;
        float rel[4] = {}, def[4] = {};
        for (const auto k : {T::Intent::Perceptual, T::Intent::Relative, T::Intent::Saturation, T::Intent::Absolute}) {
            const auto l = T::link(*adobe, T::Space::RGB, *cmyk, T::Space::CMYK, T::Render{k, true});
            all = all && l;
            if (l && k == T::Intent::Relative) l->run(green, rel, 1);
        }
        if (const auto l = T::link(*adobe, T::Space::RGB, *cmyk, T::Space::CMYK)) l->run(green, def, 1);
        check("intents: a link for each (Adobe RGB -> CMYK); the default is Relative + BPC", all && near(rel[0], def[0], 1e-6) && near(rel[3], def[3], 1e-6), "");
        check("intents: proofs differ by intent (cached apart)",
              T::proof(nullptr, cmyk, true, false, false, T::Render{T::Intent::Perceptual, true}) != T::proof(nullptr, cmyk, true, false, false), "");

        // Working spaces: an untagged Gray picture read as Gray Gamma 1.8.
        const auto gd = doc(P::Mode::Gray, [] { P::Px p; p.c = {0.5f, 1}; return p; }(), nullptr);
        const auto before = core::sample_composite(gd.image, gd.layers, 3, 3);
        check("working: none set -- untagged Gray / CMYK unmanaged, unnamed; Image > Mode's targets Klimt's",
              !core::managed(gd.image) && core::profile_name(gd.image).empty() && !T::working(T::Space::CMYK) &&
              Pr::mode_profile(P::Mode::CMYK) == Pr::builtin(P::Mode::CMYK), "");
        T::set_working(T::Space::Gray, g18);
        T::set_working(T::Space::CMYK, cmyk);
        const auto after = core::sample_composite(gd.image, gd.layers, 3, 3);
        check("working Gray Gamma 1.8: the untagged grey read through it (shown lighter than on sRGB's curve), named \"(working)\"",
              core::managed(gd.image) && after.r > before.r + 0.01 && core::profile_name(gd.image) == "Gray Gamma 1.8 (working)",
              num(before.r) + " -> " + num(after.r) + " " + core::profile_name(gd.image));
        check("working: a picture with its own profile keeps it; RGB and Lab untouched",
              core::profile_name(doc(P::Mode::Gray, [] { P::Px p; p.c = {0.5f, 1}; return p; }(), std::make_shared<const T::Icc>(T::gray(2.2))).image).find("(working)") == std::string::npos &&
              !T::working(T::Space::RGB) && core::profile_name(doc(P::Mode::RGB, P::rgba(1, 0, 0), nullptr).image).empty(), "");
        check("working: Image > Mode's Gray / CMYK targets are the working spaces", Pr::mode_profile(P::Mode::Gray) == g18 && Pr::mode_profile(P::Mode::CMYK) == cmyk &&
              Pr::mode_profile(P::Mode::RGB) == Pr::builtin(P::Mode::RGB), "");
        {   // Convert: the untagged grey taken as the working space's -- its look kept.
            auto d = std::make_unique<core::Document>();
            auto j = std::make_unique<core::DocJournal>(*d);
            d->set_door([jp = j.get()] { return jp->open(); });
            core::layers::load_fixture(*d);
            const bool to_g = Pr::convert(*j, *d, std::make_shared<const T::Icc>(T::gray(2.2))) && Pr::assign(*j, *d, nullptr) &&
                              d->image().format.mode == P::Mode::Gray && !d->image().icc;
            const auto s1 = core::sample_composite(d->image(), d->snapshot(), 150, 150);
            const bool ok = Pr::convert(*j, *d, Pr::builtin(P::Mode::RGB));
            const auto s2 = core::sample_composite(d->image(), d->snapshot(), 150, 150);
            check("working: Convert reads an untagged grey as Gray Gamma 1.8 (the look it showed stays)",
                  to_g && ok && d->image().format.mode == P::Mode::RGB && near(s2.r, s1.r, 0.01), num(s1.r) + " -> " + num(s2.r));
        }
        const auto proof_default = Pr::proof_choice("", {"/nonexistent-klimt-icc"});
        check("Proof Setup's default follows the working CMYK", proof_default.icc && *proof_default.icc == *cmyk, proof_default.name);
        T::set_working(T::Space::Gray, nullptr);
        T::set_working(T::Space::CMYK, nullptr);
        check("working: cleared -- back to the plain conversion", !core::managed(gd.image) && near(core::sample_composite(gd.image, gd.layers, 3, 3).r, before.r, 1e-6), "");
        std::string why;
        check("find: Klimt's own by name, a file by path; \"\" and a gone file nothing",
              Pr::find("Plain CMYK (no dot gain)").has_value() && !Pr::find("").has_value() && !Pr::find("/gone/x.icc").has_value() &&
              !Pr::find("No Such Profile").has_value(), "");
    }

    std::cout << "\n-- colour management: layer styles through the profile; the preview (s071) --\n";
    {
        auto ad = doc(P::Mode::RGB, P::rgba(1, 0, 0), adobe);
        core::Layer* L = nullptr;
        for (auto& [id, l] : ad.layers) if (l.kind == core::Kind::Pixel && l.pixels) { L = &l; break; }
        if (L) {
            L->styles.overlay.on = true;
            L->styles.overlay.colour = {0, 1, 0};
            const auto with = core::live::look(*L, &ad.image), plain = core::live::look(*L);
            const auto want = core::from_srgb(ad.image, 0, 1, 0);
            const auto w = with->get(5, 5), p = plain->get(5, 5);
            check("styles: an Adobe RGB document's Color Overlay green goes in through its profile (not the raw sRGB numbers)",
                  near(w.c[0], want.c[0], 0.01) && near(w.c[1], want.c[1], 0.01) && near(w.c[2], want.c[2], 0.01) && w.c[0] > 0.2f && near(p.c[0], 0, 0.01),
                  num(w.c[0]) + "," + num(w.c[1]) + "," + num(w.c[2]) + " / plain " + num(p.c[0]));
            const auto sr = doc(P::Mode::RGB, P::rgba(1, 0, 0), nullptr);
            check("styles: an untagged document's colour profile is none (plain, as before)", !core::colour_profile(sr.image) && core::colour_profile(ad.image) == adobe, "");
        } else check("styles: a pixel layer to style", false, "");
        T::set_working(T::Space::CMYK, cmyk);
        check("colour_profile: an untagged CMYK follows the working CMYK", core::colour_profile(doc(P::Mode::CMYK, ink(0, 0, 0, 0), nullptr).image) == cmyk, "");
        T::set_working(T::Space::CMYK, nullptr);
    }

    std::cout << "\n-- colour management: the monitor's profile (s071) --\n";
    {
        namespace Pr = klimt::core::profile;
        using D = Pr::MonitorDevice;
        const std::vector<D> ds{{"HDMI-1", false, {}}, {"DP-1", false, {"/a.icc", "/b.icc"}}, {"eDP-1", true, {"/laptop.icc"}}};
        check("monitor: the window's connector picks its device's default profile; unknown -> the primary; none at all -> \"\"",
              Pr::monitor_profile(ds, "DP-1") == "/a.icc" && Pr::monitor_profile(ds, "HDMI-1") == "/laptop.icc" &&
              Pr::monitor_profile(ds, "") == "/laptop.icc" && Pr::monitor_profile({{"X", false, {"/x.icc"}}}, "Y") == "/x.icc" &&
              Pr::monitor_profile({}, "DP-1").empty(), "");
        T::set_screen(std::make_shared<const T::Icc>(T::srgb()));
        const bool srgb_none = !T::screen();
        T::set_screen(std::make_shared<const T::Icc>(T::Icc{1, 2, 3}));
        const bool junk_none = !T::screen();
        T::set_screen(cmyk);
        check("screen: an sRGB, junk or non-RGB monitor profile -- the screen stays sRGB", srgb_none && junk_none && !T::screen() && !T::to_screen(nullptr), "");
        T::set_screen(adobe);   // a wide-gamut monitor
        const auto ch = [](std::uint32_t v, int sh) { return static_cast<int>(v >> sh & 0xff); };
        std::vector<std::uint32_t> px{0xffff0000u, 0xff808080u, 0x00000000u};
        if (const auto d = T::to_screen(nullptr)) d->run(px.data(), px.size());
        check("screen Adobe RGB: sRGB's red sent as less red numbers (the monitor's red is redder); grey stays grey; transparent stays",
              T::screen() == adobe && ch(px[0], 16) < 0xf0 && std::abs(ch(px[1], 16) - ch(px[1], 8)) <= 1 && px[2] == 0, std::to_string(ch(px[0], 16)));
        const auto sd = doc(P::Mode::RGB, P::rgba(1, 0, 0), nullptr);
        std::vector<std::uint32_t> t{0xffff0000u};
        core::shown_argb(sd.image, t);
        std::vector<std::uint32_t> a{0xffff0000u};
        core::shown_argb(doc(P::Mode::RGB, P::rgba(1, 0, 0), adobe).image, a);
        check("screen: an untagged document's tiles go to the monitor; an Adobe RGB one on an Adobe RGB monitor is left as it is",
              t[0] == px[0] && a[0] == 0xffff0000u, std::to_string(t[0]) + " / " + std::to_string(a[0]));
        std::vector<std::uint32_t> pr{0xff808080u};
        if (const auto d = T::proof(nullptr, cmyk, true, false, false)) d->run(pr.data(), pr.size());
        check("screen: a proof ends on the monitor too (made apart from the sRGB one)", T::proof(nullptr, cmyk, true, false, false) != nullptr &&
              std::abs(ch(pr[0], 16) - ch(pr[0], 0)) <= 2, "");
        T::set_screen(nullptr);
        std::vector<std::uint32_t> back{0xffff0000u};
        core::shown_argb(sd.image, back);
        check("screen: cleared -- untagged tiles untouched again", back[0] == 0xffff0000u && !T::to_screen(nullptr), "");
    }
}
