// klimt_selftest -- s098: HEIC / HEIF read (libs/formats/src/Heif.cpp). The
// test writes its own HEICs with libheif's encoder (x265) -- or AVIFs (aom)
// when there's no HEVC encoder -- so no iPhone file is needed. Sniffing is
// checked always; without libheif only the refusal; without an encoder the
// reading checks are skipped (said, not failed).
#include "core/File.hpp"
#include "core/RoundTrip.hpp"
#include "formats/Formats.hpp"
#include "pixel/Pixel.hpp"

#ifdef KLIMT_HAVE_HEIF
#include <libheif/heif.h>
#endif

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

namespace Fm = klimt::formats;
namespace core = klimt::core;
namespace fs = std::filesystem;

using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
fs::path scratch() {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_heif_" + std::to_string(::getpid()));
        fs::create_directories(p);
        return p;
    }();
    return d;
}

// A file of just these bytes (an ftyp box and some padding).
std::string bytes_file(const std::string& name, const std::vector<std::uint8_t>& b) {
    const auto p = (scratch() / name).string();
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return p;
}
std::vector<std::uint8_t> ftyp(const char* major, std::initializer_list<const char*> compat) {
    std::vector<std::uint8_t> b;
    const std::uint32_t n = 16 + 4 * static_cast<std::uint32_t>(compat.size());
    b.push_back(static_cast<std::uint8_t>(n >> 24)); b.push_back(static_cast<std::uint8_t>(n >> 16));
    b.push_back(static_cast<std::uint8_t>(n >> 8));  b.push_back(static_cast<std::uint8_t>(n));
    for (const char* s : {"ftyp", major}) b.insert(b.end(), s, s + 4);
    b.insert(b.end(), 4, 0);
    for (const char* s : compat) b.insert(b.end(), s, s + 4);
    b.insert(b.end(), 32, 0);
    return b;
}

#ifdef KLIMT_HAVE_HEIF
struct Enc { heif_compression_format fmt; const char* ext; };

// W x H: red rising left to right, green top to bottom, blue a constant; alpha
// (if any) a ramp. `bits` 8 or 10. Lossless where the encoder can (the
// check is a tolerance either way).
bool make(const std::string& path, Enc e, int W, int H, bool alpha, int bits, std::string& why) {
    heif_context* ctx = heif_context_alloc();
    heif_encoder* enc = nullptr;
    heif_error r = heif_context_get_encoder_for_format(ctx, e.fmt, &enc);
    if (r.code != heif_error_Ok) { heif_context_free(ctx); why = "no encoder"; return false; }
    heif_encoder_set_lossy_quality(enc, 100);
    heif_encoder_set_lossless(enc, 1);
    heif_image* img = nullptr;
    const bool deep = bits > 8;
    const heif_chroma ch = deep ? (alpha ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBB_LE)
                                : (alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB);
    heif_image_create(W, H, heif_colorspace_RGB, ch, &img);
    heif_image_add_plane(img, heif_channel_interleaved, W, H, bits);
    int stride = 0;
    std::uint8_t* px = heif_image_get_plane(img, heif_channel_interleaved, &stride);
    const int n = alpha ? 4 : 3;
    const int top = (1 << bits) - 1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int v[4] = {x * top / (W - 1), y * top / (H - 1), top / 4, (x + y) * top / (W + H - 2)};
            for (int k = 0; k < n; ++k) {
                if (deep) {
                    auto* d = px + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x * n + k) * 2;
                    d[0] = static_cast<std::uint8_t>(v[k] & 0xFF); d[1] = static_cast<std::uint8_t>(v[k] >> 8);
                } else {
                    px[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x * n + k)] = static_cast<std::uint8_t>(v[k]);
                }
            }
        }
    heif_image_handle* h = nullptr;
    r = heif_context_encode_image(ctx, img, enc, nullptr, &h);
    bool ok = r.code == heif_error_Ok;
    if (!ok) why = r.message ? r.message : "encode failed";
    if (ok) {
        r = heif_context_write_to_file(ctx, path.c_str());
        ok = r.code == heif_error_Ok;
        if (!ok) why = r.message ? r.message : "write failed";
    }
    if (h) heif_image_handle_release(h);
    heif_image_release(img);
    heif_encoder_release(enc);
    heif_context_free(ctx);
    return ok;
}

// Pixel (x, y) channel k as 0..1.
double at(const klimt::pixel::TileMap& m, int x, int y, int k) {
    return static_cast<double>(m.get(x, y).c[static_cast<std::size_t>(k)]);
}
#endif
}  // namespace

void selftest_heif(const Check& check) {
    // Sniffed by the ftyp box, not the name.
    check("heif: an iPhone's ftyp (heic) sniffs HEIF",
          Fm::sniff(bytes_file("a.bin", ftyp("heic", {"mif1", "heic"}))) == Fm::Kind::HEIF, "");
    check("heif: a mif1 compatible brand sniffs HEIF",
          Fm::sniff(bytes_file("b.bin", ftyp("xxxx", {"mif1"}))) == Fm::Kind::HEIF, "");
    check("heif: an AVIF sniffs HEIF", Fm::sniff(bytes_file("c.bin", ftyp("avif", {"mif1"}))) == Fm::Kind::HEIF, "");
    check("heif: an MP4 (isom / mp42) is not HEIF",
          Fm::sniff(bytes_file("d.mp4", ftyp("isom", {"isom", "mp42"}))) == Fm::Kind::Unknown, "");
    check("heif: its name in messages is HEIC", std::string(Fm::kind_name(Fm::Kind::HEIF)) == "HEIC", "");
    check("heif: read only -- Save doesn't write one back", core::roundtrip::writes_back(Fm::Kind::HEIF) == nullptr, "");
    check("heif: not among Export's formats", Fm::export_format(Fm::Kind::HEIF) == nullptr, "");

#ifndef KLIMT_HAVE_HEIF
    {
        std::string err;
        const auto r = Fm::read_image(bytes_file("e.heic", ftyp("heic", {"mif1"})), err);
        check("heif: without libheif, refused in words", !r && err.find("libheif") != std::string::npos, err);
        check("heif: heif_available() false", !Fm::heif_available(), "");
    }
#else
    check("heif: heif_available() true", Fm::heif_available() && !Fm::heif_version().empty(), Fm::heif_version());
    heif_init(nullptr);
    Enc e{heif_compression_HEVC, ".heic"};
    if (!heif_have_encoder_for_format(heif_compression_HEVC)) e = {heif_compression_AV1, ".avif"};
    const bool can = heif_have_encoder_for_format(e.fmt) && heif_have_decoder_for_format(e.fmt);
    if (!can) {
        check("heif: (no HEVC / AV1 encoder + decoder here -- reading checks skipped)", true, "");
    } else {
        const std::string tag = e.fmt == heif_compression_HEVC ? "HEIC" : "AVIF";
        std::string why;
        // 8-bit RGB
        const auto p8 = (scratch() / (std::string("rgb8") + e.ext)).string();
        if (!make(p8, e, 64, 40, false, 8, why)) {
            check("heif: wrote a test " + tag, false, why);
        } else {
            check("heif: the test " + tag + " sniffs HEIF", Fm::sniff(p8) == Fm::Kind::HEIF, "");
            std::string err;
            const auto r = Fm::read_image(p8, err);
            check("heif: an 8-bit " + tag + " reads", r.has_value(), err);
            if (r) {
                const auto f = r->pixels.format();
                check("heif: 64 x 40, 8-bit RGB, no alpha",
                      r->pixels.width() == 64 && r->pixels.height() == 40 && f.depth == klimt::pixel::Depth::U8 &&
                          f.mode == klimt::pixel::Mode::RGB && !f.alpha, "");
                // Lossy codecs: the ramps within a tolerance.
                const double tol = 0.06;
                const bool ramp = std::abs(at(r->pixels, 0, 20, 0) - 0.0) < tol && std::abs(at(r->pixels, 63, 20, 0) - 1.0) < tol &&
                                  std::abs(at(r->pixels, 32, 0, 1) - 0.0) < tol && std::abs(at(r->pixels, 32, 39, 1) - 1.0) < tol &&
                                  std::abs(at(r->pixels, 32, 20, 2) - 0.25) < tol;
                check("heif: its red and green ramps come back (within the codec's loss)", ramp,
                      std::to_string(at(r->pixels, 63, 20, 0)) + " " + std::to_string(at(r->pixels, 32, 39, 1)));
                check("heif: 72 dpi (HEIF keeps none)", r->dpi == 72, "");
            }
            // Opened as a document: one Background layer, kind HEIF.
            const auto c = core::file::open(p8, err);
            check("heif: opens as a document -- one layer, Background", c && c->layers.size() == 1 &&
                  c->layers.begin()->second.name == "Background" && c->kind == Fm::Kind::HEIF, err);
            if (c) check("heif: the document is 64 x 40", c->image.width == 64 && c->image.height == 40, "");
        }
        // 8-bit RGBA: alpha kept.
        const auto pa = (scratch() / (std::string("rgba8") + e.ext)).string();
        if (make(pa, e, 48, 32, true, 8, why)) {
            std::string err;
            const auto r = Fm::read_image(pa, err);
            check("heif: alpha kept (RGBA)", r && r->pixels.format().alpha, err);
        } else {
            check("heif: (no alpha encode here -- skipped)", true, why);
        }
        // 10-bit: opens 16-bit, the full range.
        const auto p10 = (scratch() / (std::string("rgb10") + e.ext)).string();
        if (make(p10, e, 64, 40, false, 10, why)) {
            std::string err;
            const auto r = Fm::read_image(p10, err);
            const bool deep = r && r->pixels.format().depth == klimt::pixel::Depth::U16;
            check("heif: a 10-bit picture opens 16-bit", deep, err);
            if (deep) check("heif: 10-bit white reaches the top of 16-bit", at(r->pixels, 63, 20, 0) > 0.95,
                            std::to_string(at(r->pixels, 63, 20, 0)));
        } else {
            check("heif: (no 10-bit encode here -- skipped)", true, why);
        }
        // Damaged: a cut file is refused in words, not a crash.
        {
            std::ifstream in(p8, std::ios::binary);
            std::vector<char> all((std::istreambuf_iterator<char>(in)), {});
            const auto cut = (scratch() / (std::string("cut") + e.ext)).string();
            std::ofstream(cut, std::ios::binary).write(all.data(), static_cast<std::streamsize>(all.size() / 2));
            std::string err;
            const auto r = Fm::read_image(cut, err);
            check("heif: a cut-short file is refused in words", !r && !err.empty(), err);
        }
    }
#endif
    std::error_code ec;
    fs::remove_all(scratch(), ec);
}
