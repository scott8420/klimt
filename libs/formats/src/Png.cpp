// PNG in and out, on libpng (s007). Every PNG becomes Gray or RGB at 8 or
// 16 bits, with alpha when it has some (an alpha channel, or tRNS); palettes
// and low bit depths expand to 8. 16-bit samples land host-endian.
#include "formats/Formats.hpp"

#include <png.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <memory>

namespace klimt::formats {

namespace {
struct File {
    std::FILE* f = nullptr;
    explicit File(const std::string& path, const char* mode) : f(std::fopen(path.c_str(), mode)) {}
    ~File() { if (f) std::fclose(f); }
};
struct PngError { char msg[200] = {}; };
void on_error(png_structp png, png_const_charp msg) {
    auto* e = static_cast<PngError*>(png_get_error_ptr(png));
    std::snprintf(e->msg, sizeof e->msg, "%s", msg);
    png_longjmp(png, 1);
}
void on_warning(png_structp, png_const_charp) {}

// s049: PNGs inside a zip (OpenRaster's layers, a .klimt's preview) are read
// from and written to memory.
struct MemIn { const std::uint8_t* d; std::size_t n, at = 0; };
void mem_read(png_structp png, png_bytep out, png_size_t len) {
    auto* m = static_cast<MemIn*>(png_get_io_ptr(png));
    if (len > m->n - m->at) png_error(png, "unexpected end of data");
    std::memcpy(out, m->d + m->at, len);
    m->at += len;
}
void mem_write(png_structp png, png_bytep in, png_size_t len) {
    auto* v = static_cast<std::vector<std::uint8_t>*>(png_get_io_ptr(png));
    v->insert(v->end(), in, in + len);
}
void mem_flush(png_structp) {}

std::optional<Raster> read_png_io(std::FILE* file, MemIn* mem, std::string& error, const Progress& p) {
    PngError err;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &err, on_error, on_warning);
    if (!png) { error = "out of memory"; return std::nullopt; }
    png_infop info = png_create_info_struct(png);
    // Everything that must outlive a longjmp lives above setjmp.
    std::unique_ptr<BandBuilder> band;
    Raster out;
    std::vector<std::uint8_t> all;      // interlaced only: the whole image
    std::vector<png_bytep> rows;
    bool cancelled = false;
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        error = std::string("not a readable PNG: ") + err.msg;
        return std::nullopt;
    }
    if (mem) png_set_read_fn(png, mem, mem_read);
    else png_init_io(png, file);
    png_set_user_limits(png, kMaxSide, kMaxSide);
    png_read_info(png, info);

    png_uint_32 w = 0, h = 0;
    int bit_depth = 0, colour = 0, interlace = 0;
    png_get_IHDR(png, info, &w, &h, &bit_depth, &colour, &interlace, nullptr, nullptr);
    if (static_cast<std::uint64_t>(w) * h > kMaxPixels) {
        png_destroy_read_struct(&png, &info, nullptr);
        error = "too large (" + std::to_string(w) + " x " + std::to_string(h) + ")";
        return std::nullopt;
    }
    if (colour == PNG_COLOR_TYPE_PALETTE) { png_set_palette_to_rgb(png); out.notes.push_back("palette expanded to RGB"); }
    if (colour == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (bit_depth == 16 && std::endian::native == std::endian::little) png_set_swap(png);
    const int passes = interlace == PNG_INTERLACE_NONE ? 1 : png_set_interlace_handling(png);
    png_read_update_info(png, info);

    const int channels = png_get_channels(png, info);
    const int depth = png_get_bit_depth(png, info);
    pixel::Format f;
    f.depth = depth == 16 ? pixel::Depth::U16 : pixel::Depth::U8;
    f.mode  = channels <= 2 ? pixel::Mode::Gray : pixel::Mode::RGB;
    f.alpha = channels == 2 || channels == 4;
    if (f.channels() != channels) {
        png_destroy_read_struct(&png, &info, nullptr);
        error = "an unexpected PNG layout";
        return std::nullopt;
    }

    png_uint_32 rx = 0, ry = 0;
    int unit = 0;
    if (png_get_pHYs(png, info, &rx, &ry, &unit) && unit == PNG_RESOLUTION_METER && rx > 0)
        out.dpi = std::round(rx * 0.0254 * 100.0) / 100.0;
    png_charp name = nullptr;
    png_bytep prof = nullptr;
    png_uint_32 plen = 0;
    int comp = 0;
    if (png_get_iCCP(png, info, &name, &comp, &prof, &plen) && prof && plen)
        out.icc.assign(prof, prof + plen);

    const int W = static_cast<int>(w), H = static_cast<int>(h);
    if (passes == 1) {
        band = std::make_unique<BandBuilder>(f, W, H);
        for (int y = 0; y < H; ++y) {
            if (p.stop()) { cancelled = true; break; }
            png_read_row(png, band->row(y), nullptr);
            band->done(y);
            if ((y & 63) == 0) p.at(static_cast<double>(y) / H);
        }
    } else {
        // Interlaced: libpng needs the whole image to de-interlace.
        out.notes.push_back("interlaced");
        const std::size_t rb = static_cast<std::size_t>(W) * static_cast<std::size_t>(f.bytes_per_pixel());
        all.resize(rb * static_cast<std::size_t>(H));
        rows.resize(static_cast<std::size_t>(H));
        for (int y = 0; y < H; ++y) rows[static_cast<std::size_t>(y)] = all.data() + rb * static_cast<std::size_t>(y);
        png_read_image(png, rows.data());
        band = std::make_unique<BandBuilder>(f, W, H);
        for (int y = 0; y < H; ++y) {
            std::memcpy(band->row(y), rows[static_cast<std::size_t>(y)], rb);
            band->done(y);
        }
    }
    png_destroy_read_struct(&png, &info, nullptr);
    if (cancelled) { error = "cancelled"; return std::nullopt; }
    out.pixels = band->take();
    p.at(1);
    return out;
}

bool write_png_io(std::FILE* file, std::vector<std::uint8_t>* mem, const std::string& path, const Raster& r,
                  std::string& error, const Progress& p, int level) {
    const auto& src = r.pixels;
    const pixel::Format sf = src.format();
    if (sf.mode != pixel::Mode::Gray && sf.mode != pixel::Mode::RGB) {
        error = "PNG holds Gray and RGB only";
        return false;
    }
    if (src.width() <= 0 || src.height() <= 0) { error = "an empty image"; return false; }
    pixel::Format f = sf;
    if (f.depth == pixel::Depth::F32) f.depth = pixel::Depth::U16;   // PNG has no float
    PngError err;
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &err, on_error, on_warning);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!info) { if (png) png_destroy_write_struct(&png, nullptr); error = "out of memory"; return false; }
    std::vector<std::uint8_t> in(static_cast<std::size_t>(src.width()) * static_cast<std::size_t>(sf.bytes_per_pixel()));
    std::vector<std::uint8_t> row(static_cast<std::size_t>(src.width()) * static_cast<std::size_t>(f.bytes_per_pixel()));
    bool cancelled = false;
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        error = std::string("PNG: ") + err.msg;
        if (!mem) std::remove(path.c_str());
        return false;
    }
    if (mem) png_set_write_fn(png, mem, mem_write, mem_flush);
    else png_init_io(png, file);
    // A profile libpng won't accept (wrong colour space, damaged) is dropped
    // with a warning rather than failing the whole file.
    png_set_benign_errors(png, 1);
    const int type = f.mode == pixel::Mode::Gray ? (f.alpha ? PNG_COLOR_TYPE_GRAY_ALPHA : PNG_COLOR_TYPE_GRAY)
                                                 : (f.alpha ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB);
    png_set_IHDR(png, info, static_cast<png_uint_32>(src.width()), static_cast<png_uint_32>(src.height()),
                 f.depth == pixel::Depth::U16 ? 16 : 8, type, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, level);
    if (r.dpi > 0) {
        const auto ppm = static_cast<png_uint_32>(std::lround(r.dpi / 0.0254));
        png_set_pHYs(png, info, ppm, ppm, PNG_RESOLUTION_METER);
    }
    if (!r.icc.empty())
        png_set_iCCP(png, info, "ICC", 0, r.icc.data(), static_cast<png_uint_32>(r.icc.size()));
    png_write_info(png, info);
    if (f.depth == pixel::Depth::U16 && std::endian::native == std::endian::little) png_set_swap(png);
    const int n = src.width() * f.channels();
    for (int y = 0; y < src.height(); ++y) {
        if (p.stop()) { cancelled = true; break; }
        src.row_bytes(y, in.data());
        if (sf.depth == pixel::Depth::F32) {
            const auto* s = reinterpret_cast<const float*>(in.data());
            auto* d = reinterpret_cast<std::uint16_t*>(row.data());
            for (int i = 0; i < n; ++i)
                d[i] = static_cast<std::uint16_t>(std::lround(std::clamp(s[i], 0.f, 1.f) * 65535.f));
            png_write_row(png, row.data());
        } else {
            png_write_row(png, in.data());
        }
        if ((y & 63) == 0) p.at(static_cast<double>(y) / src.height());
    }
    if (cancelled) {
        png_destroy_write_struct(&png, &info);
        error = "cancelled";
        if (!mem) std::remove(path.c_str());
        return false;
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    p.at(1);
    return true;
}
}  // namespace

std::optional<Raster> read_png(const std::string& path, std::string& error, const Progress& p) {
    File file(path, "rb");
    if (!file.f) { error = "can't be opened"; return std::nullopt; }
    return read_png_io(file.f, nullptr, error, p);
}

std::optional<Raster> read_png_bytes(const std::uint8_t* data, std::size_t n, std::string& error, const Progress& p) {
    if (n < 8 || std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) { error = "not a PNG"; return std::nullopt; }
    MemIn m{data, n};
    return read_png_io(nullptr, &m, error, p);
}

bool write_png(const std::string& path, const Raster& r, std::string& error, const Progress& p) {
    if (r.pixels.width() <= 0 || r.pixels.height() <= 0) { error = "an empty image"; return false; }
    File file(path, "wb");
    if (!file.f) { error = "can't be written"; return false; }
    return write_png_io(file.f, nullptr, path, r, error, p, 6);
}

bool write_png_bytes(const Raster& r, std::vector<std::uint8_t>& out, std::string& error, const Progress& p, int level) {
    out.clear();
    return write_png_io(nullptr, &out, {}, r, error, p, level);
}

}  // namespace klimt::formats
