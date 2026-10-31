// WebP in and out, on libwebp (s049). A WebP is 8-bit RGB with or without
// alpha; lossy or lossless. An animated WebP opens as its first frame (a
// note says so). The colour profile is the ICCP chunk (libwebpdemux reads
// it, libwebpmux writes it). WebP keeps no resolution: 72 dpi in, none out.
#include "formats/Formats.hpp"

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace klimt::formats {

namespace {
constexpr std::size_t kMaxFile = std::size_t{1} << 31;   // a WebP is at most 4 GB; we take 2

bool slurp(const std::string& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { error = "can't be opened"; return false; }
    const auto n = static_cast<std::size_t>(in.tellg());
    if (n > kMaxFile) { error = "too large"; return false; }
    out.resize(n);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
    if (!in) { error = "can't be read"; return false; }
    return true;
}

// Rows of 8-bit RGB(A) in, a Raster out.
Raster from_rgba(const std::uint8_t* px, int w, int h, int stride, bool alpha, const Progress& p, bool& cancelled) {
    Raster out;
    const pixel::Format f{pixel::Depth::U8, pixel::Mode::RGB, alpha};
    BandBuilder band(f, w, h);
    const int ch = alpha ? 4 : 3;
    for (int y = 0; y < h; ++y) {
        if (p.stop()) { cancelled = true; return out; }
        std::memcpy(band.row(y), px + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride),
                    static_cast<std::size_t>(w) * static_cast<std::size_t>(ch));
        band.done(y);
        if ((y & 63) == 0) p.at(0.5 + 0.5 * static_cast<double>(y) / h);
    }
    out.pixels = band.take();
    return out;
}

int writer(const std::uint8_t* data, std::size_t n, const WebPPicture* pic) {
    auto* v = static_cast<std::vector<std::uint8_t>*>(pic->custom_ptr);
    v->insert(v->end(), data, data + n);
    return 1;
}
struct Hook { const Progress* p; };
int on_progress(int percent, const WebPPicture* pic) {
    const auto* h = static_cast<const Hook*>(pic->user_data);
    h->p->at(0.5 + 0.5 * percent / 100.0);
    return h->p->stop() ? 0 : 1;
}
}  // namespace

std::optional<Raster> read_webp(const std::string& path, std::string& error, const Progress& p) {
    std::vector<std::uint8_t> file;
    if (!slurp(path, file, error)) return std::nullopt;
    WebPBitstreamFeatures feat;
    if (WebPGetFeatures(file.data(), file.size(), &feat) != VP8_STATUS_OK) { error = "not a readable WebP"; return std::nullopt; }
    if (feat.width <= 0 || feat.height <= 0 || feat.width > kMaxSide || feat.height > kMaxSide) {
        error = "too large (" + std::to_string(feat.width) + " x " + std::to_string(feat.height) + ")";
        return std::nullopt;
    }
    WebPData data{file.data(), file.size()};
    std::vector<std::uint8_t> icc;
    std::vector<std::string> notes;
    if (WebPDemuxer* dmx = WebPDemux(&data)) {
        WebPChunkIterator it;
        if (WebPDemuxGetChunk(dmx, "ICCP", 1, &it)) {
            icc.assign(it.chunk.bytes, it.chunk.bytes + it.chunk.size);
            WebPDemuxReleaseChunkIterator(&it);
        }
        WebPDemuxDelete(dmx);
    }
    p.at(0.1);
    bool cancelled = false;
    std::optional<Raster> out;
    if (feat.has_animation) {
        WebPAnimDecoderOptions o;
        WebPAnimDecoderOptionsInit(&o);
        o.color_mode = MODE_RGBA;
        o.use_threads = 1;
        WebPAnimDecoder* dec = WebPAnimDecoderNew(&data, &o);
        if (!dec) { error = "not a readable WebP animation"; return std::nullopt; }
        WebPAnimInfo info;
        std::uint8_t* frame = nullptr;
        int ts = 0;
        if (!WebPAnimDecoderGetInfo(dec, &info) || !WebPAnimDecoderGetNext(dec, &frame, &ts)) {
            WebPAnimDecoderDelete(dec);
            error = "a damaged WebP animation";
            return std::nullopt;
        }
        if (info.frame_count > 1) notes.push_back("first frame of " + std::to_string(info.frame_count));
        out = from_rgba(frame, static_cast<int>(info.canvas_width), static_cast<int>(info.canvas_height),
                        static_cast<int>(info.canvas_width) * 4, true, p, cancelled);
        WebPAnimDecoderDelete(dec);
    } else {
        const bool alpha = feat.has_alpha != 0;
        int w = 0, h = 0;
        std::uint8_t* px = alpha ? WebPDecodeRGBA(file.data(), file.size(), &w, &h)
                                 : WebPDecodeRGB(file.data(), file.size(), &w, &h);
        if (!px) { error = "a damaged WebP"; return std::nullopt; }
        p.at(0.5);
        out = from_rgba(px, w, h, w * (alpha ? 4 : 3), alpha, p, cancelled);
        WebPFree(px);
    }
    if (cancelled) { error = "cancelled"; return std::nullopt; }
    out->icc = std::move(icc);
    out->notes = std::move(notes);
    p.at(1);
    return out;
}

bool write_webp(const std::string& path, const Raster& r, std::string& error, const Progress& p, const WriteOptions& o) {
    const auto& src = r.pixels;
    const pixel::Format sf = src.format();
    if (sf.mode != pixel::Mode::Gray && sf.mode != pixel::Mode::RGB) { error = "WebP holds Gray and RGB only"; return false; }
    const int W = src.width(), H = src.height();
    if (W <= 0 || H <= 0) { error = "an empty image"; return false; }
    if (W > WEBP_MAX_DIMENSION || H > WEBP_MAX_DIMENSION) {
        error = "WebP holds at most " + std::to_string(WEBP_MAX_DIMENSION) + " px a side";
        return false;
    }
    // Every row to 8-bit RGB(A), straight.
    const int sc = sf.channels(), dc = sf.alpha ? 4 : 3;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * static_cast<std::size_t>(dc));
    std::vector<std::uint8_t> in(static_cast<std::size_t>(W) * static_cast<std::size_t>(sf.bytes_per_pixel()));
    for (int y = 0; y < H; ++y) {
        if (p.stop()) { error = "cancelled"; return false; }
        src.row_bytes(y, in.data());
        std::uint8_t* d = rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(W) * static_cast<std::size_t>(dc);
        for (int x = 0; x < W; ++x) {
            float s[5] = {};
            for (int c = 0; c < sc; ++c) {
                const std::size_t i = static_cast<std::size_t>(x) * static_cast<std::size_t>(sc) + static_cast<std::size_t>(c);
                switch (sf.depth) {
                    case pixel::Depth::U8:  s[c] = in[i] / 255.f; break;
                    case pixel::Depth::U16: { std::uint16_t v; std::memcpy(&v, in.data() + i * 2, 2); s[c] = v / 65535.f; break; }
                    case pixel::Depth::F32: { float v; std::memcpy(&v, in.data() + i * 4, 4); s[c] = v; break; }
                }
            }
            const auto q = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
            std::uint8_t* o8 = d + static_cast<std::size_t>(x) * static_cast<std::size_t>(dc);
            if (sf.mode == pixel::Mode::Gray) { o8[0] = o8[1] = o8[2] = q(s[0]); if (sf.alpha) o8[3] = q(s[1]); }
            else { o8[0] = q(s[0]); o8[1] = q(s[1]); o8[2] = q(s[2]); if (sf.alpha) o8[3] = q(s[3]); }
        }
        if ((y & 63) == 0) p.at(0.25 * y / H);
    }

    WebPConfig cfg;
    if (!WebPConfigInit(&cfg)) { error = "libwebp version mismatch"; return false; }
    cfg.quality = static_cast<float>(std::clamp(o.webp_quality, 0, 100));
    cfg.lossless = o.webp_lossless ? 1 : 0;
    cfg.exact = o.webp_lossless ? 1 : 0;   // lossless keeps the colour under clear pixels too
    cfg.thread_level = 1;
    cfg.method = o.webp_lossless ? 4 : 4;
    WebPPicture pic;
    if (!WebPPictureInit(&pic)) { error = "libwebp version mismatch"; return false; }
    pic.use_argb = cfg.lossless;
    pic.width = W;
    pic.height = H;
    const bool ok_import = sf.alpha ? WebPPictureImportRGBA(&pic, rgba.data(), W * 4) : WebPPictureImportRGB(&pic, rgba.data(), W * 3);
    if (!ok_import) { WebPPictureFree(&pic); error = "out of memory"; return false; }
    rgba.clear();
    rgba.shrink_to_fit();
    std::vector<std::uint8_t> bits;
    pic.writer = writer;
    pic.custom_ptr = &bits;
    Hook hook{&p};
    pic.user_data = &hook;
    pic.progress_hook = on_progress;
    const int ok = WebPEncode(&cfg, &pic);
    const auto code = pic.error_code;
    WebPPictureFree(&pic);
    if (!ok) {
        error = code == VP8_ENC_ERROR_USER_ABORT ? "cancelled" : "WebP encoder error " + std::to_string(static_cast<int>(code));
        return false;
    }
    // The profile: a mux re-wraps the bitstream with an ICCP chunk.
    std::vector<std::uint8_t> file_bytes;
    if (!r.icc.empty()) {
        WebPData img{bits.data(), bits.size()};
        WebPMux* mux = WebPMuxCreate(&img, 0);
        WebPData prof{r.icc.data(), r.icc.size()};
        WebPData assembled{nullptr, 0};
        if (mux && WebPMuxSetChunk(mux, "ICCP", &prof, 0) == WEBP_MUX_OK && WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK)
            file_bytes.assign(assembled.bytes, assembled.bytes + assembled.size);
        WebPDataClear(&assembled);
        if (mux) WebPMuxDelete(mux);
    }
    const std::vector<std::uint8_t>& out = file_bytes.empty() ? bits : file_bytes;
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { error = "can't be written"; return false; }
    const bool wrote = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    if (std::fclose(f) != 0 || !wrote) { std::remove(path.c_str()); error = "can't be written"; return false; }
    p.at(1);
    return true;
}

}  // namespace klimt::formats
