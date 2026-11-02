#pragma once
// core::raw -- CAMERA RAW through LibRaw (s085; REQUIREMENTS "Added" 2;
// docs/raw.md). A camera's RAW file (CR2 / CR3 / NEF / ARW / RAF / ORF /
// RW2 / PEF / DNG ...) DEVELOPED into a 16-bit RGB picture: demosaiced, white
// balanced, exposed, its highlights handled, the camera's colour carried into
// sRGB, Adobe RGB or ProPhoto (the profile attached for the last two).
//
// Opening a RAW anywhere (Open, a drop, Place, Batch) develops it with the
// settings last used in the Develop dialog (`Develop{}` the first time).
// File > Open shows the dialog first -- the embedded preview at once, then
// a half-size develop of the settings as they change (on a worker).
//
// LibRaw is optional at build time (KLIMT_HAVE_LIBRAW): without it every
// call here refuses in words ("built without LibRaw -- install
// LibRaw-devel"). GTK-free; every call is pure over its arguments and runs
// on a worker (libraw_r: thread safe).
#include "core/File.hpp"
#include "formats/Formats.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::raw {

// Built with LibRaw? (its version, "" when not)
bool available();
std::string version();

// A RAW file by its name (the extension) -- the TIFF-based ones (CR2, NEF,
// ARW, DNG ...) would otherwise open as their small TIFF preview.
bool is_raw_name(const std::string& path);
// By its name, or (a file Klimt doesn't otherwise know) LibRaw takes it.
bool is_raw(const std::string& path);

enum class Balance : std::uint8_t { AsShot, Auto, Daylight };
enum class Highlights : std::uint8_t { Clip, Blend, Rebuild };
enum class Space : std::uint8_t { SRGB, AdobeRGB, ProPhoto };
const char* balance_label(Balance b);        // "As Shot", "Auto", "Daylight"
const char* highlights_label(Highlights h);  // "Clip", "Blend", "Rebuild"
const char* space_label(Space s);            // "sRGB", "Adobe RGB (1998)", "ProPhoto RGB"

struct Develop {
    Balance    balance = Balance::AsShot;
    int        warmth = 0;            // -100 .. 100: red up / blue down (As Shot, Daylight)
    double     exposure = 0;          // EV, -2 .. +3
    bool       auto_bright = true;    // LibRaw's: the brightest 1 % to white
    Highlights highlights = Highlights::Blend;
    int        noise = 0;             // wavelet denoising threshold, 0 (off) .. 1000
    Space      space = Space::SRGB;
    bool operator==(const Develop&) const = default;
};
// As text (the prefs keep the last used) and back (fields absent: defaults).
std::string develop_text(const Develop& d);
Develop develop_parse(const std::string& text);

// What the file says about itself.
struct Info {
    std::string make, model, lens;
    int         width = 0, height = 0;   // the developed picture's (turned as shot)
    double      iso = 0, shutter = 0, aperture = 0, focal = 0;
    std::int64_t taken = 0;              // seconds since the epoch (0: unknown)
    std::string line() const;            // "Canon EOS R5 · ISO 400 · 1/250 s · f/8 · 50 mm · 8192 × 5464"
};
std::optional<Info> info(const std::string& path, std::string& error);

// The camera's own small JPEG (or bitmap) -- the dialog's first look.
// 8-bit RGBA, turned as shot. nullopt: none in the file.
std::optional<pixel::TileMap> preview(const std::string& path, std::string& error);

// Developed. `half`: half size, a quicker demosaic (the dialog's preview).
// 16-bit RGB with alpha (opaque); nullopt with `error` in words, or
// cancelled (p.cancelled).
struct Developed {
    pixel::TileMap pixels;
    std::vector<std::uint8_t> icc;   // empty: sRGB (no profile attached)
    Info           about;
};
std::optional<Developed> develop(const std::string& path, const Develop& d, bool half, std::string& error,
                                 const formats::Progress& p = {});

// file::open's way in: the picture as a document of one layer ("Background"),
// kind Unknown, the note "developed: As Shot, +0.5 EV, sRGB". Uses `d`.
std::optional<file::Contents> open(const std::string& path, const Develop& d, std::string& error,
                                   const formats::Progress& p = {});
// The settings a plain open uses (the Develop dialog's last OK), set by the
// Shell from the prefs; process-wide, thread safe.
void set_defaults(const Develop& d);
Develop defaults();

}  // namespace klimt::core::raw
