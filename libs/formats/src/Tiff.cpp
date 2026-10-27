// TIFF in and out, on libtiff (s007). The general file: Gray, RGB, CMYK
// (Separated, InkSet CMYK) and CIELab, at 8, 16 or 32-bit float, with or
// without alpha. Read: strip-organised, contiguous files come in at their own
// depth and mode; associated (premultiplied) alpha is divided out; unnamed
// extra samples (spot channels) are dropped with a note. Anything else --
// tiled, planar, palette, 1-bit, YCbCr-JPEG -- comes in through libtiff's
// RGBA reader as 8-bit RGBA, with a note saying so. First page only.
#include "formats/Formats.hpp"

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

namespace klimt::formats {

namespace {
thread_local char t_err[256];
void on_error(const char*, const char* fmt, va_list ap) { std::vsnprintf(t_err, sizeof t_err, fmt, ap); }
void on_warning(const char*, const char*, va_list) {}
struct Handlers {   // libtiff's handlers are global; set once, quietly
    Handlers() { TIFFSetErrorHandler(on_error); TIFFSetWarningHandler(on_warning); }
};
void quiet() { static Handlers h; }

struct Tif {
    TIFF* t = nullptr;
    Tif(const std::string& path, const char* mode) : t(TIFFOpen(path.c_str(), mode)) {}
    ~Tif() { if (t) TIFFClose(t); }
};

// The Lab encodings in, to Klimt's normalised Lab (L 0..100 -> 0..1; a, b
// -128..127 -> 0..1): CIELab stores a / b signed, ICCLab offset.
void lab_in(std::uint8_t* row, int width, int n, pixel::Depth d, bool signed_ab) {
    for (int x = 0; x < width; ++x) {
        if (d == pixel::Depth::U8) {
            std::uint8_t* p = row + static_cast<std::size_t>(x) * static_cast<std::size_t>(n);
            if (signed_ab) { p[1] = static_cast<std::uint8_t>(static_cast<std::int8_t>(p[1]) + 128);
                             p[2] = static_cast<std::uint8_t>(static_cast<std::int8_t>(p[2]) + 128); }
        } else if (d == pixel::Depth::U16) {
            auto* p = reinterpret_cast<std::uint16_t*>(row) + static_cast<std::size_t>(x) * static_cast<std::size_t>(n);
            if (signed_ab) { p[1] = static_cast<std::uint16_t>(static_cast<std::int16_t>(p[1]) + 32768);
                             p[2] = static_cast<std::uint16_t>(static_cast<std::int16_t>(p[2]) + 32768); }
        } else {
            auto* p = reinterpret_cast<float*>(row) + static_cast<std::size_t>(x) * static_cast<std::size_t>(n);
            p[0] /= 100.f;
            p[1] = (p[1] + 128.f) / 255.f;
            p[2] = (p[2] + 128.f) / 255.f;
        }
    }
}

std::optional<Raster> read_rgba(TIFF* t, int W, int H, Raster out, std::string& error, const Progress& p,
                                const char* why) {
    out.notes.push_back(std::string("read as 8-bit RGBA (") + why + ")");
    std::vector<std::uint32_t> all(static_cast<std::size_t>(W) * static_cast<std::size_t>(H));
    t_err[0] = 0;
    if (!TIFFReadRGBAImageOriented(t, static_cast<std::uint32_t>(W), static_cast<std::uint32_t>(H), all.data(),
                                   ORIENTATION_TOPLEFT, 0)) {
        error = std::string("not a readable TIFF: ") + t_err;
        return std::nullopt;
    }
    pixel::Format f{pixel::Depth::U8, pixel::Mode::RGB, true};
    BandBuilder band(f, W, H);
    for (int y = 0; y < H; ++y) {
        if (p.stop()) { error = "cancelled"; return std::nullopt; }
        std::uint8_t* d = band.row(y);
        for (int x = 0; x < W; ++x) {
            const std::uint32_t v = all[static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)];
            d[x * 4 + 0] = static_cast<std::uint8_t>(TIFFGetR(v));
            d[x * 4 + 1] = static_cast<std::uint8_t>(TIFFGetG(v));
            d[x * 4 + 2] = static_cast<std::uint8_t>(TIFFGetB(v));
            d[x * 4 + 3] = static_cast<std::uint8_t>(TIFFGetA(v));
        }
        unpremultiply_row(f, d, W);   // the RGBA reader hands back premultiplied
        band.done(y);
    }
    out.pixels = band.take();
    p.at(1);
    return out;
}
}  // namespace

std::optional<Raster> read_tiff(const std::string& path, std::string& error, const Progress& p) {
    quiet();
    t_err[0] = 0;
    Tif tif(path, "r");
    if (!tif.t) { error = std::string("not a readable TIFF") + (t_err[0] ? std::string(": ") + t_err : ""); return std::nullopt; }
    TIFF* t = tif.t;
    std::uint32_t w = 0, h = 0;
    std::uint16_t spp = 1, bps = 1, fmt = SAMPLEFORMAT_UINT, photo = PHOTOMETRIC_MINISBLACK,
                  planar = PLANARCONFIG_CONTIG, inkset = INKSET_CMYK;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &spp);
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bps);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLEFORMAT, &fmt);
    TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(t, TIFFTAG_INKSET, &inkset);
    if (!TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &photo)) photo = PHOTOMETRIC_MINISBLACK;
    if (w == 0 || h == 0 || w > static_cast<unsigned>(kMaxSide) || h > static_cast<unsigned>(kMaxSide) ||
        static_cast<std::uint64_t>(w) * h > kMaxPixels) {
        error = w == 0 || h == 0 ? "an empty TIFF" : "too large";
        return std::nullopt;
    }
    const int W = static_cast<int>(w), H = static_cast<int>(h);

    Raster out;
    {
        std::uint32_t len = 0;
        void* prof = nullptr;
        if (TIFFGetField(t, TIFFTAG_ICCPROFILE, &len, &prof) && prof && len)
            out.icc.assign(static_cast<std::uint8_t*>(prof), static_cast<std::uint8_t*>(prof) + len);
        float xr = 0;
        std::uint16_t unit = RESUNIT_INCH;
        TIFFGetFieldDefaulted(t, TIFFTAG_RESOLUTIONUNIT, &unit);
        if (TIFFGetField(t, TIFFTAG_XRESOLUTION, &xr) && xr > 0)
            out.dpi = unit == RESUNIT_CENTIMETER ? xr * 2.54 : xr;
        if (TIFFNumberOfDirectories(t) > 1)
            out.notes.push_back("first page of " + std::to_string(TIFFNumberOfDirectories(t)));
    }

    pixel::Format f;
    int colours = 0;
    bool signed_lab = false, invert = false;
    switch (photo) {
        case PHOTOMETRIC_MINISBLACK: f.mode = pixel::Mode::Gray; colours = 1; break;
        case PHOTOMETRIC_MINISWHITE: f.mode = pixel::Mode::Gray; colours = 1; invert = true; break;
        case PHOTOMETRIC_RGB:        f.mode = pixel::Mode::RGB;  colours = 3; break;
        case PHOTOMETRIC_SEPARATED:  f.mode = pixel::Mode::CMYK; colours = 4; break;
        case PHOTOMETRIC_CIELAB:     f.mode = pixel::Mode::Lab;  colours = 3; signed_lab = true; break;
        case PHOTOMETRIC_ICCLAB:     f.mode = pixel::Mode::Lab;  colours = 3; break;
        default: return read_rgba(t, W, H, std::move(out), error, p, "its colour layout");
    }
    if (photo == PHOTOMETRIC_SEPARATED && (inkset != INKSET_CMYK || spp < 4))
        return read_rgba(t, W, H, std::move(out), error, p, "not a CMYK ink set");
    if (TIFFIsTiled(t)) return read_rgba(t, W, H, std::move(out), error, p, "tiled");
    if (planar != PLANARCONFIG_CONTIG) return read_rgba(t, W, H, std::move(out), error, p, "planar");
    if (fmt == SAMPLEFORMAT_IEEEFP && bps == 32) f.depth = pixel::Depth::F32;
    else if (fmt == SAMPLEFORMAT_UINT && bps == 16) f.depth = pixel::Depth::U16;
    else if (fmt == SAMPLEFORMAT_UINT && bps == 8) f.depth = pixel::Depth::U8;
    else if (signed_lab && fmt == SAMPLEFORMAT_INT && (bps == 8 || bps == 16))
        f.depth = bps == 8 ? pixel::Depth::U8 : pixel::Depth::U16;
    else return read_rgba(t, W, H, std::move(out), error, p, "its sample depth");
    if (spp < colours) { error = "too few samples per pixel"; return std::nullopt; }

    // Extra samples: the first, if named alpha, is the alpha; the rest drop.
    bool assoc = false;
    f.alpha = false;
    if (spp > colours) {
        std::uint16_t nextra = 0;
        std::uint16_t* kinds = nullptr;
        if (TIFFGetField(t, TIFFTAG_EXTRASAMPLES, &nextra, &kinds) && nextra > 0 && kinds) {
            if (kinds[0] == EXTRASAMPLE_ASSOCALPHA || kinds[0] == EXTRASAMPLE_UNASSALPHA) {
                f.alpha = true;
                assoc = kinds[0] == EXTRASAMPLE_ASSOCALPHA;
            }
        }
        const int dropped = spp - colours - (f.alpha ? 1 : 0);
        if (dropped > 0) out.notes.push_back(std::to_string(dropped) + " extra channel(s) dropped");
    }

    const auto bytes = static_cast<std::size_t>(f.bytes_per_sample());
    const std::size_t scan = static_cast<std::size_t>(TIFFScanlineSize(t));
    if (scan < static_cast<std::size_t>(W) * spp * bytes) { error = "a damaged TIFF (short scanlines)"; return std::nullopt; }
    std::vector<std::uint8_t> line(scan);
    BandBuilder band(f, W, H);
    const int keep = f.channels();
    for (int y = 0; y < H; ++y) {
        if (p.stop()) { error = "cancelled"; return std::nullopt; }
        t_err[0] = 0;
        if (TIFFReadScanline(t, line.data(), static_cast<std::uint32_t>(y), 0) < 0) {
            error = std::string("a damaged TIFF: ") + t_err;
            return std::nullopt;
        }
        std::uint8_t* d = band.row(y);
        if (keep == spp) std::memcpy(d, line.data(), band.row_bytes());
        else
            for (int x = 0; x < W; ++x)
                std::memcpy(d + static_cast<std::size_t>(x) * static_cast<std::size_t>(keep) * bytes,
                            line.data() + static_cast<std::size_t>(x) * spp * bytes, static_cast<std::size_t>(keep) * bytes);
        if (invert) {
            const std::size_t n = static_cast<std::size_t>(W) * static_cast<std::size_t>(keep);
            for (std::size_t i = 0; i < n; ++i) {
                if (f.alpha && i % static_cast<std::size_t>(keep) == static_cast<std::size_t>(keep - 1)) continue;
                if (f.depth == pixel::Depth::U8) d[i] = static_cast<std::uint8_t>(255 - d[i]);
                else if (f.depth == pixel::Depth::U16) { auto* s = reinterpret_cast<std::uint16_t*>(d); s[i] = static_cast<std::uint16_t>(65535 - s[i]); }
                else { auto* s = reinterpret_cast<float*>(d); s[i] = 1.f - s[i]; }
            }
        }
        if (f.mode == pixel::Mode::Lab) lab_in(d, W, keep, f.depth, signed_lab);
        if (assoc) unpremultiply_row(f, d, W);
        band.done(y);
        if ((y & 63) == 0) p.at(static_cast<double>(y) / H);
    }
    out.pixels = band.take();
    p.at(1);
    return out;
}

bool write_tiff(const std::string& path, const Raster& r, std::string& error, const Progress& p,
                const WriteOptions& o) {
    quiet();
    const auto& src = r.pixels;
    const pixel::Format f = src.format();
    if (src.width() <= 0 || src.height() <= 0) { error = "an empty image"; return false; }
    t_err[0] = 0;
    bool ok = true, cancelled = false;
    {
        Tif tif(path, src.bytes() > (std::size_t{3} << 30) ? "w8" : "w");   // BigTIFF past ~3 GB
        if (!tif.t) { error = "can't be written"; return false; }
        TIFF* t = tif.t;
        const int colours = f.colour_channels();
        TIFFSetField(t, TIFFTAG_IMAGEWIDTH, static_cast<std::uint32_t>(src.width()));
        TIFFSetField(t, TIFFTAG_IMAGELENGTH, static_cast<std::uint32_t>(src.height()));
        TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, static_cast<std::uint16_t>(f.channels()));
        TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, static_cast<std::uint16_t>(f.bytes_per_sample() * 8));
        TIFFSetField(t, TIFFTAG_SAMPLEFORMAT, static_cast<std::uint16_t>(f.depth == pixel::Depth::F32 ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT));
        TIFFSetField(t, TIFFTAG_PLANARCONFIG, static_cast<std::uint16_t>(PLANARCONFIG_CONTIG));
        TIFFSetField(t, TIFFTAG_ORIENTATION, static_cast<std::uint16_t>(ORIENTATION_TOPLEFT));
        std::uint16_t photo = PHOTOMETRIC_RGB;
        switch (f.mode) {
            case pixel::Mode::Gray: photo = PHOTOMETRIC_MINISBLACK; break;
            case pixel::Mode::RGB:  photo = PHOTOMETRIC_RGB; break;
            case pixel::Mode::CMYK: photo = PHOTOMETRIC_SEPARATED; TIFFSetField(t, TIFFTAG_INKSET, static_cast<std::uint16_t>(INKSET_CMYK)); break;
            // 8 / 16: ICCLab (unsigned a / b, Klimt's own encoding); float: CIELab in real units.
            case pixel::Mode::Lab:  photo = f.depth == pixel::Depth::F32 ? PHOTOMETRIC_CIELAB : PHOTOMETRIC_ICCLAB; break;
        }
        TIFFSetField(t, TIFFTAG_PHOTOMETRIC, photo);
        if (f.alpha) {
            const std::uint16_t extra[] = {EXTRASAMPLE_UNASSALPHA};
            TIFFSetField(t, TIFFTAG_EXTRASAMPLES, static_cast<std::uint16_t>(1), extra);
        }
        TIFFSetField(t, TIFFTAG_COMPRESSION, static_cast<std::uint16_t>(o.tiff_deflate ? COMPRESSION_ADOBE_DEFLATE : COMPRESSION_NONE));
        if (o.tiff_deflate)
            TIFFSetField(t, TIFFTAG_PREDICTOR, static_cast<std::uint16_t>(f.depth == pixel::Depth::F32 ? PREDICTOR_FLOATINGPOINT : PREDICTOR_HORIZONTAL));
        TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(t, 0));
        if (r.dpi > 0) {
            TIFFSetField(t, TIFFTAG_XRESOLUTION, static_cast<float>(r.dpi));
            TIFFSetField(t, TIFFTAG_YRESOLUTION, static_cast<float>(r.dpi));
            TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT, static_cast<std::uint16_t>(RESUNIT_INCH));
        }
        if (!r.icc.empty()) TIFFSetField(t, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(r.icc.size()), r.icc.data());
        TIFFSetField(t, TIFFTAG_SOFTWARE, "Klimt");
        (void)colours;
        std::vector<std::uint8_t> row(static_cast<std::size_t>(src.width()) * static_cast<std::size_t>(f.bytes_per_pixel()));
        for (int y = 0; y < src.height(); ++y) {
            if (p.stop()) { cancelled = true; break; }
            src.row_bytes(y, row.data());
            if (f.mode == pixel::Mode::Lab && f.depth == pixel::Depth::F32) {
                // Float Lab is stored in its real units (L 0..100, a / b signed).
                auto* s = reinterpret_cast<float*>(row.data());
                const int n = f.channels();
                for (int x = 0; x < src.width(); ++x) {
                    float* q = s + static_cast<std::size_t>(x) * static_cast<std::size_t>(n);
                    q[0] *= 100.f; q[1] = q[1] * 255.f - 128.f; q[2] = q[2] * 255.f - 128.f;
                }
            }
            if (TIFFWriteScanline(t, row.data(), static_cast<std::uint32_t>(y), 0) < 0) { ok = false; break; }
            if ((y & 63) == 0) p.at(static_cast<double>(y) / src.height());
        }
    }
    if (cancelled || !ok) {
        std::remove(path.c_str());
        error = cancelled ? "cancelled" : std::string("TIFF: ") + t_err;
        return false;
    }
    p.at(1);
    return true;
}

}  // namespace klimt::formats
