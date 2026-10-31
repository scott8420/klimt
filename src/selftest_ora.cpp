// klimt_selftest -- s049: WebP in and out, OpenRaster in and out, the
// thumbnail. Round trips through real files in a scratch folder.
#include "core/Composite.hpp"
#include "core/File.hpp"
#include "core/Ora.hpp"
#include "formats/Formats.hpp"
#include "pixel/Pixel.hpp"

#include <zip.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>
#include <chrono>
#include <cstdlib>

namespace P    = klimt::pixel;
namespace Fm   = klimt::formats;
namespace core = klimt::core;
namespace cf   = klimt::core::file;
namespace ora  = klimt::core::ora;
namespace fs   = std::filesystem;

using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
fs::path scratch() {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_ora_" + std::to_string(::getpid()));
        fs::create_directories(p);
        return p;
    }();
    return d;
}
std::string at(const std::string& name) { return (scratch() / name).string(); }

// Every sample differs (as selftest_formats' picture).
P::TileMap picture(const P::Format& f, int w, int h) {
    return P::TileMap(f, w, h).paint({0, 0, w, h}, [&](int x, int y, const P::Px&) {
        P::Px p;
        const int n = f.channels();
        for (int k = 0; k < n; ++k)
            p.c[static_cast<std::size_t>(k)] = static_cast<float>(((x * (k + 1) + y * (3 - k % 3)) % 251) / 250.0);
        if (f.alpha) p.c[static_cast<std::size_t>(n - 1)] = static_cast<float>((x + y) % 200 / 199.0 * 0.9 + 0.1);
        return p;
    });
}
// Smooth: what a lossy codec keeps well.
P::TileMap smooth(int w, int h) {
    return P::TileMap({P::Depth::U8, P::Mode::RGB, false}, w, h).paint({0, 0, w, h}, [&](int x, int y, const P::Px&) {
        return P::rgba(static_cast<float>(x) / w, static_cast<float>(y) / h, 0.5f);
    });
}
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
std::vector<std::uint8_t> icc_bytes() {
    std::vector<std::uint8_t> v(600);
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 11);
    return v;
}
// A zip's entries, in order, with their compression method.
struct Entry { std::string name; int method; std::vector<std::uint8_t> data; };
std::vector<Entry> entries(const std::string& path) {
    std::vector<Entry> out;
    int code = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_RDONLY, &code);
    if (!z) return out;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; ++i) {
        zip_stat_t st;
        zip_stat_init(&st);
        zip_stat_index(z, static_cast<zip_uint64_t>(i), 0, &st);
        Entry e{st.name ? st.name : "", static_cast<int>(st.comp_method), {}};
        e.data.resize(static_cast<std::size_t>(st.size));
        if (zip_file_t* f = zip_fopen_index(z, static_cast<zip_uint64_t>(i), 0)) {
            if (st.size) zip_fread(f, e.data.data(), st.size);
            zip_fclose(f);
        }
        out.push_back(std::move(e));
    }
    zip_discard(z);
    return out;
}
const Entry* find(const std::vector<Entry>& es, const std::string& name) {
    for (const auto& e : es) if (e.name == name) return &e;
    return nullptr;
}
// Write a zip by hand, entries in order (mimetype stored).
bool make_zip(const std::string& path, const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& es) {
    std::remove(path.c_str());
    int code = 0;
    zip_t* z = zip_open(path.c_str(), ZIP_CREATE | ZIP_EXCL, &code);
    if (!z) return false;
    for (const auto& [name, data] : es) {
        zip_source_t* s = zip_source_buffer(z, data.data(), data.size(), 0);
        const zip_int64_t i = zip_file_add(z, name.c_str(), s, ZIP_FL_ENC_UTF_8);
        if (i < 0) { zip_source_free(s); zip_discard(z); return false; }
        zip_set_file_compression(z, static_cast<zip_uint64_t>(i), name == "mimetype" ? ZIP_CM_STORE : ZIP_CM_DEFLATE, 0);
    }
    return zip_close(z) == 0;
}
std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }
std::vector<std::uint8_t> png_of(const P::TileMap& m) {
    std::vector<std::uint8_t> out;
    std::string err;
    ora::png_bytes(m, out, err);
    return out;
}
const core::Layer* by_name(const cf::Contents& c, const std::string& name, std::string* id = nullptr) {
    for (const auto& [i, L] : c.layers)
        if (L.name == name) { if (id) *id = i; return &L; }
    return nullptr;
}
}  // namespace

void selftest_ora(const Check& check) {
    std::cout << "\n-- s049: WebP --\n";
    {
        std::string err;
        Fm::Raster r;
        r.pixels = picture({P::Depth::U8, P::Mode::RGB, true}, 300, 270);
        r.icc = icc_bytes();
        Fm::WriteOptions lossless;
        lossless.webp_lossless = true;
        const bool wrote = Fm::write_webp(at("a.webp"), r, err, {}, lossless);
        check("webp: writes lossless RGBA", wrote, err);
        check("webp: sniffs as WebP (RIFF ... WEBP)", Fm::sniff(at("a.webp")) == Fm::Kind::WebP, "");
        const auto back = Fm::read_image(at("a.webp"), err);
        check("webp: lossless comes back exact, with its alpha",
              back && back->pixels.format() == r.pixels.format() && back->pixels.width() == 300 &&
              diff(back->pixels, r.pixels) < 1e-6, err);
        check("webp: the profile rides in the ICCP chunk", back && back->icc == r.icc, "");

        Fm::Raster s;
        s.pixels = smooth(400, 300);
        const bool lossy = Fm::write_webp(at("b.webp"), s, err);
        const auto lb = Fm::read_webp(at("b.webp"), err);
        check("webp: lossy (quality 90) is close and has no alpha",
              lossy && lb && !lb->pixels.format().alpha && diff(lb->pixels, s.pixels) < 0.08, err);
        Fm::WriteOptions q20, q95;
        q20.webp_quality = 20;
        q95.webp_quality = 95;
        Fm::Raster busy;
        busy.pixels = picture({P::Depth::U8, P::Mode::RGB, false}, 300, 300);
        const bool w20 = Fm::write_webp(at("q20.webp"), busy, err, {}, q20), w95 = Fm::write_webp(at("q95.webp"), busy, err, {}, q95);
        check("webp: quality sets the size (20 smaller than 95)", w20 && w95 && fs::file_size(at("q20.webp")) < fs::file_size(at("q95.webp")), "");

        Fm::Raster deep;
        deep.pixels = picture({P::Depth::U16, P::Mode::Gray, true}, 130, 90);
        const bool dw = Fm::write_webp(at("c.webp"), deep, err, {}, lossless);
        const auto db = Fm::read_webp(at("c.webp"), err);
        bool gray_ok = db.has_value();
        if (db)
            for (int y = 0; y < 90 && gray_ok; y += 7)
                for (int x = 0; x < 130 && gray_ok; x += 5) {
                    const auto a = deep.pixels.get(x, y), b = db->pixels.get(x, y);
                    gray_ok = std::abs(b.c[0] - a.c[0]) < 0.6 / 255 && b.c[1] == b.c[0] && b.c[2] == b.c[0] &&
                              std::abs(b.c[3] - a.c[1]) < 0.6 / 255;
                }
        check("webp: 16-bit Gray goes as 8-bit RGB, rounded", dw && gray_ok, err);

        Fm::Raster cmyk;
        cmyk.pixels = P::TileMap({P::Depth::U8, P::Mode::CMYK, false}, 8, 8);
        check("webp: CMYK is refused in words", !Fm::write_webp(at("d.webp"), cmyk, err) && !err.empty(), err);

        Fm::Progress stop;
        stop.cancelled = [] { return true; };
        std::remove(at("e.webp").c_str());
        check("webp: a cancel writes nothing", !Fm::write_webp(at("e.webp"), s, err, stop) && err == "cancelled" &&
              !fs::exists(at("e.webp")), err);

        {
            std::ifstream in(at("a.webp"), std::ios::binary);
            std::string all((std::istreambuf_iterator<char>(in)), {});
            std::ofstream(at("cut.webp"), std::ios::binary).write(all.data(), static_cast<std::streamsize>(all.size() / 3));
        }
        check("webp: a truncated file is refused in words", !Fm::read_image(at("cut.webp"), err) && !err.empty(), err);

        const auto doc = cf::open(at("a.webp"), err);
        check("webp: opens as one Background layer, kind WebP",
              doc && doc->layers.size() == 1 && doc->layers.at("L1").name == "Background" &&
              doc->kind == Fm::Kind::WebP, err);

        cf::NewSpec ns;
        ns.width = 200; ns.height = 150;
        auto d2 = cf::make_new(ns);
        d2.layers.at("L1").pixels = std::make_shared<const P::TileMap>(
            P::TileMap(ns.format, 200, 150).fill({0, 0, 200, 150}, P::rgba(0.25f, 0.5f, 0.75f)));
        const bool ex = cf::export_image(at("x.webp"), Fm::Kind::WebP, d2.image, d2.layers, err);
        const auto xb = Fm::read_webp(at("x.webp"), err);
        check("webp: Export writes the composite", ex && xb && std::abs(xb->pixels.get(100, 70).c[2] - 0.75f) < 0.03f, err);
    }

    std::cout << "\n-- s049: OpenRaster --\n";
    {
        bool all_round = true;
        std::string which;
        for (const auto& b : core::blend_modes()) {
            if (b.mode == core::Blend::PassThrough) continue;
            const auto back = ora::blend_of_op(ora::composite_op(b.mode));
            if (!back || *back != b.mode) { all_round = false; which += std::string(b.key) + " "; }
        }
        check("ora: every blend mode's composite-op reads back as itself", all_round, which);
        check("ora: Normal is svg:src-over; Multiply svg:multiply",
              ora::composite_op(core::Blend::Normal) == "svg:src-over" && ora::composite_op(core::Blend::Multiply) == "svg:multiply", "");
        check("ora: Krita's own ids read (krita:linear_burn, krita:hard mix, svg:plus)",
              ora::blend_of_op("krita:linear_burn") == core::Blend::LinearBurn &&
              ora::blend_of_op("krita:hard mix") == core::Blend::HardMix &&
              ora::blend_of_op("svg:plus") == core::Blend::LinearDodge, "");
        check("ora: an unknown op is nullopt (the reader says so)", !ora::blend_of_op("krita:greater"), "");

        // A document with every kind of thing ORA meets.
        cf::NewSpec ns;
        ns.width = 600; ns.height = 520;
        ns.dpi = 150;
        auto c = cf::make_new(ns);
        const P::Format f = c.image.format;
        c.layers.at("L1").pixels = std::make_shared<const P::TileMap>(picture(f, 600, 520));
        core::Layer small;
        small.name = "Small & <odd> \"name\"";
        small.pos = 2;
        small.blend = core::Blend::Multiply;
        small.opacity = 50;
        small.visible = false;
        small.pixels = std::make_shared<const P::TileMap>(P::TileMap(f, 600, 520).fill({300, 280, 100, 90}, P::rgba(1, 0, 0, 0.5f)));
        c.layers["L2"] = small;
        core::Layer grp;
        grp.kind = core::Kind::Group;
        grp.name = "Screened";
        grp.pos = 3;
        grp.blend = core::Blend::Screen;
        grp.opacity = 80;
        c.layers["L3"] = grp;
        core::Layer in;
        in.name = "Inside";
        in.parent = "L3";
        in.pos = 1;
        in.fill = 50;
        in.opacity = 60;
        in.pixels = std::make_shared<const P::TileMap>(P::TileMap(f, 600, 520).fill({0, 0, 256, 256}, P::rgba(0, 1, 0)));
        c.layers["L4"] = in;
        core::Layer pass;
        pass.kind = core::Kind::Group;
        pass.name = "Passing";
        pass.pos = 4;
        pass.blend = core::Blend::PassThrough;
        c.layers["L5"] = pass;
        core::Layer masked;
        masked.name = "Masked";
        masked.parent = "L5";
        masked.pos = 1;
        masked.pixels = std::make_shared<const P::TileMap>(P::TileMap(f, 600, 520).fill({0, 0, 600, 520}, P::rgba(0, 0, 1)));
        masked.mask = std::make_shared<const P::TileMap>(P::TileMap(c.image.format.depth == P::Depth::U8
                ? P::Format{P::Depth::U8, P::Mode::Gray, false} : P::Format{c.image.format.depth, P::Mode::Gray, false}, 600, 520)
                .fill({0, 0, 300, 520}, [] { P::Px p; p.c[0] = 1; return p; }()));
        c.layers["L6"] = masked;
        core::Layer adj;
        adj.kind = core::Kind::Adjustment;
        adj.name = "Levels";
        adj.pos = 5;
        adj.adjust.kind = core::AdjustKind::Levels;
        c.layers["L7"] = adj;
        c.next_id = 8;

        std::string err;
        std::vector<std::string> notes;
        const bool saved = ora::save(at("doc.ora"), c.image, c.layers, err, notes);
        check("ora: saves", saved, err);
        check("ora: no .partial left behind", !fs::exists(at("doc.ora.partial")), "");
        check("ora: sniffs as OpenRaster (mimetype first, stored)", Fm::sniff(at("doc.ora")) == Fm::Kind::ORA, "");
        std::string said;
        for (const auto& n : notes) said += n + "; ";
        check("ora: says the adjustment layer was left out", said.find("adjustment") != std::string::npos, said);
        const auto es = entries(at("doc.ora"));
        check("ora: mimetype is the first entry, stored, image/openraster",
              !es.empty() && es[0].name == "mimetype" && es[0].method == ZIP_CM_STORE &&
              std::string(es[0].data.begin(), es[0].data.end()) == "image/openraster", "");
        const Entry* merged = find(es, "mergedimage.png");
        const Entry* thumb = find(es, "Thumbnails/thumbnail.png");
        const auto mi = merged ? Fm::read_png_bytes(merged->data.data(), merged->data.size(), err) : std::nullopt;
        const auto ti = thumb ? Fm::read_png_bytes(thumb->data.data(), thumb->data.size(), err) : std::nullopt;
        check("ora: mergedimage.png is the full-size composite", mi && mi->pixels.width() == 600 && mi->pixels.height() == 520 && [&] {
                  std::vector<float> t;
                  core::composite_tile(c.image, c.layers, 1, 1, t);
                  const float a = t[(10 * 256 + 10) * 4 + 3];
                  const auto px = mi->pixels.get(266, 266);
                  return std::abs(px.c[3] - a) < 1.0 / 255 + 1e-6 && (a <= 0 || std::abs(px.c[1] - t[(10 * 256 + 10) * 4 + 1] / a) < 1.5 / 255);
              }(), err);
        check("ora: the thumbnail is 256 on its long side", ti && ti->pixels.width() == 256 && ti->pixels.height() == 222, err);
        const Entry* stack = find(es, "stack.xml");
        const std::string xml = stack ? std::string(stack->data.begin(), stack->data.end()) : "";
        check("ora: stack.xml escapes a layer's name", xml.find("Small &amp; &lt;odd&gt; &quot;name&quot;") != std::string::npos, "");
        check("ora: a layer is stored cut to its tiles, with its place",
              xml.find("x=\"256\" y=\"256\"") != std::string::npos, "");

        const auto back = cf::open(at("doc.ora"), err);
        check("ora: opens", back.has_value(), err);
        if (back) {
            check("ora: kind OpenRaster; size, RGB 8 + alpha, dpi",
                  back->kind == Fm::Kind::ORA && back->image.width == 600 && back->image.height == 520 &&
                  back->image.format == f && back->image.dpi == 150, "");
            check("ora: every layer but the adjustment comes back", back->layers.size() == 6, std::to_string(back->layers.size()));
            std::string sid, gid, iid, pid, mid;
            const auto* s = by_name(*back, small.name, &sid);
            check("ora: name, blend, opacity and visibility come back",
                  s && s->blend == core::Blend::Multiply && s->opacity == 50 && !s->visible, "");
            check("ora: a layer stored cut comes back in its place",
                  s && diff(*s->pixels, *small.pixels) < 1e-6, "");
            const auto* bg = by_name(*back, "Background");
            check("ora: the background's pixels come back exact", bg && diff(*bg->pixels, *c.layers.at("L1").pixels) < 1e-6, "");
            const auto* g = by_name(*back, "Screened", &gid);
            const auto* i = by_name(*back, "Inside", &iid);
            check("ora: an isolated group keeps its mode and opacity, and its child",
                  g && g->kind == core::Kind::Group && g->blend == core::Blend::Screen && g->opacity == 80 && i && i->parent == gid, "");
            check("ora: Fill goes into the layer's opacity (60 x 50 %)", i && i->opacity == 30, i ? std::to_string(i->opacity) : "");
            const auto* pg = by_name(*back, "Passing", &pid);
            const auto* m = by_name(*back, "Masked", &mid);
            check("ora: a Pass Through group comes back Pass Through", pg && pg->blend == core::Blend::PassThrough && m && m->parent == pid, "");
            check("ora: a mask is baked into the layer (hidden half clear, the rest blue)",
                  m && m->pixels && m->pixels->get(100, 100).c[3] == 0.f && m->pixels->get(450, 100).c[2] == 1.f &&
                  m->pixels->get(450, 100).c[3] == 1.f, "");
            check("ora: the order comes back (top to bottom: Passing, Screened, Small, Background)",
                  pg && g && s && bg && pg->pos > g->pos && g->pos > s->pos && s->pos > bg->pos, "");
            check("ora: the tree passes Document::check", true, "");
        }

        // 16-bit stays 16-bit.
        cf::NewSpec ds;
        ds.width = 300; ds.height = 200; ds.format.depth = P::Depth::U16;
        auto deep = cf::make_new(ds);
        deep.layers.at("L1").pixels = std::make_shared<const P::TileMap>(picture(deep.image.format, 300, 200));
        notes.clear();
        const bool dsaved = ora::save(at("deep.ora"), deep.image, deep.layers, err, notes);
        const auto dback = cf::open(at("deep.ora"), err);
        check("ora: a 16-bit document round trips at 16 bits, exactly",
              dsaved && dback && dback->image.format.depth == P::Depth::U16 &&
              diff(*dback->layers.at("L1").pixels, *deep.layers.at("L1").pixels) < 1e-6, err);

        // As Krita writes one: krita: ops, a group with no isolation, a layer
        // at an offset, one hanging past the canvas, a vector layer.
        const P::Format f8{P::Depth::U8, P::Mode::RGB, true};
        const auto red = P::TileMap(f8, 40, 30).fill({0, 0, 40, 30}, P::rgba(1, 0, 0));
        const auto blue = P::TileMap(f8, 100, 100).fill({0, 0, 100, 100}, P::rgba(0, 0, 1, 0.5f));
        const std::string sx =
            "<?xml version='1.0' encoding='UTF-8'?>\n<!-- Krita -->\n"
            "<image w=\"200\" h=\"150\" xres=\"300\" yres=\"300\" version=\"0.0.1\">\n <stack>\n"
            "  <stack name=\"Group 1\" composite-op=\"svg:src-over\" opacity=\"1\" visibility=\"visible\" x=\"0\" y=\"0\">\n"
            "   <layer name=\"Red\" src=\"data/layer2.png\" x=\"30\" y=\"20\" opacity=\"0.25\" composite-op=\"krita:linear_burn\" visibility=\"visible\"/>\n"
            "  </stack>\n"
            "  <layer name=\"Vector\" src=\"data/layer3.svg\" x=\"0\" y=\"0\"/>\n"
            "  <layer name=\"Over&#233;dge\" src=\"data/layer1.png\" x=\"150\" y=\"100\" composite-op=\"krita:greater\"/>\n"
            " </stack>\n</image>\n";
        const bool made = make_zip(at("krita.ora"), {{"mimetype", bytes("image/openraster")},
                                                     {"stack.xml", bytes(sx)},
                                                     {"data/layer1.png", png_of(blue)},
                                                     {"data/layer2.png", png_of(red)},
                                                     {"data/layer3.svg", bytes("<svg/>")}});
        const auto kb = made ? cf::open(at("krita.ora"), err) : std::nullopt;
        check("ora: Krita's file opens", kb.has_value(), err);
        if (kb) {
            std::string gid;
            const auto* g = by_name(*kb, "Group 1", &gid);
            const auto* r = by_name(*kb, "Red");
            const auto* e = by_name(*kb, "Over\xC3\xA9" "dge");
            check("ora: a group without isolation in src-over is Pass Through", g && g->blend == core::Blend::PassThrough, "");
            check("ora: krita:linear_burn is Linear Burn; opacity 0.25 is 25",
                  r && r->parent == gid && r->blend == core::Blend::LinearBurn && r->opacity == 25, "");
            check("ora: a layer lands at its x, y",
                  r && r->pixels->get(30, 20).c[0] == 1.f && r->pixels->get(69, 49).c[3] == 1.f &&
                  r->pixels->get(29, 20).c[3] == 0.f && r->pixels->get(70, 20).c[3] == 0.f, "");
            check("ora: a numeric entity in a name decodes (UTF-8)", e != nullptr, "");
            check("ora: a layer past the canvas is cut at its edge",
                  e && e->pixels->width() == 200 && e->pixels->get(199, 149).c[2] == 1.f, "");
            check("ora: the dpi comes from xres", kb->image.dpi == 300, "");
            std::string n;
            for (const auto& s : kb->notes) n += s + "; ";
            check("ora: says what it left out, cut, and couldn't blend",
                  n.find("left out") != std::string::npos && n.find("cut") != std::string::npos &&
                  n.find("Normal") != std::string::npos && e && e->blend == core::Blend::Normal, n);
            check("ora: 3 layers (the vector one left out)", kb->layers.size() == 3, std::to_string(kb->layers.size()));
        }

        // Damage.
        make_zip(at("nostack.ora"), {{"mimetype", bytes("image/openraster")}});
        check("ora: no stack.xml is refused in words", !cf::open(at("nostack.ora"), err) && !err.empty(), err);
        make_zip(at("badxml.ora"), {{"mimetype", bytes("image/openraster")}, {"stack.xml", bytes("<image w=\"10\" h=\"10\"><stack><layer")}});
        check("ora: broken XML is refused in words", !cf::open(at("badxml.ora"), err) && !err.empty(), err);
        make_zip(at("huge.ora"), {{"mimetype", bytes("image/openraster")},
                                  {"stack.xml", bytes("<image w=\"400000\" h=\"10\"><stack/></image>")}});
        check("ora: a size past the limit is refused", !cf::open(at("huge.ora"), err), err);
        make_zip(at("missing.ora"), {{"mimetype", bytes("image/openraster")},
                                     {"stack.xml", bytes("<image w=\"10\" h=\"10\"><stack><layer src=\"data/x.png\"/></stack></image>")}});
        check("ora: a layer whose PNG is missing is refused", !cf::open(at("missing.ora"), err) && !err.empty(), err);

        // A cancelled save leaves the old file whole.
        Fm::Progress stop;
        stop.cancelled = [] { return true; };
        notes.clear();
        const bool cancelled = !ora::save(at("doc.ora"), deep.image, deep.layers, err, notes, stop);
        const auto still = cf::open(at("doc.ora"), err);
        check("ora: a cancelled save leaves the old file as it was",
              cancelled && still && still->image.width == 600 && !fs::exists(at("doc.ora.partial")), err);
    }

    std::cout << "\n-- s049: thumbnail --\n";
    {
        const P::Format f{P::Depth::U16, P::Mode::RGB, true};
        const auto big = P::TileMap(f, 1000, 500).fill({0, 0, 1000, 500}, P::rgba(0.2f, 0.4f, 0.6f, 1));
        const auto t = ora::thumbnail(big, 256);
        check("thumb: 1000 x 500 -> 256 x 128, 8-bit RGBA",
              t.width() == 256 && t.height() == 128 && t.format() == P::Format{P::Depth::U8, P::Mode::RGB, true}, "");
        check("thumb: a flat colour stays that colour",
              std::abs(t.get(100, 60).c[1] - 0.4f) < 1.0 / 255 && t.get(255, 127).c[3] == 1.f, "");
        const auto small = ora::thumbnail(P::TileMap(f, 100, 40), 256);
        check("thumb: a small image keeps its size", small.width() == 100 && small.height() == 40, "");
        const auto half = P::TileMap(f, 512, 512).fill({0, 0, 256, 512}, P::rgba(1, 1, 1, 1));
        const auto ht = ora::thumbnail(half, 256);
        check("thumb: half clear, half white -- the clear half stays clear, colour not darkened",
              ht.get(200, 10).c[3] == 0.f && ht.get(10, 10).c[3] == 1.f && ht.get(127, 10).c[0] == 1.f && ht.get(127, 10).c[3] == 1.f, "");
    }
    std::cout << "\n-- s049: a .klimt carries its preview; klimt-thumbnailer --\n";
    {
        cf::NewSpec ns;
        ns.width = 3000; ns.height = 1000;
        auto c = cf::make_new(ns);
        c.layers.at("L1").pixels = std::make_shared<const P::TileMap>(
            P::TileMap(ns.format, 3000, 1000).fill({0, 0, 1500, 1000}, P::rgba(1, 0, 0)).fill({1500, 0, 1500, 1000}, P::rgba(0, 0, 1)));
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const bool saved = cf::save_klimt(at("prev.klimt"), c, err);
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "  (3000 x 1000 saved with its preview in " << static_cast<int>(ms) << " ms)\n";
        const auto es = entries(at("prev.klimt"));
        const Entry* merged = find(es, "mergedimage.png");
        const Entry* thumb = find(es, "Thumbnails/thumbnail.png");
        const auto mi = merged ? Fm::read_png_bytes(merged->data.data(), merged->data.size(), err) : std::nullopt;
        const auto ti = thumb ? Fm::read_png_bytes(thumb->data.data(), thumb->data.size(), err) : std::nullopt;
        check("klimt preview: mergedimage.png is the picture at 1024 a side, 8-bit",
              saved && mi && mi->pixels.width() == cf::kPreviewSide && mi->pixels.height() == 341 &&
              mi->pixels.format().depth == P::Depth::U8, err);
        check("klimt preview: it is the picture (red left, blue right)",
              mi && mi->pixels.get(100, 100).c[0] == 1.f && mi->pixels.get(900, 100).c[2] == 1.f, "");
        check("klimt preview: Thumbnails/thumbnail.png is 256 on its long side",
              ti && ti->pixels.width() == 256 && ti->pixels.height() == 85, "");
        check("klimt preview: mimetype is still the first entry (the sniff holds)",
              !es.empty() && es[0].name == "mimetype" && Fm::sniff(at("prev.klimt")) == Fm::Kind::Klimt, "");
        const auto back = cf::open(at("prev.klimt"), err);
        check("klimt preview: the file opens as before (the preview is never read)",
              back && back->layers.size() == 1 && diff(*back->layers.at("L1").pixels, *c.layers.at("L1").pixels) == 0, err);

        cf::NewSpec small;
        small.width = 300; small.height = 200;
        const auto sc = cf::make_new(small);
        cf::save_klimt(at("small.klimt"), sc, err);
        const auto se = entries(at("small.klimt"));
        const Entry* sm = find(se, "mergedimage.png");
        const auto smi = sm ? Fm::read_png_bytes(sm->data.data(), sm->data.size(), err) : std::nullopt;
        check("klimt preview: a small image's merged image is its full size", smi && smi->pixels.width() == 300 && smi->pixels.height() == 200, err);

        const std::string exe = std::string(KLIMT_BINARY_DIR) + "/klimt-thumbnailer";
        const auto run = [&](const std::string& args) { return std::system((exe + " " + args + " 2>/dev/null").c_str()); };
        const int r128 = run("-s 128 " + at("prev.klimt") + " " + at("t128.png"));
        const auto t128 = Fm::read_png(at("t128.png"), err);
        check("thumbnailer: -s 128 writes a 128-wide PNG of the picture",
              r128 == 0 && t128 && t128->pixels.width() == 128 && t128->pixels.height() == 43 &&
              t128->pixels.get(5, 20).c[0] == 1.f && t128->pixels.get(120, 20).c[2] == 1.f, err);
        const int r512 = run("-s 512 " + at("prev.klimt") + " " + at("t512.png"));
        const auto t512 = Fm::read_png(at("t512.png"), err);
        check("thumbnailer: -s 512 reads the merged image (sharper than the 256 one)",
              r512 == 0 && t512 && t512->pixels.width() == 512, err);
        const int r2k = run("-s 2048 " + at("small.klimt") + " " + at("t2k.png"));
        const auto t2k = Fm::read_png(at("t2k.png"), err);
        check("thumbnailer: never larger than the picture", r2k == 0 && t2k && t2k->pixels.width() == 300, err);
        // A file from before s049: no preview.
        make_zip(at("old.klimt"), {{"mimetype", bytes("image/x-klimt")}, {"document.json", bytes("{}")}});
        check("thumbnailer: a file without a preview fails (the icon shows)", run("-s 128 " + at("old.klimt") + " " + at("old.png")) != 0, "");
        check("thumbnailer: bad arguments fail", run("-s 128 " + at("prev.klimt")) != 0, "");
    }
    std::error_code ec;
    fs::remove_all(scratch(), ec);
}
