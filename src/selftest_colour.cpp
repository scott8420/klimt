// s067 (WEAKNESSES 14): colour management -- libs/tint, and a Gray / CMYK /
// document shown and painted through its profile. docs/colour.md.
#include "core/Composite.hpp"
#include "core/File.hpp"
#include "core/Document.hpp"
#include "core/Profile.hpp"
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
}
