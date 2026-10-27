// klimt_selftest -- libs/formats and the pixel-lib pieces it stands on (s007).
// Round trips through real files in a scratch folder: what goes in at a depth
// and mode comes back at that depth and mode, exactly for the lossless
// formats, within JPEG's tolerance for JPEG.
#include "formats/Formats.hpp"
#include "pixel/Pixel.hpp"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>
#include <array>
#include <map>

namespace P  = klimt::pixel;
namespace Fm = klimt::formats;
namespace fs = std::filesystem;

using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
fs::path scratch() {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_" + std::to_string(::getpid()));
        fs::create_directories(p);
        return p;
    }();
    return d;
}

// A test picture at any format: a smooth ramp per channel, alpha a ramp too,
// so every sample differs and every channel is checked.
P::TileMap picture(const P::Format& f, int w, int h) {
    P::TileMap m(f, w, h);
    return m.paint({0, 0, w, h}, [&](int x, int y, const P::Px&) {
        P::Px p;
        const int n = f.channels();
        for (int k = 0; k < n; ++k)
            p.c[static_cast<std::size_t>(k)] = static_cast<float>(((x * (k + 1) + y * (3 - k % 3)) % 251) / 250.0);
        if (f.alpha) p.c[static_cast<std::size_t>(n - 1)] = static_cast<float>((x + y) % 200 / 199.0 * 0.9 + 0.1);
        return p;
    });
}

// Largest difference in any channel between two maps of one size.
double diff(const P::TileMap& a, const P::TileMap& b) {
    double worst = 0;
    const int n = std::min(a.format().channels(), b.format().channels());
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const auto pa = a.get(x, y), pb = b.get(x, y);
            for (int k = 0; k < n; ++k)
                worst = std::max(worst, static_cast<double>(std::abs(pa.c[static_cast<std::size_t>(k)] - pb.c[static_cast<std::size_t>(k)])));
        }
    return worst;
}

// A minimal ICC profile libpng accepts: the header ('acsp', D50) and one
// tag (a white point), then noise to 512 bytes (libpng refuses an iCCP
// chunk that compresses to almost nothing as "too short").
std::vector<std::uint8_t> fake_icc(const char* space) {
    std::vector<std::uint8_t> v(512, 0);
    for (std::size_t i = 164; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 13);
    const auto be = [&](std::size_t at, std::uint32_t x) {
        v[at] = static_cast<std::uint8_t>(x >> 24); v[at + 1] = static_cast<std::uint8_t>(x >> 16);
        v[at + 2] = static_cast<std::uint8_t>(x >> 8); v[at + 3] = static_cast<std::uint8_t>(x);
    };
    be(0, 512);
    be(8, 0x02100000);
    std::memcpy(v.data() + 12, "mntr", 4);
    std::memcpy(v.data() + 16, space, 4);
    std::memcpy(v.data() + 20, "XYZ ", 4);
    std::memcpy(v.data() + 36, "acsp", 4);
    be(68, 0x0000F6D6); be(72, 0x00010000); be(76, 0x0000D32D);
    be(128, 1);
    std::memcpy(v.data() + 132, "wtpt", 4);
    be(136, 144);
    be(140, 20);
    std::memcpy(v.data() + 144, "XYZ ", 4);
    be(152, 0x0000F6D6); be(156, 0x00010000); be(160, 0x0000D32D);
    return v;
}

std::string fmt_name(const P::Format& f) {
    const char* m[] = {"Gray", "RGB", "CMYK", "Lab"};
    const char* d[] = {"8", "16", "32f"};
    return std::string(m[static_cast<int>(f.mode)]) + d[static_cast<int>(f.depth)] + (f.alpha ? "+a" : "");
}
}  // namespace

void selftest_formats(const Check& check) {
    std::cout << "\n-- pixel: raw tiles (s007) --\n";
    {
        const P::Format f{P::Depth::U8, P::Mode::RGB, true};
        std::vector<std::uint8_t> same(static_cast<std::size_t>(P::kTile) * P::kTile * 4);
        for (std::size_t i = 0; i < same.size(); i += 4) { same[i] = 10; same[i + 1] = 20; same[i + 2] = 30; same[i + 3] = 255; }
        const auto u = P::Tile::from_bytes(f, same);
        check("from_bytes: one colour collapses to a uniform tile", u && u->is_uniform() && u->bytes() == 4, "");
        check("from_bytes: all zero WITH alpha is no tile (transparent)",
              P::Tile::from_bytes(f, std::vector<std::uint8_t>(same.size(), 0)) == nullptr, "");
        const P::Format g{P::Depth::U8, P::Mode::Gray, false};
        const auto black = P::Tile::from_bytes(g, std::vector<std::uint8_t>(static_cast<std::size_t>(P::kTile) * P::kTile, 0));
        check("from_bytes: all zero WITHOUT alpha is black, kept", black && black->is_uniform(), "");
        check("from_bytes: the wrong size is refused", P::Tile::from_bytes(f, std::vector<std::uint8_t>(7)) == nullptr, "");
        same[400] = 99;
        const auto full = P::Tile::from_bytes(f, same);
        check("from_bytes: one pixel different keeps the whole tile", full && !full->is_uniform() && full->get(100, 0).c[0] > 0.38f, "");
        const auto m = picture(f, 300, 260);
        std::vector<std::uint8_t> row(300 * 4);
        bool ok = true;
        for (int y : {0, 255, 256, 259}) {
            m.row_bytes(y, row.data());
            for (int x : {0, 255, 256, 299})
                if (std::abs(row[static_cast<std::size_t>(x) * 4 + 1] / 255.f - m.get(x, y).c[1]) > 1e-6f) ok = false;
        }
        check("row_bytes: a row read across tiles is the map's pixels", ok, "");
        auto grid = std::vector<P::TileMap::TileRef>{m.tile(0, 0), m.tile(1, 1)};
        const auto made = P::TileMap::from_tiles(f, 300, 260, grid);
        check("from_tiles: the grid becomes the map (padded to cols x rows)",
              made.tile(0, 0) == m.tile(0, 0) && made.tile(1, 0) == m.tile(1, 1) && made.tile(1, 1) == nullptr && made.cols() == 2, "");
    }

    std::cout << "\n-- formats: the band builder --\n";
    {
        const P::Format f{P::Depth::U16, P::Mode::RGB, false};
        const auto src = picture(f, 600, 300);
        Fm::BandBuilder b(f, 600, 300);
        for (int y = 0; y < 300; ++y) { src.row_bytes(y, b.row(y)); b.done(y); }
        const auto got = b.take();
        check("band: rows in, the same pixels out", got.width() == 600 && got.height() == 300 && diff(src, got) == 0, "");
        // An edge tile whose live part is one colour is uniform, not 256 KB.
        const auto flat = P::TileMap(f, 300, 300).fill({0, 0, 300, 300}, P::rgba(0.5f, 0.25f, 0.75f));
        Fm::BandBuilder b2(f, 300, 300);
        for (int y = 0; y < 300; ++y) { flat.row_bytes(y, b2.row(y)); b2.done(y); }
        const auto got2 = b2.take();
        check("band: an edge tile of one colour is uniform", got2.tile(1, 1) && got2.tile(1, 1)->is_uniform(), "");
    }

    std::cout << "\n-- formats: sniffing --\n";
    const auto path = [](const std::string& name) { return (scratch() / name).string(); };
    {
        std::ofstream(path("junk.png")) << "this is not an image at all";
        check("sniff: text is Unknown", Fm::sniff(path("junk.png")) == Fm::Kind::Unknown, "");
        std::string err;
        check("read_image: Unknown is an error in words, not a throw",
              !Fm::read_image(path("junk.png"), err) && !err.empty(), err);
        check("read_image: a missing file says so", !Fm::read_image(path("nope.png"), err) && err == "can't be read", err);
    }

    std::cout << "\n-- formats: PNG --\n";
    for (const P::Format f : {P::Format{P::Depth::U8, P::Mode::RGB, true}, P::Format{P::Depth::U8, P::Mode::RGB, false},
                              P::Format{P::Depth::U16, P::Mode::RGB, true}, P::Format{P::Depth::U8, P::Mode::Gray, false},
                              P::Format{P::Depth::U16, P::Mode::Gray, true}}) {
        Fm::Raster r;
        r.pixels = picture(f, 333, 270);
        r.dpi = 300;
        r.icc = fake_icc("RGB ");
        if (const char* e = std::getenv("KLIMT_ICC")) { std::ifstream in(e, std::ios::binary); r.icc.assign(std::istreambuf_iterator<char>(in), {}); }   // dropped (benignly) on the Gray files
        std::string err;
        const auto file = path("rt_" + fmt_name(f) + ".png");
        const bool wrote = Fm::write_png(file, r, err);
        const auto back = Fm::read_image(file, err);
        check("png " + fmt_name(f) + ": round trip, same format, exact",
              wrote && back && back->pixels.format() == f && back->pixels.width() == 333 && diff(r.pixels, back->pixels) == 0, err);
        if (f.depth == P::Depth::U8 && f.mode == P::Mode::RGB && f.alpha)
            check("png: resolution and profile come back", back && std::abs(back->dpi - 300) < 0.05 && back->icc == r.icc,
                  back ? std::to_string(back->dpi) : "");
    }
    {
        Fm::Raster r;
        r.pixels = picture({P::Depth::F32, P::Mode::RGB, true}, 64, 64);
        std::string err;
        Fm::write_png(path("f32.png"), r, err);
        const auto back = Fm::read_png(path("f32.png"), err);
        check("png: 32-bit float is written 16-bit", back && back->pixels.format().depth == P::Depth::U16 &&
                                                         diff(r.pixels, back->pixels) < 1.0 / 65535 + 1e-6, err);
        r.pixels = picture({P::Depth::U8, P::Mode::CMYK, false}, 8, 8);
        check("png: CMYK is refused in words", !Fm::write_png(path("cmyk.png"), r, err) && !err.empty(), err);
    }

    std::cout << "\n-- formats: JPEG --\n";
    for (const P::Format f : {P::Format{P::Depth::U8, P::Mode::RGB, false}, P::Format{P::Depth::U8, P::Mode::Gray, false},
                              P::Format{P::Depth::U8, P::Mode::CMYK, false}}) {
        Fm::Raster r;
        // JPEG is lossy: a smooth picture, so the tolerance means something.
        r.pixels = P::TileMap(f, 300, 200).paint({0, 0, 300, 200}, [&](int x, int y, const P::Px&) {
            P::Px p;
            for (int k = 0; k < f.colour_channels(); ++k)
                p.c[static_cast<std::size_t>(k)] = static_cast<float>(0.2 + 0.6 * (x + k * 37) / 450.0 * (y + 50) / 250.0);
            return p;
        });
        r.dpi = 150;
        std::string err;
        Fm::WriteOptions o;
        o.jpeg_quality = 100;
        const auto file = path("rt_" + fmt_name(f) + ".jpg");
        const bool wrote = Fm::write_jpeg(file, r, err, {}, o);
        const auto back = Fm::read_image(file, err);
        const double d = back ? diff(r.pixels, back->pixels) : 1;
        check("jpeg " + fmt_name(f) + ": round trip, same mode, within tolerance",
              wrote && back && back->pixels.format() == f && d < 0.03 &&
              (f.mode == P::Mode::CMYK || std::abs(back->dpi - 150) < 0.5),   // no JFIF (so no dpi) on CMYK
              "max diff " + std::to_string(d) + " " + err);
    }
    {
        Fm::Raster r;
        r.pixels = picture({P::Depth::U16, P::Mode::RGB, true}, 40, 40);
        std::string err;
        const bool wrote = Fm::write_jpeg(path("alpha.jpg"), r, err);
        const auto back = Fm::read_jpeg(path("alpha.jpg"), err);
        check("jpeg: alpha and 16-bit drop to 8-bit RGB", wrote && back && back->pixels.format() == P::Format{P::Depth::U8, P::Mode::RGB, false}, err);
        // A JPEG named .png still opens as what it is.
        fs::copy_file(path("alpha.jpg"), path("really_a_jpeg.png"), fs::copy_options::overwrite_existing);
        check("sniff: by content, not by name", Fm::sniff(path("really_a_jpeg.png")) == Fm::Kind::JPEG, "");
    }

    std::cout << "\n-- formats: TIFF --\n";
    for (const P::Format f : {P::Format{P::Depth::U8, P::Mode::RGB, true}, P::Format{P::Depth::U16, P::Mode::RGB, false},
                              P::Format{P::Depth::F32, P::Mode::RGB, true}, P::Format{P::Depth::U8, P::Mode::CMYK, false},
                              P::Format{P::Depth::U16, P::Mode::CMYK, true}, P::Format{P::Depth::U8, P::Mode::Lab, false},
                              P::Format{P::Depth::U16, P::Mode::Lab, true}, P::Format{P::Depth::F32, P::Mode::Lab, false},
                              P::Format{P::Depth::U8, P::Mode::Gray, true}, P::Format{P::Depth::F32, P::Mode::Gray, false}}) {
        Fm::Raster r;
        r.pixels = picture(f, 290, 300);
        r.dpi = 600;
        r.icc = {9, 8, 7};
        std::string err;
        const auto file = path("rt_" + fmt_name(f) + ".tif");
        const bool wrote = Fm::write_tiff(file, r, err);
        const auto back = Fm::read_image(file, err);
        const double d = back ? diff(r.pixels, back->pixels) : 1;
        // Float Lab goes through real units and back: float rounding only.
        const double tol = f.depth == P::Depth::F32 && f.mode == P::Mode::Lab ? 1e-5 : 0;
        check("tiff " + fmt_name(f) + ": round trip, same format, exact", wrote && back && back->pixels.format() == f && d <= tol &&
              std::abs(back->dpi - 600) < 0.01 && back->icc == r.icc, "diff " + std::to_string(d) + " " + err);
    }
    {
        Fm::Raster r;
        r.pixels = picture({P::Depth::U8, P::Mode::RGB, false}, 100, 100);
        std::string err;
        Fm::WriteOptions o;
        o.tiff_deflate = false;
        const bool wrote = Fm::write_tiff(path("plain.tif"), r, err, {}, o);
        const auto back = Fm::read_tiff(path("plain.tif"), err);
        check("tiff: uncompressed too", wrote && back && diff(r.pixels, back->pixels) == 0, err);
        check("sniff: TIFF", Fm::sniff(path("plain.tif")) == Fm::Kind::TIFF, "");
    }

    std::cout << "\n-- formats: cancel, damage, progress --\n";
    {
        Fm::Raster r;
        r.pixels = picture({P::Depth::U8, P::Mode::RGB, true}, 512, 600);
        std::string err;
        Fm::write_png(path("big.png"), r, err);
        Fm::Progress stop;
        stop.cancelled = [] { return true; };
        for (const auto& [name, fn] : std::vector<std::pair<std::string, std::function<bool(const std::string&)>>>{
                 {"png", [&](const std::string& p) { return Fm::write_png(p, r, err, stop); }},
                 {"jpeg", [&](const std::string& p) { return Fm::write_jpeg(p, r, err, stop); }},
                 {"tiff", [&](const std::string& p) { return Fm::write_tiff(p, r, err, stop); }}}) {
            const auto p = path("cancelled." + name);
            const bool wrote = fn(p);
            check("write " + name + ": a cancel leaves no file", !wrote && err == "cancelled" && !fs::exists(p), err);
        }
        check("read: a cancel returns nothing", !Fm::read_image(path("big.png"), err, stop) && err == "cancelled", err);
        double last = -1;
        bool rising = true;
        Fm::Progress follow;
        follow.progress = [&](double f) { if (f < last) rising = false; last = f; };
        Fm::read_image(path("big.png"), err, follow);
        check("read: progress rises to 1", rising && last == 1.0, std::to_string(last));
        // A truncated PNG: an error, not a crash.
        {
            std::ifstream in(path("big.png"), std::ios::binary);
            std::string all((std::istreambuf_iterator<char>(in)), {});
            std::ofstream(path("cut.png"), std::ios::binary).write(all.data(), static_cast<std::streamsize>(all.size() / 3));
        }
        check("png: a truncated file is an error in words", !Fm::read_image(path("cut.png"), err) && !err.empty(), err);
        {
            std::ifstream in(path("rt_RGB16.tif"), std::ios::binary);
            std::string all((std::istreambuf_iterator<char>(in)), {});
            std::ofstream(path("cut.tif"), std::ios::binary).write(all.data(), 200);
        }
        check("tiff: a truncated file is an error in words", !Fm::read_image(path("cut.tif"), err) && !err.empty(), err);
    }
}

// ── s007 m2: the document in files ──────────────────────────────────────────
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"

namespace core = klimt::core;
namespace cf   = klimt::core::file;

namespace {
// Two layer trees the same but for the pixel POINTERS: every field, and every
// pixel by value.
bool same_tree(const std::map<std::string, core::Layer>& a, const std::map<std::string, core::Layer>& b, std::string& why) {
    if (a.size() != b.size()) { why = "layer count"; return false; }
    for (const auto& [id, la] : a) {
        const auto it = b.find(id);
        if (it == b.end()) { why = "missing " + id; return false; }
        core::Layer x = la, y = it->second;
        const auto px = x.pixels, py = y.pixels;
        x.pixels = y.pixels = nullptr;
        if (!(x == y)) { why = "fields of " + id; return false; }
        if (!!px != !!py) { why = "pixels present on " + id; return false; }
        if (px && (px->width() != py->width() || !(px->format() == py->format()) || diff(*px, *py) != 0)) {
            why = "pixels of " + id;
            return false;
        }
    }
    return true;
}
}  // namespace

void selftest_files(const Check& check) {
    const auto path = [](const std::string& name) { return (scratch() / name).string(); };

    std::cout << "\n-- file: New --\n";
    {
        cf::NewSpec s;
        check("new: a sane spec passes", cf::check(s).empty(), cf::check(s));
        s.width = 0;
        check("new: a zero size is refused in words", !cf::check(s).empty(), cf::check(s));
        s = {};
        s.width = 400000;
        check("new: past the side limit is refused", !cf::check(s).empty(), "");
        s = {};
        s.format.alpha = false;
        s.background = cf::Background::Transparent;
        check("new: transparent without alpha is refused", !cf::check(s).empty(), cf::check(s));
        s = {};
        s.width = 6600; s.height = 10200; s.format.depth = klimt::pixel::Depth::U16;
        const auto c = cf::make_new(s);
        const auto& px = *c.layers.at("L1").pixels;
        check("new: a 600 dpi white spread is all uniform tiles (a few KB)", px.bytes() < 16 * 1024 && px.get(6599, 10199).c[0] == 1.f,
              std::to_string(px.bytes()) + " bytes");
        s = {};
        s.background = cf::Background::Transparent;
        const auto t = cf::make_new(s);
        check("new: a transparent background holds no tiles", t.layers.at("L1").pixels->tile_count() == 0, "");
        s = {};
        s.format = {klimt::pixel::Depth::U8, klimt::pixel::Mode::CMYK, false};
        const auto k = cf::make_new(s);
        check("new: CMYK white is no ink", k.layers.at("L1").pixels->get(5, 5).c == klimt::pixel::Px{}.c, "");
        core::Document d;
        core::Document::Loading load(d);
        d.replace(k.image, k.layers, k.next_id);
        check("new: the document it makes holds its invariants", d.check().empty() && d.fresh_id() == "L2", d.check());
    }

    std::cout << "\n-- file: the .klimt round trip --\n";
    {
        // The selftest fixture tree, painted: Background uniform, A a gradient,
        // B a DUPLICATE of A's pixels (shared tiles), Top part-transparent.
        core::Document d;
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        core::layers::load_fixture(d);
        cf::Contents c;
        c.image = d.image();
        c.image.dpi = 240;
        c.image.icc = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{1, 2, 3, 250});
        c.layers = d.snapshot();
        const auto f = c.image.format;
        const auto grad = std::make_shared<const klimt::pixel::TileMap>(picture(f, c.image.width, c.image.height));
        c.layers.at("L1").pixels = std::make_shared<const klimt::pixel::TileMap>(
            klimt::pixel::TileMap(f, c.image.width, c.image.height).fill({0, 0, 600, 400}, klimt::pixel::rgba(0.2f, 0.4f, 0.6f)));
        c.layers.at("L4").pixels = grad;
        c.layers.at("L3").pixels = grad;
        c.layers.at("L5").pixels = std::make_shared<const klimt::pixel::TileMap>(
            klimt::pixel::TileMap(f, c.image.width, c.image.height).fill({10, 10, 100, 50}, klimt::pixel::rgba(1, 0, 0, 0.5f)));
        c.layers.at("L5").blend = klimt::pixel::Blend::Multiply;
        c.layers.at("L5").opacity = 37;
        c.layers.at("L4").locks = core::lock::Alpha | core::lock::Position;
        c.layers.at("L3").visible = false;
        c.layers.at("L3").clipped = true;
        c.next_id = 42;
        std::string err;
        const bool saved = cf::save_klimt(path("doc.klimt"), c, err);
        check("klimt: saves", saved, err);
        check("klimt: sniffs as Klimt (mimetype first, stored)", klimt::formats::sniff(path("doc.klimt")) == klimt::formats::Kind::Klimt, "");
        check("klimt: no .partial left behind", !fs::exists(path("doc.klimt.partial")), "");
        const auto back = cf::open(path("doc.klimt"), err);
        std::string why;
        check("klimt: opens", back.has_value(), err);
        if (back) {
            check("klimt: every layer field and every pixel comes back", same_tree(c.layers, back->layers, why), why);
            check("klimt: the image comes back (size, format, dpi, profile)",
                  back->image.width == c.image.width && back->image.format == f && back->image.dpi == 240 &&
                  back->image.icc && *back->image.icc == *c.image.icc, "");
            check("klimt: next id comes back (ids never reused)", back->next_id == 42, std::to_string(back->next_id));
            check("klimt: shared tiles stay shared (written once, read once)",
                  back->layers.at("L3").pixels->tile(1, 0) == back->layers.at("L4").pixels->tile(1, 0), "");
            check("klimt: a uniform tile stays uniform", back->layers.at("L1").pixels->tile(0, 0)->is_uniform(), "");
            check("klimt: kind is Klimt", back->kind == klimt::formats::Kind::Klimt, "");
        }
        // 16-bit and float documents round trip too.
        for (const auto depth : {klimt::pixel::Depth::U16, klimt::pixel::Depth::F32}) {
            cf::NewSpec s;
            s.width = 300; s.height = 280; s.format.depth = depth;
            auto n = cf::make_new(s);
            n.layers.at("L1").pixels = std::make_shared<const klimt::pixel::TileMap>(picture(n.image.format, 300, 280));
            const bool ok = cf::save_klimt(path("deep.klimt"), n, err);
            const auto b = cf::open(path("deep.klimt"), err);
            check(std::string("klimt: ") + (depth == klimt::pixel::Depth::U16 ? "16-bit" : "32-bit float") + " round trips exactly",
                  ok && b && same_tree(n.layers, b->layers, why), why + err);
        }
        // A cancelled save over an existing file leaves the old file whole.
        cf::Contents other = c;
        other.layers.at("L5").name = "Changed";
        klimt::formats::Progress stop;
        stop.cancelled = [] { return true; };
        const bool cancelled = !cf::save_klimt(path("doc.klimt"), other, err, stop);
        const auto still = cf::open(path("doc.klimt"), err);
        check("klimt: a cancelled save leaves the old file as it was",
              cancelled && still && still->layers.at("L5").name == "Top" && !fs::exists(path("doc.klimt.partial")), err);
        // Damage.
        {
            std::ifstream in(path("doc.klimt"), std::ios::binary);
            std::string all((std::istreambuf_iterator<char>(in)), {});
            std::ofstream(path("cut.klimt"), std::ios::binary).write(all.data(), static_cast<std::streamsize>(all.size() / 2));
        }
        check("klimt: a truncated file is refused in words", !cf::open(path("cut.klimt"), err) && !err.empty(), err);
        check("open: garbage is refused in words", !cf::open(path("junk.png"), err) && !err.empty(), err);
    }

    std::cout << "\n-- file: images open as a document; Export --\n";
    {
        klimt::formats::Raster r;
        r.pixels = picture({klimt::pixel::Depth::U16, klimt::pixel::Mode::RGB, true}, 320, 200);
        r.dpi = 300;
        std::string err;
        klimt::formats::write_tiff(path("in.tif"), r, err);
        const auto c = cf::open(path("in.tif"), err);
        check("open: a TIFF is one layer, Background, at its own depth",
              c && c->layers.size() == 1 && c->layers.at("L1").name == "Background" &&
              c->image.format == r.pixels.format() && c->image.dpi == 300 && c->next_id == 2 &&
              c->kind == klimt::formats::Kind::TIFF, err);
        // Export: the composite, as a PNG -- what the compositor shows.
        cf::NewSpec s;
        s.width = 300; s.height = 260;
        auto doc = cf::make_new(s);
        core::Layer top;
        top.name = "Top"; top.pos = 2; top.opacity = 50;
        top.pixels = std::make_shared<const klimt::pixel::TileMap>(
            klimt::pixel::TileMap(s.format, 300, 260).fill({0, 0, 150, 260}, klimt::pixel::rgba(0, 0, 0)));
        doc.layers["L2"] = top;
        const bool wrote = cf::export_image(path("out.png"), klimt::formats::Kind::PNG, doc.image, doc.layers, err);
        const auto back = klimt::formats::read_png(path("out.png"), err);
        std::vector<float> tile;
        core::composite_tile(doc.image, doc.layers, 0, 0, tile);
        check("export: the PNG is the composite", wrote && back && std::abs(back->pixels.get(10, 10).c[0] - tile[(10 * 256 + 10) * 4]) < 1.0 / 255 &&
              std::abs(back->pixels.get(200, 10).c[0] - 1.f) < 1e-6, err);
        cf::NewSpec clear;
        clear.width = 64; clear.height = 64; clear.background = cf::Background::Transparent;
        const auto empty = cf::make_new(clear);
        cf::export_image(path("out.jpg"), klimt::formats::Kind::JPEG, empty.image, empty.layers, err);
        const auto jp = klimt::formats::read_jpeg(path("out.jpg"), err);
        check("export: JPEG shows white where nothing is", jp && jp->pixels.get(30, 30).c[0] > 0.98f, err);
        check("export: Klimt is not an export kind", !cf::export_image(path("x.klimt"), klimt::formats::Kind::Klimt, doc.image, doc.layers, err), err);
    }

    std::cout << "\n-- composite: Gray / CMYK / Lab shown as RGB (unmanaged) --\n";
    {
        const auto one = [](klimt::pixel::Mode m, klimt::pixel::Px px) {
            cf::NewSpec s;
            s.width = 16; s.height = 16; s.format = {klimt::pixel::Depth::U8, m, true};
            s.background = cf::Background::Colour;
            s.colour = px;
            const auto c = cf::make_new(s);
            std::vector<float> t;
            core::composite_tile(c.image, c.layers, 0, 0, t);
            return std::array<float, 3>{t[0], t[1], t[2]};
        };
        const auto g = one(klimt::pixel::Mode::Gray, [] { klimt::pixel::Px p; p.c[0] = 0.5f; return p; }());
        check("composite: gray 50% shows as 50% grey", std::abs(g[0] - 0.502f) < 0.01f && g[0] == g[2], std::to_string(g[0]));
        const auto k = one(klimt::pixel::Mode::CMYK, [] { klimt::pixel::Px p; p.c = {1, 0, 0, 0, 0}; return p; }());
        check("composite: full cyan shows as cyan", k[0] < 0.01f && k[1] > 0.99f && k[2] > 0.99f, "");
        const auto l = one(klimt::pixel::Mode::Lab, [] { klimt::pixel::Px p; p.c = {1, 128.f / 255, 128.f / 255, 0, 0}; return p; }());
        check("composite: Lab white shows as white", l[0] > 0.98f && l[1] > 0.98f && l[2] > 0.98f,
              std::to_string(l[0]) + "," + std::to_string(l[1]) + "," + std::to_string(l[2]));
    }

    std::cout << "\n-- journal: the saved mark (unsaved changes) --\n";
    {
        core::Document d;
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        core::layers::load_fixture(d);
        const auto saved = j.mark();
        core::layers::rename(j, d, "L4", "Renamed");
        check("mark: a step moves it (unsaved)", j.mark() != saved, "");
        j.undo();
        check("mark: undo back to the save matches it (saved)", j.mark() == saved, "");
        j.redo();
        const auto after = j.mark();
        core::layers::set_opacity(j, d, "L4", 50);
        const auto m1 = j.mark();
        core::layers::set_opacity(j, d, "L4", 40);   // merges into the last step
        check("mark: a merged step is a new state", j.mark() != m1 && j.size() == 2, "");
        j.undo();
        check("mark: undo of the merged step goes back past both", j.mark() == after, "");
        j.clear();
        check("mark: clear is a new state", j.mark() != saved && j.mark() != after, "");
    }
    std::error_code ec;
    fs::remove_all(scratch(), ec);
}

// ── s007 m4: dirty tiles ────────────────────────────────────────────────────
void selftest_dirty(const Check& check) {
    std::cout << "\n-- composite: dirty tiles (s007 m4) --\n";
    namespace P = klimt::pixel;
    // The verb table's fixture, painted so its tiles differ: Background
    // uniform; A (clipped to B) a disc over two tiles; B a block in one tile,
    // Multiply; Top a bar in the right column; the group at 80%.
    const auto painted = [](core::Document& d) {
        core::layers::load_fixture(d);
        core::Document::Loading load(d);
        const auto im = d.image();
        const auto set = [&](const char* id, auto&& edit) {
            auto l = *d.get(id);
            edit(l);
            d.put(id, l);
        };
        set("L1", [&](core::Layer& l) { l.pixels = std::make_shared<const P::TileMap>(d.blank().fill({0, 0, im.width, im.height}, P::rgba(0.5f, 0.6f, 0.7f))); });
        set("L4", [&](core::Layer& l) {
            l.clipped = true;
            l.pixels = std::make_shared<const P::TileMap>(d.blank().paint({150, 30, 220, 200}, [](int x, int y, const P::Px& o) {
                const double dx = x - 260.0, dy = y - 130.0;
                return dx * dx + dy * dy < 100.0 * 100.0 ? P::rgba(0.9f, 0.2f, 0.1f, 0.8f) : o;
            }));
        });
        set("L3", [&](core::Layer& l) {
            l.blend = P::Blend::Multiply;
            l.pixels = std::make_shared<const P::TileMap>(d.blank().fill({300, 60, 150, 150}, P::rgba(0.2f, 0.8f, 0.3f, 1)));
        });
        set("L5", [&](core::Layer& l) { l.pixels = std::make_shared<const P::TileMap>(d.blank().fill({520, 0, 80, 400}, P::rgba(1, 1, 0, 0.6f))); });
        set("L2", [&](core::Layer& l) { l.opacity = 80; });
    };
    const auto render = [](const core::Image& im, const core::Layers& ls, int col, int row) {
        std::vector<std::uint32_t> out;
        int w = 0, h = 0;
        core::composite_tile_argb(im, ls, col, row, out, w, h);
        return out;
    };
    bool all_ok = true, any_partial = false;
    std::string bad;
    for (const auto& v : core::layers::verbs()) {
        core::Document d;
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        painted(d);
        if (v.setup) v.setup(d);
        const auto im0 = d.image();
        const auto before = d.snapshot();
        v.run(j, d);
        const auto after = d.snapshot();
        const auto dirty = core::changed_tiles(im0, before, d.image(), after);
        if (!dirty.all && dirty.count() < static_cast<std::size_t>(dirty.cols * dirty.rows)) any_partial = true;
        // THE INVARIANT: a tile not named is a tile that didn't change.
        for (int row = 0; row < dirty.rows; ++row)
            for (int col = 0; col < dirty.cols; ++col)
                if (!dirty.has(col, row) && render(im0, before, col, row) != render(d.image(), after, col, row)) {
                    all_ok = false;
                    bad += v.name + "(" + std::to_string(col) + "," + std::to_string(row) + ") ";
                }
    }
    check("dirty: for every verb, an unnamed tile is an unchanged tile", all_ok, bad);
    check("dirty: and some verbs name fewer than all tiles", any_partial, "");
    {
        core::Document d;
        core::DocJournal j(d);
        d.set_door([&] { return j.open(); });
        painted(d);
        const auto im = d.image();
        auto before = d.snapshot();
        core::layers::rename(j, d, "L4", "Sky");
        core::layers::set_locks(j, d, "L5", core::lock::Pixels);
        check("dirty: a rename and a lock name no tile", core::changed_tiles(im, before, im, d.snapshot()).count() == 0, "");
        before = d.snapshot();
        core::layers::fill(j, d, "L3", {300, 300, 20, 20}, P::rgba(0, 0, 1, 1));   // inside tile (1, 1) only
        const auto t = core::changed_tiles(im, before, im, d.snapshot());
        check("dirty: a fill names only the tiles it touched", t.count() == 1 && t.has(1, 1), std::to_string(t.count()));
        before = d.snapshot();
        core::layers::set_opacity(j, d, "L5", 30);   // Top holds only the right column
        const auto o = core::changed_tiles(im, before, im, d.snapshot());
        check("dirty: an opacity change names the layer's own tiles", o.count() == 2 && o.has(2, 0) && o.has(2, 1), std::to_string(o.count()));
        // Delete the base a clipped layer clips to, without touching the
        // clipped one: A now shows unclipped -- where B never was.
        {
            core::Document e;
            core::DocJournal je(e);
            e.set_door([&] { return je.open(); });
            painted(e);
            const auto b0 = e.snapshot();
            core::layers::remove(je, e, {"L3"});
            const auto t2 = core::changed_tiles(im, b0, im, e.snapshot());
            bool ok = e.get("L4") && e.get("L4")->pos == b0.at("L4").pos;   // A itself untouched
            for (int row = 0; row < t2.rows; ++row)
                for (int col = 0; col < t2.cols; ++col)
                    if (!t2.has(col, row) && render(im, b0, col, row) != render(im, e.snapshot(), col, row)) ok = false;
            check("dirty: deleting a clip base names what its clipped layers now show", ok, "");
        }
        core::Image bigger = im;
        bigger.width += 1;
        check("dirty: a new image size names everything", core::changed_tiles(im, before, bigger, before).all, "");
    }
    {
        // The edge tile is the image's part only: 600 x 400 -> the last
        // column is 88 wide, the last row 144 tall.
        core::Document d;
        painted(d);
        std::vector<std::uint32_t> out;
        int w = 0, h = 0;
        core::composite_tile_argb(d.image(), d.snapshot(), 2, 1, out, w, h);
        check("argb tile: an edge tile is cropped to the image", w == 88 && h == 144 && out.size() == 88u * 144u,
              std::to_string(w) + " x " + std::to_string(h));
    }
}
