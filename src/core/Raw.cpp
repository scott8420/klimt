// core/Raw -- camera RAW through LibRaw (s085). See core/Raw.hpp and
// docs/raw.md.
#include "core/Raw.hpp"
#include "tint/Tint.hpp"
#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <unistd.h>

#ifdef KLIMT_HAVE_LIBRAW
#include <libraw/libraw.h>
#endif

namespace klimt::core::raw {

namespace fs = std::filesystem;
namespace P = pixel;
using json = nlohmann::json;

namespace {

const char* kBalanceKeys[] = {"as-shot", "auto", "daylight"};
const char* kHighlightKeys[] = {"clip", "blend", "rebuild"};
const char* kSpaceKeys[] = {"srgb", "adobe-rgb", "prophoto"};

std::mutex g_mutex;
Develop g_defaults;

std::string lower_ext(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::string shutter_text(double s) {
    if (s <= 0) return {};
    char b[32];
    if (s < 0.5) std::snprintf(b, sizeof b, "1/%.0f s", 1.0 / s);
    else std::snprintf(b, sizeof b, "%.1f s", s);
    return b;
}

// A map turned as LibRaw's `flip` says (0 none, 3 half, 5 counter-clockwise, 6 clockwise).
P::TileMap turned(const P::TileMap& m, int flip) {
    if (flip != 3 && flip != 5 && flip != 6) return m;
    const int W = m.width(), H = m.height();
    const int w = flip == 3 ? W : H, h = flip == 3 ? H : W;
    P::TileMap out(m.format(), w, h);
    return out.paint({0, 0, w, h}, [&](int x, int y, const P::Px&) {
        if (flip == 3) return m.get(W - 1 - x, H - 1 - y);
        if (flip == 6) return m.get(y, H - 1 - x);   // clockwise
        return m.get(W - 1 - y, x);                   // counter-clockwise
    });
}

}  // namespace

const char* balance_label(Balance b) {
    switch (b) { case Balance::AsShot: return "As Shot"; case Balance::Auto: return "Auto"; case Balance::Daylight: return "Daylight"; }
    return "";
}
const char* highlights_label(Highlights h) {
    switch (h) { case Highlights::Clip: return "Clip"; case Highlights::Blend: return "Blend"; case Highlights::Rebuild: return "Rebuild"; }
    return "";
}
const char* space_label(Space s) {
    switch (s) { case Space::SRGB: return "sRGB"; case Space::AdobeRGB: return "Adobe RGB (1998)"; case Space::ProPhoto: return "ProPhoto RGB"; }
    return "";
}

std::string develop_text(const Develop& d) {
    return json{{"balance", kBalanceKeys[static_cast<int>(d.balance)]}, {"warmth", d.warmth}, {"exposure", d.exposure},
                {"auto_bright", d.auto_bright}, {"highlights", kHighlightKeys[static_cast<int>(d.highlights)]},
                {"noise", d.noise}, {"space", kSpaceKeys[static_cast<int>(d.space)]}}
        .dump();
}

Develop develop_parse(const std::string& text) {
    Develop d;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return d;
    const auto pick = [&](const char* key, const char* const* keys, int n, auto& out) {
        if (!j.contains(key) || !j[key].is_string()) return;
        const auto v = j[key].get<std::string>();
        for (int i = 0; i < n; ++i)
            if (v == keys[i]) out = static_cast<std::remove_reference_t<decltype(out)>>(i);
    };
    pick("balance", kBalanceKeys, 3, d.balance);
    pick("highlights", kHighlightKeys, 3, d.highlights);
    pick("space", kSpaceKeys, 3, d.space);
    if (j.contains("warmth") && j["warmth"].is_number()) d.warmth = std::clamp(j["warmth"].get<int>(), -100, 100);
    if (j.contains("exposure") && j["exposure"].is_number()) d.exposure = std::clamp(j["exposure"].get<double>(), -2.0, 3.0);
    if (j.contains("auto_bright") && j["auto_bright"].is_boolean()) d.auto_bright = j["auto_bright"].get<bool>();
    if (j.contains("noise") && j["noise"].is_number()) d.noise = std::clamp(j["noise"].get<int>(), 0, 1000);
    return d;
}

std::string Info::line() const {
    std::string s = make.empty() && model.empty() ? std::string("A camera") : (model.rfind(make, 0) == 0 ? model : make + " " + model);
    if (iso > 0) s += " · ISO " + std::to_string(static_cast<int>(std::lround(iso)));
    if (shutter > 0) s += " · " + shutter_text(shutter);
    char b[32];
    if (aperture > 0) { std::snprintf(b, sizeof b, " · f/%.1f", aperture); s += b; }
    if (focal > 0) { std::snprintf(b, sizeof b, " · %.0f mm", focal); s += b; }
    if (width > 0) s += " · " + std::to_string(width) + " × " + std::to_string(height);
    return s;
}

bool is_raw_name(const std::string& path) {
    static const char* exts[] = {".3fr", ".ari", ".arw", ".bay", ".cr2", ".cr3", ".crw", ".dcr", ".dcs", ".dng", ".erf",
                                 ".fff", ".iiq", ".k25", ".kdc", ".mef", ".mos", ".mrw", ".nef", ".nrw", ".orf", ".pef",
                                 ".raf", ".raw", ".rw2", ".rwl", ".sr2", ".srf", ".srw", ".x3f"};
    const auto e = lower_ext(path);
    return std::any_of(std::begin(exts), std::end(exts), [&](const char* x) { return e == x; });
}

void set_defaults(const Develop& d) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_defaults = d;
}
Develop defaults() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_defaults;
}

std::optional<file::Contents> open(const std::string& path, const Develop& d, std::string& error, const formats::Progress& p) {
    auto dev = develop(path, d, false, error, p);
    if (!dev) return std::nullopt;
    file::Contents c;
    c.kind = formats::Kind::Unknown;
    c.image.width = dev->pixels.width();
    c.image.height = dev->pixels.height();
    c.image.format = dev->pixels.format();
    c.image.dpi = 300;   // a RAW says none; a print size to start from
    if (!dev->icc.empty()) c.image.icc = std::make_shared<const std::vector<std::uint8_t>>(std::move(dev->icc));
    Layer bg;
    bg.name = "Background";
    bg.pos = 1;
    bg.pixels = std::make_shared<const P::TileMap>(std::move(dev->pixels));
    c.layers["L1"] = std::move(bg);
    c.next_id = 2;
    char ev[24];
    std::snprintf(ev, sizeof ev, "%+.1f EV", d.exposure);
    c.notes.push_back(std::string("developed: ") + balance_label(d.balance) + (d.exposure != 0 ? std::string(", ") + ev : "") + ", " +
                      space_label(d.space));
    return c;
}

#ifdef KLIMT_HAVE_LIBRAW

namespace {

struct Cancel {
    const formats::Progress* p;
};
int on_progress(void* data, enum LibRaw_progress stage, int iteration, int expected) {
    const auto* c = static_cast<Cancel*>(data);
    if (c->p && c->p->stop()) return 1;   // non-zero: LibRaw stops (LIBRAW_CANCELLED_BY_CALLBACK)
    if (c->p && expected > 0) {
        // The stages run in order, each a power of two: a rough fraction from which one.
        int k = 0;
        for (unsigned s = static_cast<unsigned>(stage); s > 1; s >>= 1) ++k;
        c->p->at(std::min(0.95, (k + static_cast<double>(iteration) / expected) / 22.0));
    }
    return 0;
}

std::string why(int code) {
    if (code == LIBRAW_FILE_UNSUPPORTED) return "a camera file LibRaw doesn't know";
    if (code == LIBRAW_CANCELLED_BY_CALLBACK) return "cancelled";
    if (code == LIBRAW_IO_ERROR || code == LIBRAW_DATA_ERROR) return "a damaged RAW file";
    return libraw_strerror(code);
}

// What the file says (no sizes: adjust_sizes_info_only() would end the
// LibRaw object's open -- develop() takes them from the picture).
Info basics_of(LibRaw& r) {
    Info i;
    const auto& d = r.imgdata;
    i.make = d.idata.make;
    i.model = d.idata.model;
    i.lens = d.lens.Lens;
    i.iso = d.other.iso_speed;
    i.shutter = d.other.shutter;
    i.aperture = d.other.aperture;
    i.focal = d.other.focal_len;
    i.taken = static_cast<std::int64_t>(d.other.timestamp);
    return i;
}
Info info_of(LibRaw& r) {
    Info i = basics_of(r);
    r.adjust_sizes_info_only();
    i.width = r.imgdata.sizes.iwidth;
    i.height = r.imgdata.sizes.iheight;
    return i;
}

}  // namespace

bool available() { return true; }
std::string version() { return LibRaw::version(); }

bool is_raw(const std::string& path) {
    if (is_raw_name(path)) return true;
    auto r = std::make_unique<LibRaw>();
    return r->open_file(path.c_str()) == LIBRAW_SUCCESS;
}

std::optional<Info> info(const std::string& path, std::string& error) {
    auto r = std::make_unique<LibRaw>();
    const int e = r->open_file(path.c_str());
    if (e != LIBRAW_SUCCESS) { error = why(e); return std::nullopt; }
    return info_of(*r);
}

std::optional<P::TileMap> preview(const std::string& path, std::string& error) {
    auto r = std::make_unique<LibRaw>();
    int e = r->open_file(path.c_str());
    if (e != LIBRAW_SUCCESS) { error = why(e); return std::nullopt; }
    const int flip = r->imgdata.sizes.flip;
    e = r->unpack_thumb();
    if (e != LIBRAW_SUCCESS) { error = "no preview inside it"; return std::nullopt; }
    libraw_processed_image_t* t = r->dcraw_make_mem_thumb(&e);
    if (!t) { error = "no preview inside it"; return std::nullopt; }
    std::optional<P::TileMap> out;
    if (t->type == LIBRAW_IMAGE_JPEG) {
        const std::string tmp = (fs::temp_directory_path() / ("klimt-raw-thumb-" + std::to_string(::getpid()) + "-" +
                                                              std::to_string(reinterpret_cast<std::uintptr_t>(t)) + ".jpg")).string();
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(t->data), static_cast<std::streamsize>(t->data_size));
        }
        if (auto rr = formats::read_jpeg(tmp, error)) out = std::move(rr->pixels);
        std::remove(tmp.c_str());
    } else if (t->type == LIBRAW_IMAGE_BITMAP && t->colors == 3 && t->bits == 8) {
        const P::Format f8{P::Depth::U8, P::Mode::RGB, true};
        formats::BandBuilder bb(f8, t->width, t->height);
        for (int y = 0; y < t->height; ++y) {
            std::uint8_t* d = bb.row(y);
            const std::uint8_t* s = t->data + static_cast<std::size_t>(y) * t->width * 3;
            for (int x = 0; x < t->width; ++x) {
                d[x * 4] = s[x * 3]; d[x * 4 + 1] = s[x * 3 + 1]; d[x * 4 + 2] = s[x * 3 + 2]; d[x * 4 + 3] = 255;
            }
            bb.done(y);
        }
        out = bb.take();
    }
    LibRaw::dcraw_clear_mem(t);
    if (!out) { if (error.empty()) error = "no preview inside it"; return std::nullopt; }
    return turned(*out, flip);
}

std::optional<Developed> develop(const std::string& path, const Develop& d, bool half, std::string& error,
                                 const formats::Progress& p) {
    auto r = std::make_unique<LibRaw>();
    Cancel cancel{&p};
    r->set_progress_handler(&on_progress, &cancel);
    int e = r->open_file(path.c_str());
    if (e != LIBRAW_SUCCESS) { error = why(e); return std::nullopt; }
    auto& prm = r->imgdata.params;
    prm.output_bps = 16;
    prm.half_size = half ? 1 : 0;
    prm.user_qual = half ? 0 : 3;   // linear for the preview, AHD for the picture
    prm.highlight = d.highlights == Highlights::Clip ? 0 : d.highlights == Highlights::Blend ? 2 : 5;
    prm.no_auto_bright = d.auto_bright ? 0 : 1;
    prm.threshold = static_cast<float>(std::clamp(d.noise, 0, 1000));
    if (d.exposure != 0) {
        prm.exp_correc = 1;
        prm.exp_shift = static_cast<float>(std::pow(2.0, std::clamp(d.exposure, -2.0, 3.0)));
        prm.exp_preser = 0.8f;   // keep the highlights when brightening
    }
    switch (d.space) {
        case Space::SRGB: prm.output_color = 1; prm.gamm[0] = 1 / 2.4; prm.gamm[1] = 12.92; break;
        case Space::AdobeRGB: prm.output_color = 2; prm.gamm[0] = 256.0 / 563.0; prm.gamm[1] = 0; break;
        case Space::ProPhoto: prm.output_color = 4; prm.gamm[0] = 1 / 1.8; prm.gamm[1] = 0; break;
    }
    // White balance: the camera's, LibRaw's grey-world, or daylight -- warmed or cooled.
    const auto& col = r->imgdata.color;
    const bool shot = col.cam_mul[0] > 0 && col.cam_mul[1] > 0;
    Balance b = d.balance == Balance::AsShot && !shot ? Balance::Auto : d.balance;
    if (b == Balance::Auto) {
        prm.use_auto_wb = 1;
    } else {
        const float* base = b == Balance::AsShot ? col.cam_mul : col.pre_mul;
        const double w = std::clamp(d.warmth, -100, 100) / 200.0;   // +-0.5 stop on red / blue
        if (w == 0 && b == Balance::AsShot) prm.use_camera_wb = 1;
        else if (base[0] > 0 && base[1] > 0) {
            prm.user_mul[0] = static_cast<float>(base[0] * std::pow(2.0, w));
            prm.user_mul[1] = base[1];
            prm.user_mul[2] = static_cast<float>(base[2] * std::pow(2.0, -w));
            prm.user_mul[3] = base[3] > 0 ? base[3] : base[1];
        }
    }
    Info about = basics_of(*r);
    e = r->unpack();
    if (e != LIBRAW_SUCCESS) { error = why(e); return std::nullopt; }
    e = r->dcraw_process();
    if (e != LIBRAW_SUCCESS) { error = why(e); return std::nullopt; }
    libraw_processed_image_t* im = r->dcraw_make_mem_image(&e);
    if (!im) { error = why(e); return std::nullopt; }
    if (im->type != LIBRAW_IMAGE_BITMAP || im->colors != 3 || im->bits != 16) {
        LibRaw::dcraw_clear_mem(im);
        error = "LibRaw gave back an unexpected picture";
        return std::nullopt;
    }
    const P::Format f16{P::Depth::U16, P::Mode::RGB, true};
    formats::BandBuilder bb(f16, im->width, im->height);
    const auto* src = reinterpret_cast<const std::uint16_t*>(im->data);
    for (int y = 0; y < im->height; ++y) {
        auto* dst = reinterpret_cast<std::uint16_t*>(bb.row(y));
        const std::uint16_t* s = src + static_cast<std::size_t>(y) * im->width * 3;
        for (int x = 0; x < im->width; ++x) {
            dst[x * 4] = s[x * 3]; dst[x * 4 + 1] = s[x * 3 + 1]; dst[x * 4 + 2] = s[x * 3 + 2]; dst[x * 4 + 3] = 65535;
        }
        bb.done(y);
        if ((y & 255) == 0 && p.stop()) { LibRaw::dcraw_clear_mem(im); error = "cancelled"; return std::nullopt; }
    }
    about.width = im->width;
    about.height = im->height;
    LibRaw::dcraw_clear_mem(im);
    Developed out{bb.take(), {}, about};
    if (d.space == Space::AdobeRGB) out.icc = tint::adobe_rgb();
    else if (d.space == Space::ProPhoto) out.icc = tint::prophoto_rgb();
    p.at(1);
    return out;
}

#else   // built without LibRaw: every way in says so

namespace {
const char* kWithout = "Klimt was built without LibRaw -- install LibRaw-devel and build again";
}
bool available() { return false; }
std::string version() { return {}; }
bool is_raw(const std::string& path) { return is_raw_name(path); }
std::optional<Info> info(const std::string&, std::string& error) { error = kWithout; return std::nullopt; }
std::optional<P::TileMap> preview(const std::string&, std::string& error) { error = kWithout; return std::nullopt; }
std::optional<Developed> develop(const std::string&, const Develop&, bool, std::string& error, const formats::Progress&) {
    error = kWithout;
    return std::nullopt;
}

#endif

}  // namespace klimt::core::raw
