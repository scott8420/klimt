// HEIC / HEIF in, on libheif (s098; docs/formats.md "HEIC"). Read only --
// Klimt never writes one. An iPhone's photo: HEVC inside the HEIF box format;
// AVIF (AV1 inside the same boxes) opens the same way when libheif has an AV1
// decoder. The PRIMARY picture opens (a burst or a file of several says so);
// libheif turns it as the file says (irot / imir), so it opens upright. 8-bit
// opens 8-bit; 10- and 12-bit (an HDR iPhone shot) open 16-bit. The colour
// profile is kept (an iPhone's is Display P3); a file with only an nclx colour
// box that isn't sRGB says so in a note. HEIF keeps no resolution: 72 dpi.
//
// libheif is optional at build time (KLIMT_HAVE_HEIF): without it, opening a
// HEIC says "built without libheif" in words. With it but no HEVC decoder
// (Fedora's own libheif -- the patent), it says which package to add.
#include "formats/Formats.hpp"

#include <cstring>
#include <fstream>

#ifdef KLIMT_HAVE_HEIF
#include <libheif/heif.h>
#endif

namespace klimt::formats {

#ifdef KLIMT_HAVE_HEIF

namespace {

struct Init {   // libheif 1.13+: plugins (the decoders) load at heif_init
    Init() { heif_init(nullptr); }
    ~Init() { heif_deinit(); }
};
void init_once() { static Init i; }

constexpr const char* kNoHevc =
    "no HEVC decoder in this libheif -- on Fedora: sudo dnf install libheif-freeworld (RPM Fusion); "
    "on Ubuntu: sudo apt install libheif-plugin-libde265";
constexpr const char* kNoAv1 =
    "no AV1 decoder in this libheif -- on Fedora: sudo dnf install libheif (with dav1d or libaom); "
    "on Ubuntu: sudo apt install libheif-plugin-dav1d";

// AV1 inside (an AVIF) -- by its major brand, for the "no decoder" message.
bool is_avif_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    char b[12] = {};
    in.read(b, sizeof b);
    return in.gcount() == 12 && (std::memcmp(b + 8, "avif", 4) == 0 || std::memcmp(b + 8, "avis", 4) == 0);
}

std::string why(const heif_error& e, bool avif) {
    if (e.code == heif_error_Unsupported_feature || e.code == heif_error_Unsupported_filetype ||
        e.subcode == heif_suberror_Unsupported_codec)
        return avif ? kNoAv1 : kNoHevc;
    if (e.code == heif_error_Memory_allocation_error) return "too large";
    if (e.code == heif_error_Input_does_not_exist) return "can't be opened";
    return std::string(avif ? "a damaged AVIF (" : "a damaged HEIC (") + (e.message ? e.message : "?") + ")";
}

// libheif's 10 / 12 bit samples, little-endian in 16-bit words, to Klimt's full 16-bit range.
inline std::uint16_t widen(std::uint16_t v, int bits) {
    if (bits >= 16) return v;
    return static_cast<std::uint16_t>((v << (16 - bits)) | (v >> (2 * bits - 16)));
}

}  // namespace

bool heif_available() { return true; }
std::string heif_version() { return heif_get_version(); }

std::optional<Raster> read_heif(const std::string& path, std::string& error, const Progress& p) {
    init_once();
    const bool avif = is_avif_file(path);
    heif_context* ctx = heif_context_alloc();
    if (!ctx) { error = "out of memory"; return std::nullopt; }
    struct Free {
        heif_context* c = nullptr; heif_image_handle* h = nullptr; heif_image* i = nullptr;
        ~Free() { if (i) heif_image_release(i); if (h) heif_image_handle_release(h); if (c) heif_context_free(c); }
    } own{ctx};

    heif_error e = heif_context_read_from_file(ctx, path.c_str(), nullptr);
    if (e.code != heif_error_Ok) { error = why(e, avif); return std::nullopt; }
    e = heif_context_get_primary_image_handle(ctx, &own.h);
    if (e.code != heif_error_Ok) { error = why(e, avif); return std::nullopt; }
    heif_image_handle* h = own.h;

    const int w = heif_image_handle_get_width(h), H = heif_image_handle_get_height(h);
    if (w <= 0 || H <= 0 || w > kMaxSide || H > kMaxSide ||
        static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(H) > kMaxPixels) {
        error = "too large (" + std::to_string(w) + " x " + std::to_string(H) + ")";
        return std::nullopt;
    }
    const bool alpha = heif_image_handle_has_alpha_channel(h) != 0;
    const int bits = heif_image_handle_get_luma_bits_per_pixel(h);
    const bool deep = bits > 8;

    Raster out;
    std::vector<std::string> notes;
    const int top = heif_context_get_number_of_top_level_images(ctx);
    if (top > 1) notes.push_back("the main picture of " + std::to_string(top));

    // The colour profile: an ICC (prof / rICC) is kept; an nclx box alone
    // that isn't sRGB / BT.709 primaries is said.
    switch (heif_image_handle_get_color_profile_type(h)) {
        case heif_color_profile_type_prof:
        case heif_color_profile_type_rICC: {
            const std::size_t n = heif_image_handle_get_raw_color_profile_size(h);
            if (n > 0 && n < (std::size_t{64} << 20)) {
                out.icc.resize(n);
                if (heif_image_handle_get_raw_color_profile(h, out.icc.data()).code != heif_error_Ok) out.icc.clear();
            }
            break;
        }
        case heif_color_profile_type_nclx: {
            heif_color_profile_nclx* nclx = nullptr;
            if (heif_image_handle_get_nclx_color_profile(h, &nclx).code == heif_error_Ok && nclx) {
                if (nclx->color_primaries == heif_color_primaries_SMPTE_EG_432_1)
                    notes.push_back("Display P3 colour without a profile -- opened as sRGB");
                else if (nclx->color_primaries != heif_color_primaries_ITU_R_BT_709_5 &&
                         nclx->color_primaries != heif_color_primaries_unspecified)
                    notes.push_back("its colour space has no profile -- opened as sRGB");
                heif_nclx_color_profile_free(nclx);
            }
            break;
        }
        default: break;
    }
    p.at(0.1);
    if (p.stop()) { error = "cancelled"; return std::nullopt; }

    const heif_chroma chroma = deep ? (alpha ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBB_LE)
                                    : (alpha ? heif_chroma_interleaved_RGBA : heif_chroma_interleaved_RGB);
    heif_decoding_options* opt = heif_decoding_options_alloc();   // transformations applied (irot / imir): upright
    e = heif_decode_image(h, &own.i, heif_colorspace_RGB, chroma, opt);
    heif_decoding_options_free(opt);
    if (e.code != heif_error_Ok) { error = why(e, avif); return std::nullopt; }
    p.at(0.6);

    int stride = 0;
    const std::uint8_t* px = heif_image_get_plane_readonly(own.i, heif_channel_interleaved, &stride);
    const int iw = heif_image_get_width(own.i, heif_channel_interleaved);
    const int ih = heif_image_get_height(own.i, heif_channel_interleaved);   // turned: may swap w / h
    if (!px || iw <= 0 || ih <= 0) { error = "a damaged HEIC (no pixels)"; return std::nullopt; }
    const int sbits = deep ? heif_image_get_bits_per_pixel_range(own.i, heif_channel_interleaved) : 8;

    const pixel::Format f{deep ? pixel::Depth::U16 : pixel::Depth::U8, pixel::Mode::RGB, alpha};
    BandBuilder band(f, iw, ih);
    const std::size_t samples = static_cast<std::size_t>(iw) * (alpha ? 4u : 3u);
    for (int y = 0; y < ih; ++y) {
        if (p.stop()) { error = "cancelled"; return std::nullopt; }
        const std::uint8_t* src = px + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        std::uint8_t* dst = band.row(y);
        if (!deep) {
            std::memcpy(dst, src, samples);
        } else {
            auto* d16 = reinterpret_cast<std::uint16_t*>(dst);
            for (std::size_t k = 0; k < samples; ++k) {
                const std::uint16_t v = static_cast<std::uint16_t>(src[2 * k] | (src[2 * k + 1] << 8));
                d16[k] = widen(v, sbits);
            }
        }
        band.done(y);
        if ((y & 63) == 0) p.at(0.6 + 0.4 * static_cast<double>(y) / ih);
    }
    out.pixels = band.take();
    out.notes = std::move(notes);
    p.at(1);
    return out;
}

#else   // no libheif

bool heif_available() { return false; }
std::string heif_version() { return {}; }

std::optional<Raster> read_heif(const std::string&, std::string& error, const Progress&) {
    error = "Klimt was built without libheif -- install libheif-devel (Ubuntu: libheif-dev) and rebuild";
    return std::nullopt;
}

#endif

}  // namespace klimt::formats
