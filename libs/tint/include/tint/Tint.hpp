#pragma once
// libs/tint -- COLOUR MANAGEMENT (s067; WEAKNESSES 14). docs/colour.md.
//
// A document's ICC profile (core::Image::icc) says what its numbers mean.
// tint links that profile with sRGB -- the space the screen and the colour
// pickers speak -- both ways, through Little CMS 2: relative colorimetric
// with black point compensation (Photoshop's "Relative Colorimetric, BPC",
// its default). Links are made once per profile and shared (thread safe).
//
// Colours in and out are Klimt's own 0..1 encoding, straight (not
// premultiplied): Gray v; RGB r g b; CMYK c m y k as INK (0 none .. 1 full);
// Lab L 0..1 = 0..100, a / b 0..1 = -128..127. Worked at 16 bits.
// GTK-free.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace klimt::tint {

enum class Space : std::uint8_t { Gray, RGB, CMYK, Lab };
using Icc = std::vector<std::uint8_t>;

// s070: HOW a conversion maps one gamut into another -- ICC's four rendering
// intents, and black point compensation. Klimt's default (and Photoshop's):
// Relative Colorimetric with BPC.
enum class Intent : std::uint8_t { Perceptual, Relative, Saturation, Absolute };
struct Render {
    Intent intent = Intent::Relative;
    bool   bpc = true;
    bool operator==(const Render&) const = default;
};
const char* intent_key(Intent i);     // "perceptual", "relative", "saturation", "absolute"
const char* intent_label(Intent i);   // "Perceptual", "Relative Colorimetric", ...
bool intent_from(const std::string& key, Intent& out);

int channels(Space s);

// What a profile is: readable, its colour space, its description.
struct Info {
    bool        ok = false;
    Space       space = Space::RGB;
    std::string name;
};
Info inspect(const Icc& icc);

// One direction between a profile and sRGB.
class Link {
public:
    virtual ~Link() = default;
    int in = 0, out = 0;   // channels
    // n colours: `src` holds n x in values, `dst` gets n x out.
    virtual void run(const float* src, float* dst, std::size_t n) const = 0;
};
// The profile (in space `s`) to sRGB, and sRGB to it. nullptr when the
// profile can't be read or isn't of space `s` (the caller keeps its own
// unmanaged conversion then).
std::shared_ptr<const Link> to_srgb(const Icc& icc, Space s);
std::shared_ptr<const Link> from_srgb(const Icc& icc, Space s);
// The same for a shared profile (a document's): found by the pointer, so
// asking per tile costs nothing -- the cache holds the profile while it
// holds its links.
using IccRef = std::shared_ptr<const Icc>;
std::shared_ptr<const Link> to_srgb(const IccRef& icc, Space s);
std::shared_ptr<const Link> from_srgb(const IccRef& icc, Space s);

// Is it sRGB -- near enough that a link would change nothing (an RGB
// profile whose primaries, white and curve are sRGB's)?
bool is_srgb(const Icc& icc);
bool is_srgb(const IccRef& icc);   // cached by the profile

// s067 m2: the SCREEN's way -- an RGB document's colours (Cairo's
// premultiplied ARGB32, native-endian, 8 bits) turned to sRGB in place.
class Display {
public:
    virtual ~Display() = default;
    virtual void run(std::uint32_t* argb, std::size_t n) const = 0;
};
// nullptr when there's nothing to do: no profile, sRGB, not RGB, unreadable.
std::shared_ptr<const Display> display(const IccRef& icc);

// s069: SOFT PROOF -- the screen's colours as `press` would print them
// (View > Proof Colors), and / or what it can't reach marked (View > Gamut
// Warning: those pixels painted `kGamutGrey`). `source` is the RGB profile of
// the colours handed in (nullptr: sRGB); `press` any profile of any space (a
// press / printer CMYK, most often). Relative colorimetric + BPC into the
// press; `paper` (Simulate Paper Colour): back to the screen absolute
// colorimetric -- the paper's white and the ink's black shown as they are.
// nullptr: neither `colours` nor `gamut`, or a profile unusable. Cached by
// the profiles' pointers and the flags; thread safe.
inline constexpr std::uint32_t kGamutGrey = 0x80;   // 50% grey (Photoshop's default)
// s070: `r` -- the intent / BPC into the press (Proof Setup's).
std::shared_ptr<const Display> proof(const IccRef& source, const IccRef& press, bool colours, bool paper, bool gamut,
                                     Render r = {});

// s071: THE SCREEN -- the monitor's own profile (from colord). Every tile
// goes from the composite's space (an RGB document's own, or sRGB) to it,
// not to sRGB. nullptr (the default, and for an sRGB-like or unusable or
// non-RGB profile): the screen is taken as sRGB, as before. Process-wide,
// thread safe. The colour pickers and the Eyedropper still speak sRGB.
void   set_screen(IccRef icc);
IccRef screen();
// The tiles' way: `source` (an RGB profile; nullptr: sRGB) to the screen.
// nullptr when there's nothing to do (both sRGB). Cached.
std::shared_ptr<const Display> to_screen(const IccRef& source);

// s070: WORKING SPACES (Edit > Colour Settings) -- what an UNTAGGED Gray or
// CMYK document's numbers are taken to mean. nullptr (the default): the
// plain conversion of s007. Process-wide, thread safe; set from the UI
// thread, read by the workers.
void   set_working(Space s, IccRef icc);   // Gray or CMYK; others ignored
IccRef working(Space s);

// Profiles made here, for new documents and the tests: sRGB; a grey with a
// pure gamma; an RGB of the given primaries (xy), D65 white and gamma (Adobe
// RGB (1998): 0.64 0.33, 0.21 0.71, 0.15 0.06, gamma 2.2); and a plain CMYK
// -- the textbook conversion (1-c)(1-k) as ICC, no dot gain.
// s068: profile to profile, directly (no stop in sRGB -- a CMYK or wide RGB
// gamut isn't clipped on the way). nullptr: either one unusable or not of
// its stated space. Not cached: one per Convert.
// s070: `r` the intent and BPC (Convert to Profile's).
std::shared_ptr<const Link> link(const Icc& from, Space fs, const Icc& to, Space ts, Render r = {});
// s068: what a document's numbers are taken to mean when it carries no
// profile -- the plain conversions s007 shows: sRGB; a grey on sRGB's curve;
// plain CMYK; Lab D50.
Icc implicit(Space s);

Icc srgb();
Icc gray(double gamma);
Icc rgb(const double primaries[6], double gamma, const std::string& name);
Icc adobe_rgb();
Icc prophoto_rgb();   // s085: ROMM primaries, D50, gamma 1.8 (camera RAW's widest)
Icc plain_cmyk();

}  // namespace klimt::tint
