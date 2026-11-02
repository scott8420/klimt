// klimt_selftest -- s085: camera RAW through LibRaw (core/Raw.hpp). The test
// writes its own DNG (a Bayer RGGB mosaic: red on the left, blue on the right)
// with libtiff, so no camera file is needed. Without LibRaw (Scott hasn't
// installed LibRaw-devel yet) only the refusal is checked -- the app still
// builds and starts.
#include "core/File.hpp"
#include "core/Raw.hpp"
#include "pixel/Pixel.hpp"

#include <tiffio.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

namespace P  = klimt::pixel;
namespace core = klimt::core;
namespace raw = klimt::core::raw;
namespace fs = std::filesystem;

using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
fs::path scratch() {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_raw_" + std::to_string(::getpid()));
        fs::create_directories(p);
        return p;
    }();
    return d;
}
// A DNG: W x H, 16-bit RGGB, the left half red, the right half blue, a dim
// green throughout; its camera colour linear sRGB; `orient` its EXIF turn.
bool make_dng(const std::string& path, int W, int H, int orient) {
    TIFFSetWarningHandler(nullptr);
    TIFF* t = TIFFOpen(path.c_str(), "w");
    if (!t) return false;
    static const TIFFFieldInfo cfa[] = {
        {33421, 2, 2, TIFF_SHORT, FIELD_CUSTOM, 1, 0, const_cast<char*>("CFARepeatPatternDim")},
        {33422, -1, -1, TIFF_BYTE, FIELD_CUSTOM, 1, 1, const_cast<char*>("CFAPattern")},
    };
    TIFFMergeFieldInfo(t, cfa, 2);
    TIFFSetField(t, TIFFTAG_SUBFILETYPE, 0);
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH, W);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH, H);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, 16);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 1);
    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC, 32803);   // CFA
    TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_ORIENTATION, orient);
    TIFFSetField(t, TIFFTAG_MAKE, "Klimt");
    TIFFSetField(t, TIFFTAG_MODEL, "Test Camera");
    const std::uint16_t dim[2] = {2, 2};
    TIFFSetField(t, 33421, dim);
    const std::uint8_t pat[4] = {0, 1, 1, 2};
    TIFFSetField(t, 33422, 4, pat);
    const std::uint8_t ver[4] = {1, 4, 0, 0}, bver[4] = {1, 1, 0, 0};
    TIFFSetField(t, TIFFTAG_DNGVERSION, ver);
    TIFFSetField(t, TIFFTAG_DNGBACKWARDVERSION, bver);
    TIFFSetField(t, TIFFTAG_UNIQUECAMERAMODEL, "Klimt Test Camera");
    const float cm[9] = {3.2406f, -1.5372f, -0.4986f, -0.9689f, 1.8758f, 0.0415f, 0.0557f, -0.2040f, 1.0570f};
    TIFFSetField(t, TIFFTAG_COLORMATRIX1, 9, cm);
    TIFFSetField(t, TIFFTAG_CALIBRATIONILLUMINANT1, 21);
    const float neutral[3] = {1, 1, 1};
    TIFFSetField(t, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
    const std::uint32_t white = 65535;
    TIFFSetField(t, TIFFTAG_WHITELEVEL, 1, &white);
    std::vector<std::uint16_t> row(static_cast<std::size_t>(W));
    bool ok = true;
    for (int y = 0; y < H && ok; ++y) {
        for (int x = 0; x < W; ++x) {
            const int c = (y & 1) ? ((x & 1) ? 2 : 1) : ((x & 1) ? 1 : 0);
            const bool left = x < W / 2;
            const double v = c == 0 ? (left ? 0.6 : 0.05) : c == 1 ? 0.1 : (left ? 0.05 : 0.6);
            row[static_cast<std::size_t>(x)] = static_cast<std::uint16_t>(v * 65535);
        }
        ok = TIFFWriteScanline(t, row.data(), static_cast<std::uint32_t>(y), 0) == 1;
    }
    TIFFClose(t);
    return ok;
}
double mean(const P::TileMap& m, int x0, int y0, int w, int h, int ch) {
    double s = 0;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) s += m.get(x, y).c[static_cast<std::size_t>(ch)];
    return s / (w * h);
}
}  // namespace

void selftest_raw(const Check& check) {
    check("raw: a camera file by its name (.CR2, .nef, .dng ...); not a PNG",
          raw::is_raw_name("/x/IMG_0001.CR2") && raw::is_raw_name("a.nef") && raw::is_raw_name("b.dng") && raw::is_raw_name("c.CR3") &&
              !raw::is_raw_name("d.png") && !raw::is_raw_name("e.tif"), "");
    {
        raw::Develop d;
        d.balance = raw::Balance::Daylight;
        d.warmth = 30;
        d.exposure = 1.5;
        d.auto_bright = false;
        d.highlights = raw::Highlights::Rebuild;
        d.noise = 200;
        d.space = raw::Space::ProPhoto;
        check("raw: the Develop settings as text and back", raw::develop_parse(raw::develop_text(d)) == d, raw::develop_text(d));
        check("raw: unreadable settings -- the defaults", raw::develop_parse("nonsense") == raw::Develop{}, "");
    }
    const std::string dng = (scratch() / "test.dng").string();
    const std::string turned = (scratch() / "turned.dng").string();
    const bool made = make_dng(dng, 64, 48, 1) && make_dng(turned, 64, 48, 6);
    check("raw: the test's own DNG written", made, "");
    if (!raw::available()) {   // built without LibRaw: refused, saying what to install
        std::string err;
        const auto c = core::file::open(dng, err);
        check("raw: built without LibRaw -- a RAW is refused, saying to install LibRaw-devel",
              !c && err.find("LibRaw-devel") != std::string::npos, err);
        return;
    }
    check("raw: LibRaw is here, its version said", !raw::version().empty(), raw::version());
    {
        std::string err;
        const auto i = raw::info(dng, err);
        check("raw: what the file says -- the camera, the picture's size",
              i && i->make == "Klimt" && i->model == "Test Camera" && i->width == 64 && i->height == 48 &&
                  i->line().rfind("Klimt Test Camera", 0) == 0, i ? i->line() : err);
        const auto t = raw::info(turned, err);
        check("raw: shot turned (EXIF 6) -- 48 x 64", t && t->width == 48 && t->height == 64, t ? t->line() : err);
    }
    raw::Develop flat;
    flat.auto_bright = false;
    {
        std::string err;
        const auto d = raw::develop(dng, flat, false, err);
        check("raw: developed -- 16-bit RGB, its full size, opaque", d && d->pixels.width() == 64 && d->pixels.height() == 48 &&
              d->pixels.format() == P::Format{P::Depth::U16, P::Mode::RGB, true} && d->pixels.get(30, 20).c[3] == 1.f, err);
        check("raw: demosaiced -- red on the left, blue on the right",
              d && mean(d->pixels, 4, 8, 16, 32, 0) > 2 * mean(d->pixels, 4, 8, 16, 32, 2) &&
                  mean(d->pixels, 44, 8, 16, 32, 2) > 2 * mean(d->pixels, 44, 8, 16, 32, 0), err);
        check("raw: sRGB -- no profile attached", d && d->icc.empty(), "");
        const auto t = raw::develop(turned, flat, false, err);
        check("raw: turned as shot -- 48 x 64, the red at the top", t && t->pixels.width() == 48 && t->pixels.height() == 64 &&
              mean(t->pixels, 8, 4, 32, 16, 0) > 2 * mean(t->pixels, 8, 4, 32, 16, 2), err);
        raw::Develop up = flat;
        up.exposure = 1;
        const auto u = raw::develop(dng, up, false, err);
        check("raw: +1 EV is brighter", u && d && mean(u->pixels, 44, 8, 16, 32, 1) > mean(d->pixels, 44, 8, 16, 32, 1) * 1.3, err);
        raw::Develop warm = flat;
        warm.balance = raw::Balance::Daylight;
        warm.warmth = 100;
        raw::Develop day = flat;
        day.balance = raw::Balance::Daylight;
        const auto w = raw::develop(dng, warm, false, err);
        const auto dd = raw::develop(dng, day, false, err);
        const auto ratio = [&](const raw::Developed& x) { return mean(x.pixels, 4, 8, 16, 32, 0) / std::max(1e-6, mean(x.pixels, 44, 8, 16, 32, 2)); };
        check("raw: warmer -- red up against blue", w && dd && ratio(*w) > ratio(*dd) * 1.1, err);
        raw::Develop adobe = flat;
        adobe.space = raw::Space::AdobeRGB;
        const auto a = raw::develop(dng, adobe, false, err);
        check("raw: Adobe RGB -- its profile attached", a && !a->icc.empty(), err);
        const auto h = raw::develop(dng, flat, true, err);
        check("raw: half size (the dialog's preview) -- 32 x 24", h && h->pixels.width() == 32 && h->pixels.height() == 24, err);
        klimt::formats::Progress stop;
        stop.cancelled = [] { return true; };
        std::string e2;
        const auto c = raw::develop(dng, flat, false, e2, stop);
        check("raw: cancelled -- nothing, said", !c && e2 == "cancelled", e2);
    }
    {
        std::string err;
        raw::set_defaults(flat);
        const auto c = core::file::open(dng, err);
        check("raw: File > Open of a DNG -- developed (not its TIFF preview), one layer, 16-bit",
              c && c->image.width == 64 && c->image.format.depth == P::Depth::U16 && c->layers.size() == 1 &&
                  !c->notes.empty() && c->notes[0].rfind("developed", 0) == 0, c ? c->notes[0] : err);
        raw::set_defaults({});
        const auto p = raw::preview(dng, err);
        check("raw: no preview inside the test's DNG -- said", !p && !err.empty(), err);
        const std::string junk = (scratch() / "junk.cr2").string();
        std::ofstream(junk) << "not a raw at all";
        std::string e3;
        const auto j = core::file::open(junk, e3);
        check("raw: a .cr2 that isn't one -- refused in words", !j && !e3.empty(), e3);
    }
}
