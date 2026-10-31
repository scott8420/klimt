#pragma once
// libs/formats -- image files in and out (s007). GTK-free; on libpng,
// libjpeg(-turbo) and libtiff directly (Scott, s006 -> s007, question 2: the
// 16-bit, CMYK and ICC cases need them -- GdkPixbuf is 8-bit RGB only).
//
// A file becomes ONE tile map at its own depth and mode (docs/pixels.md 1, 4):
// an 8-bit PNG stays 8-bit, a 16-bit TIFF stays 16-bit, a CMYK JPEG stays
// CMYK. Samples are stored STRAIGHT (call 3), so premultiplied ("associated")
// TIFF alpha is divided out on the way in. Rows are cut into tiles a band of
// 256 rows at a time, so a reader never holds the image twice.
//
// Readers and writers take a Progress: `cancelled` is asked between rows and
// a cancel returns failure with error "cancelled" (a cancel leaves nothing);
// `progress` hears 0..1. Errors are words for a toast, never a throw.
#include "pixel/Pixel.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace klimt::formats {

enum class Kind { Unknown, PNG, JPEG, TIFF, Klimt, WebP, ORA, PSD };   // s049: WebP, OpenRaster; s050: Photoshop
// By the file's first bytes, not its name (a .png that is a JPEG opens).
Kind sniff(const std::string& path);
// "PNG", "JPEG", "TIFF", "Klimt", "WebP", "OpenRaster", "" -- for messages.
const char* kind_name(Kind k);

struct Progress {
    std::function<bool()>       cancelled;
    std::function<void(double)> progress;
    bool stop() const { return cancelled && cancelled(); }
    void at(double f) const { if (progress) progress(f); }
};

// What a file holds: its pixels (width, height and format inside), its
// resolution and its colour profile (kept, not yet used -- libs/tint).
struct Raster {
    pixel::TileMap            pixels;
    double                    dpi = 72;
    std::vector<std::uint8_t> icc;
    std::vector<std::string>  notes;   // "first page of 3", "palette expanded"
};

// The guard on what a reader will allocate: no side over this, and no more
// than kMaxPixels in all (a corrupt header must not ask for 400 GB).
constexpr int           kMaxSide   = 300000;
constexpr std::uint64_t kMaxPixels = 2'000'000'000ull;

std::optional<Raster> read_image(const std::string& path, std::string& error, const Progress& p = {});
std::optional<Raster> read_png (const std::string& path, std::string& error, const Progress& p = {});
std::optional<Raster> read_jpeg(const std::string& path, std::string& error, const Progress& p = {});
std::optional<Raster> read_tiff(const std::string& path, std::string& error, const Progress& p = {});
// s049: WebP, lossy or lossless, 8-bit RGB(A); an animation's first frame.
std::optional<Raster> read_webp(const std::string& path, std::string& error, const Progress& p = {});
// s049: a PNG held in memory (an entry of a zip: OpenRaster's layers).
std::optional<Raster> read_png_bytes(const std::uint8_t* data, std::size_t n, std::string& error,
                                     const Progress& p = {});

// Writers take the raster's own format where the file can hold it, and say
// what they changed in `error` when they can't: PNG is Gray / RGB at 8 or 16
// (32f is written 16); JPEG is Gray / RGB / CMYK at 8 with no alpha (alpha is
// dropped -- flatten first); TIFF holds every mode and depth Klimt has.
struct WriteOptions {
    int  jpeg_quality = 92;          // 1..100
    bool tiff_deflate = true;        // else uncompressed
    int  webp_quality = 90;          // s049: 0..100, lossy
    bool webp_lossless = false;      // s049: exact pixels (quality then sets the effort)
    int  png_level = 6;              // s050: zlib 0..9 (lossless at every level)
    bool operator==(const WriteOptions&) const = default;
};
bool write_png (const std::string& path, const Raster& r, std::string& error, const Progress& p = {},
                const WriteOptions& o = {});
bool write_jpeg(const std::string& path, const Raster& r, std::string& error, const Progress& p = {},
                const WriteOptions& o = {});
bool write_tiff(const std::string& path, const Raster& r, std::string& error, const Progress& p = {},
                const WriteOptions& o = {});
// s049: WebP is 8-bit Gray / RGB with or without alpha (Gray is written as
// RGB; 16 / 32f are rounded to 8). The profile rides in an ICCP chunk.
bool write_webp(const std::string& path, const Raster& r, std::string& error, const Progress& p = {},
                const WriteOptions& o = {});
// s049: a PNG into memory (zlib `level` 0..9).
bool write_png_bytes(const Raster& r, std::vector<std::uint8_t>& out, std::string& error,
                     const Progress& p = {}, int level = 6);

// ── for readers: rows in, tiles out ─────────────────────────────────────────
// Feed every row top to bottom, as STORED bytes of the format (native
// endian). Each 256th row cuts a band into tiles; take() hands the map back.
class BandBuilder {
public:
    BandBuilder(const pixel::Format& f, int width, int height);
    std::uint8_t* row(int y);        // the buffer for row y (rows in order)
    void          done(int y);       // row y is written
    pixel::TileMap take();           // after the last row
    std::size_t   row_bytes() const { return m_row; }

private:
    void cut();
    pixel::Format                       m_f;
    int                                 m_w, m_h, m_band0 = 0, m_filled = 0;
    std::size_t                         m_row;
    std::vector<std::uint8_t>           m_band;
    std::vector<pixel::TileMap::TileRef> m_grid;
};

// Straight <-> premultiplied for a stored row (TIFF associated alpha).
void unpremultiply_row(const pixel::Format& f, std::uint8_t* row, int width);

}  // namespace klimt::formats
