// klimt_selftest -- s080: the CLIPPING PATH and EPS. The mark (one step, the
// .klimt keeps it, a deleted path layer leaves it inert); Photoshop's path
// resources (8.24 records, names, 2999) out and back through TIFF and PSD;
// the EPS writer's pieces (run-length, ASCII85, the clip as PostScript, the
// preview TIFF) and whole files read back -- the image data decoded to the
// composite's own numbers. Rules only: nothing here depends on the machine.
#include "core/Clip.hpp"
#include "core/Document.hpp"
#include "core/Eps.hpp"
#include "core/File.hpp"
#include "core/Path.hpp"
#include "core/Paths.hpp"
#include "core/Psd.hpp"
#include "formats/Formats.hpp"
#include "pixel/Pixel.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace clip = klimt::core::clip;
namespace eps = klimt::core::eps;
namespace path = klimt::core::path;
namespace paths = klimt::core::paths;
namespace fs = std::filesystem;
namespace formats = klimt::formats;
using Check = std::function<void(const std::string&, bool, const std::string&)>;
using Bytes = std::vector<std::uint8_t>;

namespace {

fs::path scratch() {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_clip_" + std::to_string(::getpid()));
        fs::create_directories(p);
        return p;
    }();
    return d;
}
std::string at(const std::string& n) { return (scratch() / n).string(); }
std::string slurp(const std::string& f) {
    std::ifstream in(f, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
std::uint32_t le32(const std::string& s, std::size_t o) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[o])) | static_cast<std::uint32_t>(static_cast<unsigned char>(s[o + 1])) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(s[o + 2])) << 16 | static_cast<std::uint32_t>(static_cast<unsigned char>(s[o + 3])) << 24;
}
std::uint32_t be32(const Bytes& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) << 24 | static_cast<std::uint32_t>(b[o + 1]) << 16 | static_cast<std::uint32_t>(b[o + 2]) << 8 | b[o + 3];
}
unsigned be16(const Bytes& b, std::size_t o) { return static_cast<unsigned>(b[o]) << 8 | b[o + 1]; }

struct Doc {
    core::Document d;
    core::DocJournal j{d};
    Doc() { d.set_door([this] { return j.open(); }); L::load_fixture(d); }
};

bool same_path(const path::Path& a, const path::Path& b, double eps) {
    if (a.subs.size() != b.subs.size()) return false;
    for (std::size_t s = 0; s < a.subs.size(); ++s) {
        const auto& x = a.subs[s];
        const auto& y = b.subs[s];
        if (x.closed != y.closed || x.nodes.size() != y.nodes.size()) return false;
        for (std::size_t i = 0; i < x.nodes.size(); ++i) {
            const auto& p = x.nodes[i];
            const auto& q = y.nodes[i];
            const auto near = [&](path::P u, path::P v) { return std::abs(u.x - v.x) <= eps && std::abs(u.y - v.y) <= eps; };
            if (!near(p.at, q.at) || !near(p.in, q.in) || !near(p.out, q.out) || p.smooth != q.smooth) return false;
        }
    }
    return true;
}

// An open subpath with a corner and a smooth point, and a closed ellipse.
path::Path two_subs() {
    path::Path p = paths::ellipse_path(300, 200, 150, 90);
    path::Sub open;
    open.closed = false;
    open.nodes.push_back(path::Node::corner({20.5, 30.25}));
    path::Node sm{{120, 60}, {100, 40}, {140, 80}, true};
    open.nodes.push_back(sm);
    open.nodes.push_back(path::Node::corner({200, 20}));
    p.subs.push_back(open);
    return p;
}

// The PostScript part of an EPS file (after a DOS header, if any).
std::string ps_of(const std::string& f) {
    if (f.size() >= 30 && static_cast<unsigned char>(f[0]) == 0xC5) return f.substr(le32(f, 4), le32(f, 8));
    return f;
}
// The image's encoded data: between the procedure's "} exec\n" and "~>".
Bytes image_data(const std::string& ps, bool flate, std::size_t expect) {
    const auto a = ps.find("} exec\n");
    const auto b = ps.find("~>", a);
    if (a == std::string::npos || b == std::string::npos) return {};
    const Bytes packed = eps::un_ascii85(ps.substr(a + 7, b + 2 - (a + 7)));
    if (!flate) return eps::un_run_length(packed);
    Bytes out(expect);
    uLongf n = expect;
    if (uncompress(out.data(), &n, packed.data(), static_cast<uLong>(packed.size())) != Z_OK) return {};
    out.resize(n);
    return out;
}

}  // namespace

void selftest_clip(const Check& check) {
    // ── m1: the mark ────────────────────────────────────────────────────────
    {
        Doc t;
        L::load_path_layer(t.d, paths::rect_path(40, 30, 300, 200));
        check("clip: none to begin with", clip::of(t.d).empty(), "");
        const bool ok = clip::set(t.j, t.d, "L6", 2.04);
        check("clip: a path layer marked -- ONE step \"Clipping Path\"", ok && clip::of(t.d) == "L6" && t.j.undo_label() == "Clipping Path",
              t.j.undo_label());
        check("clip: the flatness kept, to a tenth", std::abs(t.d.image().clip_flatness - 2.0) < 1e-9, std::to_string(t.d.image().clip_flatness));
        check("clip: the same again changes nothing", !clip::set(t.j, t.d, "L6", 2.0), "");
        check("clip: the flatness capped at 100", clip::set(t.j, t.d, "L6", 150) && t.d.image().clip_flatness == 100, "");
        check("clip: a pixel layer refused", !clip::set(t.j, t.d, "L4", 1) && clip::of(t.d) == "L6", "");
        check("clip: a layer that isn't there refused", !clip::set(t.j, t.d, "L99", 1), "");
        t.j.undo();
        t.j.undo();
        check("clip: undo takes the mark off", clip::of(t.d).empty() && t.d.image().clip_path.empty(), "");
        t.j.redo();
        check("clip: redo puts it back", clip::of(t.d) == "L6", "");
        // A deleted path layer: the mark inert; its undo brings it back.
        L::remove(t.j, t.d, {"L6"});
        check("clip: the path layer deleted -- no clipping path (the mark inert)", clip::of(t.d).empty() && t.d.image().clip_path == "L6", "");
        t.j.undo();
        check("clip: the delete undone -- the clipping path is back", clip::of(t.d) == "L6", "");
        check("clip: None clears it -- \"No Clipping Path\"", clip::set(t.j, t.d, "", 0) && clip::of(t.d).empty() &&
              t.j.undo_label() == "No Clipping Path", t.j.undo_label());
        // An empty path can't be the clip.
        Doc e;
        L::load_path_layer(e.d, path::Path{});
        check("clip: an empty path layer refused", !clip::set(e.j, e.d, "L6", 0), "");
    }
    {   // The .klimt keeps it; a mark on a layer not in the file isn't written.
        Doc t;
        L::load_path_layer(t.d, paths::ellipse_path(200, 150, 120, 90));
        clip::set(t.j, t.d, "L6", 3.5);
        core::file::Contents c;
        c.image = t.d.image();
        c.layers = t.d.snapshot();
        c.next_id = 7;
        std::string err;
        const bool saved = core::file::save_klimt(at("clip.klimt"), c, err);
        auto back = saved ? core::file::open(at("clip.klimt"), err) : std::nullopt;
        check("clip: the .klimt keeps the clipping path and its flatness",
              back && back->image.clip_path == "L6" && std::abs(back->image.clip_flatness - 3.5) < 1e-9, err);
        c.layers.erase("L6");
        auto again = core::file::save_klimt(at("clip2.klimt"), c, err) ? core::file::open(at("clip2.klimt"), err) : std::nullopt;
        check("clip: a mark whose layer is gone isn't written", again && again->image.clip_path.empty(), err);
    }

    // ── m2: Photoshop's path resources ──────────────────────────────────────
    {
        const auto p = two_subs();
        const Bytes b = clip::encode(p, 600, 400);
        check("psd paths: 26-byte records", !b.empty() && b.size() % 26 == 0, std::to_string(b.size()));
        check("psd paths: a fill-rule record first, the initial fill rule second", be16(b, 0) == 6 && be16(b, 26) == 8, "");
        check("psd paths: a closed subpath's length record (0) counts its knots", be16(b, 52) == 0 && be16(b, 54) == 4, "");
        check("psd paths: the ellipse's knots are linked (1)", be16(b, 78) == 1, std::to_string(be16(b, 78)));
        const std::size_t open_at = 52 + 26 * 5;
        check("psd paths: the open subpath's length record (3), its corner (5), its smooth knot (4)",
              be16(b, open_at) == 3 && be16(b, open_at + 2) == 3 && be16(b, open_at + 26) == 5 && be16(b, open_at + 52) == 4, "");
        // 8.24 fractions of the side, vertical first: the anchor (20.5, 30.25) of 600 x 400.
        const std::size_t corner = open_at + 26;
        const auto v = be32(b, corner + 10), h = be32(b, corner + 14);
        check("psd paths: points as 8.24 fractions -- vertical, then horizontal",
              v == static_cast<std::uint32_t>(std::lround(30.25 / 400 * 16777216.0)) && h == static_cast<std::uint32_t>(std::lround(20.5 / 600 * 16777216.0)),
              std::to_string(v) + " " + std::to_string(h));
        const auto back = clip::decode(b.data(), b.size(), 600, 400);
        check("psd paths: decoded -- the same anchors, handles, corners and ends (1/1000 px)", back && same_path(*back, p, 1e-3), "");
        check("psd paths: not a multiple of 26 -- not a path", !clip::decode(b.data(), b.size() - 3, 600, 400), "");
    }
    {   // resources(): every path layer, names made unique, 2999 naming the clip.
        Doc t;
        L::add_path_layer(t.j, t.d, paths::rect_path(10, 10, 100, 100));
        const auto second = L::add_path_layer(t.j, t.d, paths::ellipse_path(300, 200, 100, 80));
        {   // the same name twice
            auto l = *t.d.get(second);
            l.name = "Path 1";
            core::Document::Loading load(t.d);
            t.d.put(second, l);
        }
        clip::set(t.j, t.d, second, 2.5);
        const Bytes r = clip::resources(t.d.image(), t.d.snapshot());
        const auto parsed = clip::parse(r.data(), r.size(), 600, 400);
        check("psd paths: both path layers written", parsed.paths.size() == 2, std::to_string(parsed.paths.size()));
        check("psd paths: names made unique", parsed.paths.size() == 2 && parsed.paths[0].name != parsed.paths[1].name,
              parsed.paths.size() == 2 ? parsed.paths[0].name + " / " + parsed.paths[1].name : "");
        check("psd paths: 2999 names the clipping path, its flatness kept",
              !parsed.clip.empty() && std::abs(parsed.flatness - 2.5) < 1e-3 &&
              std::any_of(parsed.paths.begin(), parsed.paths.end(), [&](const clip::Named& n) {
                  return n.name == parsed.clip && same_path(n.path, paths::ellipse_path(300, 200, 100, 80), 1e-3); }),
              parsed.clip);
        check("psd paths: no path layer -- no resources", clip::resources(Doc().d.image(), Doc().d.snapshot()).empty(), "");
        Bytes cut(r.begin(), r.begin() + static_cast<long>(r.size() / 2));
        check("psd paths: a truncated block ends the walk, what came before stands",
              clip::parse(cut.data(), cut.size(), 600, 400).paths.size() <= 1, "");

        // TIFF: exported and opened again -- path layers, the clip marked.
        std::string err;
        const bool ok = core::file::export_image(at("clip.tif"), formats::Kind::TIFF, t.d.image(), t.d.snapshot(), err);
        auto back = ok ? core::file::open(at("clip.tif"), err) : std::nullopt;
        int npaths = 0;
        std::string clip_name;
        if (back) for (const auto& [id, l] : back->layers) if (l.kind == core::Kind::Path) ++npaths;
        if (back && !back->image.clip_path.empty()) clip_name = back->layers.at(back->image.clip_path).name;
        check("tiff: the paths ride in tag 34377 and come back as path layers", npaths == 2, err + " " + std::to_string(npaths));
        check("tiff: the clipping path marked again, its outline the same",
              back && !clip_name.empty() && same_path(*back->layers.at(back->image.clip_path).path, paths::ellipse_path(300, 200, 100, 80), 1e-3) &&
              std::abs(back->image.clip_flatness - 2.5) < 1e-9, clip_name);
        check("tiff: the opened tree holds (path layers above the picture)",
              back && [&] { core::Document d; core::Document::Loading l(d); d.replace(back->image, back->layers, back->next_id); return d.check().empty(); }(), "");
        // PSD: the same.
        std::vector<std::string> notes;
        const bool pok = core::psd::save(at("clip.psd"), t.d.image(), t.d.snapshot(), err, notes);
        auto pback = pok ? core::psd::open(at("clip.psd"), err) : std::nullopt;
        int pp = 0;
        if (pback) for (const auto& [id, l] : pback->layers) if (l.kind == core::Kind::Path) ++pp;
        check("psd: the paths written as resources and read back as path layers", pp == 2, err);
        check("psd: the clipping path marked again", pback && !core::clip::of(pback->image, pback->layers).empty(), "");
        const auto pl = core::psd::plan(t.d.image(), t.d.snapshot());
        check("psd: the plan says the paths go as Photoshop paths, the clip marked",
              std::any_of(pl.kept.begin(), pl.kept.end(), [](const std::string& s) { return s.find("Photoshop paths (the clipping path marked)") != std::string::npos; }),
              pl.kept.empty() ? "" : pl.kept.back());
    }

    // ── m3: EPS, the pieces ─────────────────────────────────────────────────
    {
        std::mt19937 rng(80);
        bool rl_ok = true, a85_ok = true, lines_ok = true;
        std::string why;
        for (int round = 0; round < 60; ++round) {
            Bytes in(static_cast<std::size_t>(rng() % 700));
            for (std::size_t i = 0; i < in.size();) {   // runs and noise
                const std::size_t k = std::min<std::size_t>(in.size() - i, 1 + rng() % 300);
                const bool run = rng() % 2;
                const auto v = static_cast<std::uint8_t>(rng());
                for (std::size_t q = 0; q < k; ++q) in[i + q] = run ? v : static_cast<std::uint8_t>(rng());
                i += k;
            }
            if (round == 1) in.assign(13, 0);   // zeros: 'z'
            const Bytes rl = eps::run_length(in);
            if (eps::un_run_length(rl) != in || rl.back() != 128 || rl.size() > in.size() + (in.size() + 127) / 128 + 1) rl_ok = false;
            const std::string a = eps::ascii85(in);
            if (eps::un_ascii85(a) != in || a.find("~>") == std::string::npos) {
                a85_ok = false;
                why = "round " + std::to_string(round) + ": " + std::to_string(in.size()) + " -> " + std::to_string(eps::un_ascii85(a).size());
            }
            std::size_t s = 0;
            while (s < a.size()) {
                const auto e = a.find('\n', s);
                const std::string line = a.substr(s, e == std::string::npos ? std::string::npos : e - s);
                if (line.size() > 77 || (!line.empty() && line[0] == '%')) lines_ok = false;
                if (e == std::string::npos) break;
                s = e + 1;
            }
        }
        check("eps: run-length encodes and decodes back, its end byte 128, never much longer", rl_ok, "");
        check("eps: ASCII85 encodes and decodes back (every tail length, 'z' for zeros)", a85_ok, why);
        check("eps: ASCII85 lines short, none starting with %", lines_ok, "");
        check("eps: four zeros are 'z'", eps::ascii85(Bytes(4, 0)).rfind("z~>", 0) == 0, eps::ascii85(Bytes(4, 0)));
        const std::string ps = eps::clip_ps(paths::rect_path(40, 30, 300, 200), 400, 72);
        check("eps: the clip in points, y up -- (40, 30) of a 400 px tall picture is 40 370",
              ps.find("40 370 moveto") != std::string::npos && ps.find("300 370 lineto") != std::string::npos &&
              ps.find("closepath") != std::string::npos && ps.find("eoclip") != std::string::npos, ps);
        const std::string ps2 = eps::clip_ps(paths::rect_path(40, 30, 300, 200), 400, 144);
        check("eps: at 144 dpi the points halve", ps2.find("20 185 moveto") != std::string::npos, ps2);
        check("eps: curves as curveto", eps::clip_ps(paths::ellipse_path(100, 100, 50, 40), 200, 72).find("curveto") != std::string::npos, "");
        // The preview TIFF reads back.
        Bytes rgb(5 * 3 * 3, 0);
        for (std::size_t i = 0; i < rgb.size(); i += 3) rgb[i] = 200;
        const Bytes tf = eps::tiff_rgb(rgb, 5, 3, 72);
        { std::ofstream o(at("preview.tif"), std::ios::binary); o.write(reinterpret_cast<const char*>(tf.data()), static_cast<std::streamsize>(tf.size())); }
        std::string err;
        const auto r = formats::read_tiff(at("preview.tif"), err);
        check("eps: the preview TIFF reads back, its size and colour", r && r->pixels.width() == 5 && r->pixels.height() == 3 &&
              std::abs(r->pixels.get(2, 1).c[0] - 200 / 255.f) < 0.01f && r->pixels.get(2, 1).c[1] < 0.01f, err);
    }

    // ── m3: EPS, whole files ────────────────────────────────────────────────
    {
        Doc t;
        {
            core::Document::Loading load(t.d);
            auto im = t.d.image();
            im.dpi = 144;
            t.d.set_image(im);
        }
        L::load_path_layer(t.d, paths::rect_path(100, 100, 300, 250));
        clip::set(t.j, t.d, "L6", 2);
        const auto im = t.d.image();
        const auto ls = t.d.snapshot();
        std::string err;
        std::vector<std::string> notes;
        formats::WriteOptions o;
        const bool ok = eps::save(at("rgb.eps"), im, ls, "rgb", o, err, notes);
        const std::string f = slurp(at("rgb.eps"));
        check("eps: written", ok && !f.empty(), err);
        const bool dos = f.size() > 30 && static_cast<unsigned char>(f[0]) == 0xC5 && static_cast<unsigned char>(f[1]) == 0xD0 &&
                         static_cast<unsigned char>(f[2]) == 0xD3 && static_cast<unsigned char>(f[3]) == 0xC6;
        check("eps: a preview -- the DOS EPS header, its parts where it says",
              dos && le32(f, 4) == 30 && le32(f, 4) + le32(f, 8) == le32(f, 20) && le32(f, 20) + le32(f, 24) == f.size(), "");
        const std::string ps = ps_of(f);
        check("eps: EPSF-3.0 PostScript, LanguageLevel 2, Clean7Bit",
              ps.rfind("%!PS-Adobe-3.0 EPSF-3.0\n", 0) == 0 && ps.find("%%LanguageLevel: 2") != std::string::npos &&
              std::all_of(ps.begin(), ps.end(), [](char c) { return static_cast<unsigned char>(c) < 128; }), "");
        check("eps: the bounding box -- 600 x 400 px at 144 dpi is 300 x 200 pt", ps.find("%%BoundingBox: 0 0 300 200\n") != std::string::npos, "");
        check("eps: the clipping path clips, its flatness set", ps.find("2 setflat") != std::string::npos && ps.find("eoclip") != std::string::npos &&
              ps.find("50 150 moveto") != std::string::npos, "");
        check("eps: it ends", ps.find("showpage") != std::string::npos && ps.rfind("%%EOF\n") == ps.size() - 6, "");
        // %BeginPhotoshop: the path resources, as hex comments.
        Bytes res;
        {
            const auto a = ps.find("%BeginPhotoshop: ");
            const auto b = ps.find("%EndPhotoshop");
            if (a != std::string::npos && b != std::string::npos) {
                const auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
                for (std::size_t i = ps.find('\n', a) + 1; i + 1 < b;) {
                    if (ps[i] == '%' || ps[i] == ' ' || ps[i] == '\n') { ++i; continue; }
                    res.push_back(static_cast<std::uint8_t>(hex(ps[i]) * 16 + hex(ps[i + 1])));
                    i += 2;
                }
            }
        }
        const auto pr = clip::parse(res.data(), res.size(), im.width, im.height);
        check("eps: %BeginPhotoshop carries the paths and names the clip", pr.paths.size() == 1 && pr.clip == pr.paths[0].name, pr.clip);
        // The image data: the composite's own numbers, white behind.
        const Bytes data = image_data(ps, false, 600 * 400 * 3);
        const auto px = [&](int x, int y, int c) { return static_cast<int>(data[(static_cast<std::size_t>(y) * 600 + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)]); };
        check("eps: the image data decodes to 600 x 400 x RGB", data.size() == 600u * 400u * 3u, std::to_string(data.size()));
        check("eps: the red square red, the background white",
              data.size() == 600u * 400u * 3u && px(150, 150, 0) == 255 && px(150, 150, 1) == 0 && px(150, 150, 2) == 0 &&
              px(10, 10, 0) == 255 && px(10, 10, 1) == 255 && px(599, 399, 2) == 255, "");
        check("eps: the notes say RGB, the clip, the preview",
              std::find(notes.begin(), notes.end(), "RGB") != notes.end() &&
              std::any_of(notes.begin(), notes.end(), [](const std::string& n) { return n.rfind("clipping path", 0) == 0; }) &&
              std::any_of(notes.begin(), notes.end(), [](const std::string& n) { return n.rfind("a TIFF preview 300", 0) == 0; }), "");
        // The preview at 72 ppi: 300 x 200.
        const std::string tif = dos ? f.substr(le32(f, 20), le32(f, 24)) : std::string();
        { std::ofstream out(at("p.tif"), std::ios::binary); out << tif; }
        const auto pv = formats::read_tiff(at("p.tif"), err);
        check("eps: the preview is the picture at 72 ppi (300 x 200), red where it's red",
              pv && pv->pixels.width() == 300 && pv->pixels.height() == 200 && pv->pixels.get(75, 75).c[0] > 0.9f && pv->pixels.get(75, 75).c[1] < 0.1f, err);

        // CMYK, ZIP (level 3), no preview.
        formats::WriteOptions c;
        c.eps_cmyk = true;
        c.eps_flate = true;
        c.eps_preview = false;
        notes.clear();
        const bool cok = eps::save(at("cmyk.eps"), im, ls, "cmyk", c, err, notes);
        const std::string cf = slurp(at("cmyk.eps"));
        check("eps: no preview -- plain PostScript, no DOS header", cok && cf.rfind("%!PS-Adobe-3.0 EPSF-3.0", 0) == 0, err);
        check("eps: ZIP is LanguageLevel 3, Flate, CMYK", cf.find("%%LanguageLevel: 3") != std::string::npos &&
              cf.find("/FlateDecode") != std::string::npos && cf.find("/DeviceCMYK") != std::string::npos &&
              cf.find("%%DocumentProcessColors: Cyan Magenta Yellow Black") != std::string::npos, "");
        const Bytes cd = image_data(cf, true, 600 * 400 * 4);
        const auto cp = [&](int x, int y, int k) { return static_cast<int>(cd[(static_cast<std::size_t>(y) * 600 + static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(k)]); };
        check("eps: the CMYK data decodes to 600 x 400 x 4", cd.size() == 600u * 400u * 4u, std::to_string(cd.size()));
        check("eps: white is no ink; red is magenta and yellow, little cyan",
              cd.size() == 600u * 400u * 4u && cp(10, 10, 0) < 8 && cp(10, 10, 1) < 8 && cp(10, 10, 2) < 8 && cp(10, 10, 3) < 8 &&
              cp(150, 150, 1) > 150 && cp(150, 150, 2) > 150 && cp(150, 150, 0) < 60,
              cd.size() < 4 ? "" : std::to_string(cp(150, 150, 0)) + " " + std::to_string(cp(150, 150, 1)) + " " + std::to_string(cp(150, 150, 2)) + " " + std::to_string(cp(150, 150, 3)));
        // No clipping path: no clip.
        clip::set(t.j, t.d, "", 0);
        notes.clear();
        eps::save(at("plain.eps"), t.d.image(), t.d.snapshot(), "plain", c, err, notes);
        const std::string nf = slurp(at("plain.eps"));
        check("eps: no clipping path -- no eoclip, said", nf.find("eoclip") == std::string::npos &&
              std::find(notes.begin(), notes.end(), "no clipping path") != notes.end(), "");
        const auto pl = eps::plan(t.d.image(), t.d.snapshot(), c);
        check("eps: the plan says there's no clipping path, and where to set one",
              std::any_of(pl.left_out.begin(), pl.left_out.end(), [](const std::string& s) { return s.find("Clipping Path") != std::string::npos; }), "");
        // A cancel leaves nothing.
        formats::Progress stop;
        stop.cancelled = [] { return true; };
        notes.clear();
        const bool cancelled = !eps::save(at("gone.eps"), im, ls, "gone", o, err, notes, stop);
        check("eps: a cancel writes nothing", cancelled && !fs::exists(at("gone.eps")) && !fs::exists(at("gone.eps.partial")), err);
        check("eps: an export format, \".eps\"", formats::export_format(formats::Kind::EPS) && std::string(formats::export_format(formats::Kind::EPS)->ext) == ".eps" &&
              formats::export_format_of_name("x.EPS") && formats::export_format_of_name("x.EPS")->kind == formats::Kind::EPS, "");
    }
    std::error_code ec;
    fs::remove_all(scratch(), ec);
}
