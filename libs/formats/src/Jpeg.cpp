// JPEG in and out, on libjpeg(-turbo) (s007). Gray, RGB (from YCbCr) and
// CMYK (and YCCK) at 8 bits; never alpha. Photoshop's CMYK JPEGs carry the
// Adobe marker and store INVERTED inks; Klimt stores ink amounts (1 = full
// ink), so they flip on the way in and back on the way out.
#include "formats/Formats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <jpeglib.h>

namespace klimt::formats {

namespace {
struct File {
    std::FILE* f = nullptr;
    File(const std::string& path, const char* mode) : f(std::fopen(path.c_str(), mode)) {}
    ~File() { if (f) std::fclose(f); }
};
struct Err {
    jpeg_error_mgr mgr;
    std::jmp_buf   jump;
    char           msg[JMSG_LENGTH_MAX] = {};
};
void on_error(j_common_ptr c) {
    auto* e = reinterpret_cast<Err*>(c->err);
    (*c->err->format_message)(c, e->msg);
    std::longjmp(e->jump, 1);
}
void on_message(j_common_ptr, int) {}   // warnings stay quiet
}  // namespace

std::optional<Raster> read_jpeg(const std::string& path, std::string& error, const Progress& p) {
    File file(path, "rb");
    if (!file.f) { error = "can't be opened"; return std::nullopt; }
    jpeg_decompress_struct cinfo{};
    Err err{};
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = on_error;
    err.mgr.emit_message = on_message;
    std::unique_ptr<BandBuilder> band;
    std::vector<std::uint8_t> scan;
    Raster out;
    bool cancelled = false;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        error = std::string("not a readable JPEG: ") + err.msg;
        return std::nullopt;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, file.f);
    jpeg_save_markers(&cinfo, JPEG_APP0 + 2, 0xFFFF);   // ICC
    jpeg_read_header(&cinfo, TRUE);
    if (cinfo.image_width > static_cast<unsigned>(kMaxSide) || cinfo.image_height > static_cast<unsigned>(kMaxSide) ||
        static_cast<std::uint64_t>(cinfo.image_width) * cinfo.image_height > kMaxPixels) {
        jpeg_destroy_decompress(&cinfo);
        error = "too large";
        return std::nullopt;
    }
    pixel::Format f;
    f.depth = pixel::Depth::U8;
    f.alpha = false;
    bool invert = false;
    switch (cinfo.jpeg_color_space) {
        case JCS_GRAYSCALE: f.mode = pixel::Mode::Gray; cinfo.out_color_space = JCS_GRAYSCALE; break;
        case JCS_CMYK:
        case JCS_YCCK:
            f.mode = pixel::Mode::CMYK;
            cinfo.out_color_space = JCS_CMYK;
            invert = cinfo.saw_Adobe_marker;
            break;
        default: f.mode = pixel::Mode::RGB; cinfo.out_color_space = JCS_RGB; break;
    }
    JOCTET* icc = nullptr;
    unsigned icc_len = 0;
    if (jpeg_read_icc_profile(&cinfo, &icc, &icc_len) && icc) {
        out.icc.assign(icc, icc + icc_len);
        std::free(icc);
    }
    if (cinfo.saw_JFIF_marker && cinfo.X_density > 0) {
        if (cinfo.density_unit == 1) out.dpi = cinfo.X_density;
        else if (cinfo.density_unit == 2) out.dpi = cinfo.X_density * 2.54;
    }
    jpeg_start_decompress(&cinfo);
    if (static_cast<int>(cinfo.output_components) != f.channels()) {
        jpeg_destroy_decompress(&cinfo);
        error = "an unexpected JPEG layout";
        return std::nullopt;
    }
    const int W = static_cast<int>(cinfo.output_width), H = static_cast<int>(cinfo.output_height);
    band = std::make_unique<BandBuilder>(f, W, H);
    while (cinfo.output_scanline < cinfo.output_height) {
        if (p.stop()) { cancelled = true; break; }
        const int y = static_cast<int>(cinfo.output_scanline);
        JSAMPROW row = band->row(y);
        jpeg_read_scanlines(&cinfo, &row, 1);
        if (invert)
            for (std::size_t i = 0; i < band->row_bytes(); ++i) row[i] = static_cast<std::uint8_t>(255 - row[i]);
        band->done(y);
        if ((y & 63) == 0) p.at(static_cast<double>(y) / H);
    }
    if (cancelled) jpeg_abort_decompress(&cinfo);
    else jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    if (cancelled) { error = "cancelled"; return std::nullopt; }
    out.pixels = band->take();
    p.at(1);
    return out;
}

bool write_jpeg(const std::string& path, const Raster& r, std::string& error, const Progress& p,
                const WriteOptions& o) {
    const auto& src = r.pixels;
    const pixel::Format sf = src.format();
    if (sf.mode == pixel::Mode::Lab) { error = "JPEG holds Gray, RGB and CMYK only"; return false; }
    if (src.width() <= 0 || src.height() <= 0) { error = "an empty image"; return false; }
    File file(path, "wb");
    if (!file.f) { error = "can't be written"; return false; }
    jpeg_compress_struct cinfo{};
    Err err{};
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = on_error;
    err.mgr.emit_message = on_message;
    const int colours = sf.colour_channels(), n = sf.channels();
    std::vector<std::uint8_t> in(static_cast<std::size_t>(src.width()) * static_cast<std::size_t>(sf.bytes_per_pixel()));
    std::vector<std::uint8_t> row(static_cast<std::size_t>(src.width()) * static_cast<std::size_t>(colours));
    bool cancelled = false;
    if (setjmp(err.jump)) {
        jpeg_destroy_compress(&cinfo);
        error = std::string("JPEG: ") + err.msg;
        std::remove(path.c_str());
        return false;
    }
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, file.f);
    cinfo.image_width = static_cast<JDIMENSION>(src.width());
    cinfo.image_height = static_cast<JDIMENSION>(src.height());
    cinfo.input_components = colours;
    cinfo.in_color_space = sf.mode == pixel::Mode::Gray ? JCS_GRAYSCALE : sf.mode == pixel::Mode::CMYK ? JCS_CMYK : JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, std::clamp(o.jpeg_quality, 1, 100), TRUE);
    if (sf.mode == pixel::Mode::CMYK) cinfo.write_Adobe_marker = TRUE;
    if (r.dpi > 0) {
        cinfo.density_unit = 1;
        cinfo.X_density = cinfo.Y_density = static_cast<UINT16>(std::lround(std::min(r.dpi, 65535.0)));
    }
    jpeg_start_compress(&cinfo, TRUE);
    if (!r.icc.empty()) jpeg_write_icc_profile(&cinfo, r.icc.data(), static_cast<unsigned>(r.icc.size()));
    const bool invert = sf.mode == pixel::Mode::CMYK;
    for (int y = 0; y < src.height(); ++y) {
        if (p.stop()) { cancelled = true; break; }
        src.row_bytes(y, in.data());
        for (int x = 0; x < src.width(); ++x)
            for (int k = 0; k < colours; ++k) {
                const std::size_t si = static_cast<std::size_t>(x) * static_cast<std::size_t>(n) + static_cast<std::size_t>(k);
                float v = 0;
                switch (sf.depth) {
                    case pixel::Depth::U8:  v = in[si] / 255.f; break;
                    case pixel::Depth::U16: v = reinterpret_cast<const std::uint16_t*>(in.data())[si] / 65535.f; break;
                    case pixel::Depth::F32: v = reinterpret_cast<const float*>(in.data())[si]; break;
                }
                auto b = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
                row[static_cast<std::size_t>(x) * static_cast<std::size_t>(colours) + static_cast<std::size_t>(k)] =
                    invert ? static_cast<std::uint8_t>(255 - b) : b;
            }
        JSAMPROW rp = row.data();
        jpeg_write_scanlines(&cinfo, &rp, 1);
        if ((y & 63) == 0) p.at(static_cast<double>(y) / src.height());
    }
    if (cancelled) {
        jpeg_abort_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);
        error = "cancelled";
        std::remove(path.c_str());
        return false;
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    p.at(1);
    return true;
}

}  // namespace klimt::formats
