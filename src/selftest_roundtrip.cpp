// klimt_selftest -- s083: the Composix round-trip (core/RoundTrip.hpp). A PDF
// carries the Klimt document inside it and opens as that document; Save
// writes back in the format the file was; a failure leaves the file as it
// was. Rules only: nothing here depends on this machine's fonts.
#include "core/File.hpp"
#include "core/Pdf.hpp"
#include "core/RoundTrip.hpp"
#include "core/Smart.hpp"
#include "core/Text.hpp"
#include "core/Undo.hpp"
#include "formats/Formats.hpp"
#include "pixel/Pixel.hpp"
#include "text/Text.hpp"

#include <zlib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

namespace P  = klimt::pixel;
namespace F  = klimt::formats;
namespace core = klimt::core;
namespace rt = klimt::core::roundtrip;
namespace fs = std::filesystem;

using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
fs::path scratch() {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_rt_" + std::to_string(::getpid()));
        fs::create_directories(p);
        return p;
    }();
    return d;
}
std::string at(const std::string& name) { return (scratch() / name).string(); }
std::string slurp(const std::string& f) {
    std::ifstream in(f, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void spit(const std::string& f, const std::string& b) {
    std::ofstream out(f, std::ios::binary | std::ios::trunc);
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
}
bool same(const P::TileMap& a, const P::TileMap& b) {
    if (a.width() != b.width() || a.height() != b.height()) return false;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x)
            if (!(a.get(x, y) == b.get(x, y))) return false;
    return true;
}
bool has(const std::vector<std::string>& v, const std::string& start) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.rfind(start, 0) == 0; });
}
std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& n : v) s += (s.empty() ? "" : "; ") + n;
    return s;
}
// The layers that draw (not '@' records), by name.
std::map<std::string, const core::Layer*> by_name(const core::file::Contents& c) {
    std::map<std::string, const core::Layer*> m;
    for (const auto& [id, L] : c.layers)
        if (!id.empty() && id[0] != '@') m[L.name] = &L;
    return m;
}
// A hand-written PDF with a CLASSIC cross-reference table (what older cairo,
// and many other writers, make): one empty page.
std::string classic_pdf() {
    std::string s = "%PDF-1.4\n";
    std::vector<std::size_t> off;
    const auto obj = [&](const std::string& body) {
        off.push_back(s.size());
        s += std::to_string(off.size()) + " 0 obj\n" + body + "\nendobj\n";
    };
    obj("<< /Type /Catalog /Pages 2 0 R >>");
    obj("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    obj("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 72 72] >>");
    const std::size_t x = s.size();
    s += "xref\n0 4\n0000000000 65535 f \n";
    for (const auto o : off) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", o);
        s += line;
    }
    s += "trailer\n<< /Size 4 /Root 1 0 R /ID [<0123abcd> <0123abcd>] >>\nstartxref\n" + std::to_string(x) + "\n%%EOF\n";
    return s;
}
}  // namespace

void selftest_roundtrip(const Check& check) {
    using L = core::Layer;
    const int W = 300, H = 200;
    const P::Format f8{P::Depth::U8, P::Mode::RGB, true};
    core::file::Contents doc;
    doc.image.width = W; doc.image.height = H; doc.image.format = f8; doc.image.dpi = 300;
    const auto fill = [&](P::Rect r, P::Px c) { return std::make_shared<const P::TileMap>(P::TileMap(f8, W, H).fill(r, c)); };
    {
        L paper; paper.name = "Paper"; paper.pos = 1; paper.pixels = fill({0, 0, W, H}, P::rgba(0.98f, 0.95f, 0.88f));
        L patch; patch.name = "Patch"; patch.pos = 2; patch.pixels = fill({150, 100, 80, 60}, P::rgba(0.9f, 0.2f, 0.1f));
        L t; t.kind = core::Kind::Text; t.name = "Title"; t.pos = 3;
        klimt::text::Spec s; s.text = "Title"; s.size = 30; s.x = 20; s.y = 60; s.colour = {0.1f, 0.1f, 0.6f, 1.f};
        t.text = std::make_shared<const klimt::text::Spec>(s);
        t.pixels = core::text_pixels(doc.image, s);
        doc.layers = {{"L1", paper}, {"L2", patch}, {"L3", t}};
        doc.next_id = 4;
    }
    const auto look = [&](const core::file::Contents& c) { return core::file::flatten(c.image, c.layers); };
    const auto want = look(doc);

    // ── PDF: the page, and the document inside it ───────────────────────────
    const std::string pdf = at("placed.pdf");
    {
        std::string err;
        std::vector<std::string> notes;
        const bool ok = rt::save_back(pdf, F::Kind::PDF, doc, err, notes);
        const std::string b = ok ? slurp(pdf) : std::string{};
        check("roundtrip: a PDF written back -- a PDF, its last line %%EOF", ok && b.rfind("%PDF-", 0) == 0 &&
              b.size() > 6 && b.compare(b.size() - 6, 6, "%%EOF\n") == 0, err);
        check("roundtrip: the PDF's page as ever -- its type is type (a font embedded)", b.find("/FontFile") != std::string::npos, "");
        check("roundtrip: the document rides in ONE incremental update named from its trailer",
              b.find("/KlimtDocument") != std::string::npos && b.rfind("startxref") > b.rfind("/Type /KlimtDocument") &&
                  b.rfind("/Prev ") > b.rfind("/Type /KlimtDocument"), "");
        check("roundtrip: the toast says the layers are inside", has(notes, "Klimt's layers kept inside it"), joined(notes));
        check("roundtrip: nothing left beside it", !fs::exists(pdf + ".partial") &&
              std::none_of(fs::directory_iterator(scratch()), fs::directory_iterator(), [](const fs::directory_entry& e) {
                  return e.path().filename().string().find("placed.pdf.") == 0;
              }), "");
        check("roundtrip: kind_of says PDF", rt::kind_of(pdf) == F::Kind::PDF, "");
        const auto emb = rt::embedded(pdf);
        check("roundtrip: the .klimt read back out of it", emb && emb->size() > 100 && (*emb)[0] == 'P' && (*emb)[1] == 'K', "");
    }
    {
        std::string err;
        const auto c = core::file::open(pdf, err);
        const auto m = c ? by_name(*c) : std::map<std::string, const L*>{};
        check("roundtrip: opening the PDF opens the DOCUMENT -- three layers by name, kind PDF",
              c && c->kind == F::Kind::PDF && m.size() == 3 && m.count("Paper") && m.count("Patch") && m.count("Title"), err);
        check("roundtrip: its type still type (a text layer, its words)",
              m.count("Title") && m.at("Title")->kind == core::Kind::Text && m.at("Title")->text && m.at("Title")->text->text == "Title", "");
        const auto got = c ? look(*c) : std::nullopt;
        check("roundtrip: the picture exactly as it was", got && want && same(*got, *want), "");
        check("roundtrip: its resolution kept (300)", c && c->image.dpi == 300, "");
        check("roundtrip: the open's note says where the layers came from", c && has(c->notes, "Klimt's layers"), c ? joined(c->notes) : "");
    }
    // ── again: saved over, the newest document wins (no stale copy read) ───
    {
        auto d2 = doc;
        d2.layers["L2"].name = "Patch moved";
        d2.layers["L2"].pixels = fill({10, 120, 80, 60}, P::rgba(0.1f, 0.6f, 0.2f));
        std::string err;
        std::vector<std::string> notes;
        const bool ok = rt::save_back(pdf, F::Kind::PDF, d2, err, notes);
        const auto c = ok ? core::file::open(pdf, err) : std::nullopt;
        const auto m = c ? by_name(*c) : std::map<std::string, const L*>{};
        const auto got = c ? look(*c) : std::nullopt;
        const auto w2 = look(d2);
        check("roundtrip: saved again -- the new document, not the old", c && m.count("Patch moved") && !m.count("Patch") && got && w2 && same(*got, *w2), err);
        const std::string b = slurp(pdf);
        check("roundtrip: one document in the file (rewritten whole, not stacked)",
              b.find("/Type /KlimtDocument") == b.rfind("/Type /KlimtDocument") && b.find("/Type /KlimtDocument") != std::string::npos, "");
    }
    // ── a PDF Klimt didn't write: refused, in words ─────────────────────────
    {
        const std::string plain = at("plain.pdf");
        std::string err;
        std::vector<std::string> notes;
        core::pdf::save(plain, doc.image, doc.layers, "plain", err, notes);
        std::string e2;
        const auto c = core::file::open(plain, e2);
        check("roundtrip: a PDF without Klimt's layers is refused, saying why", !c && e2.find("didn't write") != std::string::npos, e2);
        check("roundtrip: ... and has none to give", !rt::embedded(plain), "");
    }
    // ── a classic cross-reference table: the update in the same style ──────
    {
        const std::string f = at("classic.pdf");
        spit(f, classic_pdf());
        const auto emb = rt::embedded(at("placed.pdf"));
        std::string err;
        const bool ok = emb && rt::embed(f, *emb, err);
        const std::string b = slurp(f);
        const auto obj = b.find("/Type /KlimtDocument");
        const auto tail = obj == std::string::npos ? std::string{} : b.substr(obj);
        check("roundtrip: a table PDF gets a table update (xref, a trailer with /Prev, /Root, /ID kept)",
              ok && tail.find("\nxref\n") != std::string::npos && tail.find("/Prev ") != std::string::npos &&
                  tail.find("/Root 1 0 R") != std::string::npos && tail.find("/ID [<0123abcd> <0123abcd>]") != std::string::npos &&
                  tail.find("/XRef") == std::string::npos, err);
        const auto back = rt::embedded(f);
        check("roundtrip: ... and the document read back from it, byte for byte", back && emb && *back == *emb, "");
    }
    // ── rewritten by another tool, the stream compressed: still found ──────
    {
        const auto emb = rt::embedded(pdf);
        std::string z;
        if (emb) {
            uLongf n = compressBound(static_cast<uLong>(emb->size()));
            z.resize(n);
            compress(reinterpret_cast<Bytef*>(z.data()), &n, emb->data(), static_cast<uLong>(emb->size()));
            z.resize(n);
        }
        std::string s = "%PDF-1.7\n9 0 obj\n<< /Filter /FlateDecode /Length " + std::to_string(z.size()) +
                        " /Subtype /application#2Fx-klimt /Type /KlimtDocument >>\nstream\n" + z + "\nendstream\nendobj\n%%EOF\n";
        const std::string f = at("rewritten.pdf");
        spit(f, s);
        const auto back = rt::embedded(f);
        check("roundtrip: a FlateDecode'd copy (qpdf rewrote it) is inflated and found", back && emb && *back == *emb, "");
    }
    // ── the other formats: PSD keeps layers, TIFF is the one picture ───────
    {
        const std::string psd = at("placed.psd");
        std::string err;
        std::vector<std::string> notes;
        const bool ok = rt::save_back(psd, F::Kind::PSD, doc, err, notes);
        const auto c = ok ? core::file::open(psd, err) : std::nullopt;
        check("roundtrip: PSD written back with its layers", c && c->kind == F::Kind::PSD && by_name(*c).count("Patch"), err);
        const std::string tif = at("placed.tif");
        notes.clear();
        const bool ok2 = rt::save_back(tif, F::Kind::TIFF, doc, err, notes);
        const auto t = ok2 ? core::file::open(tif, err) : std::nullopt;
        const auto got = t ? look(*t) : std::nullopt;
        check("roundtrip: TIFF written back -- the picture, exactly", t && t->kind == F::Kind::TIFF && got && want && same(*got, *want), err);
        check("roundtrip: TIFF -- \"flattened\" said (three layers in, one picture out)", has(notes, "flattened -- a TIFF"), joined(notes));
        const std::string png = at("one.png");
        core::file::Contents one = doc;
        one.layers.erase("L2"); one.layers.erase("L3");
        notes.clear();
        rt::save_back(png, F::Kind::PNG, one, err, notes);
        check("roundtrip: one layer -- nothing to say about flattening", notes.empty(), joined(notes));
    }
    // ── a failure or a cancel leaves the placed file as it was ─────────────
    {
        const std::string before = slurp(pdf);
        F::Progress stop;
        stop.cancelled = [] { return true; };
        std::string err;
        std::vector<std::string> notes;
        const bool ok = rt::save_back(pdf, F::Kind::PDF, doc, err, notes, stop);
        check("roundtrip: cancelled -- false, the file byte for byte as it was", !ok && slurp(pdf) == before, err);
        check("roundtrip: cancelled -- no temp file left", std::none_of(fs::directory_iterator(scratch()), fs::directory_iterator(),
              [](const fs::directory_entry& e) { return e.path().filename().string().find("placed.pdf.") == 0; }), "");
        check("roundtrip: an unknown kind refused in words", !rt::save_back(at("x.bin"), F::Kind::Unknown, doc, err, notes) && !err.empty(), err);
        check("roundtrip: writes_back names the formats; none for Unknown",
              std::string(rt::writes_back(F::Kind::PDF)) == "PDF" && std::string(rt::writes_back(F::Kind::TIFF)) == "TIFF" &&
                  !rt::writes_back(F::Kind::Unknown), "");
    }
    // ── m3: Edit Contents -- the picture as a document, and back ───────────
    {
        core::Document d;
        core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        core::layers::load_fixture(d);
        const auto pic = P::TileMap(f8, 90, 60).paint({0, 0, 90, 60}, [](int x, int y, const P::Px&) {
            return P::rgba(static_cast<float>(x) / 90, ((x / 5 + y / 5) % 2) ? 1.f : 0.f, static_cast<float>(y) / 60, 1.f);
        });
        const auto id = core::layers::place_smart(j, d, pic, "inset.png", 240);
        const auto l = d.get(id);
        const auto c = l && l->smart ? rt::contents_of(*l->smart, d.image()) : core::file::Contents{};
        const auto m = by_name(c);
        check("roundtrip: Edit Contents -- the picture as a document: its size, its dpi, one layer named for it",
              c.image.width == 90 && c.image.height == 60 && c.image.dpi == 240 && m.size() == 1 && m.count("inset.png") &&
                  c.image.format == d.image().format, "");
        const auto same_back = rt::picture_of(c, d.image().format);
        check("roundtrip: Edit Contents -- untouched, it draws the picture exactly", same_back && same(*same_back, pic), "");
        // Painted on in its tab: a stripe across it, on a layer of its own.
        auto edited = c;
        core::Layer stripe;
        stripe.name = "Stripe"; stripe.pos = 2;
        stripe.pixels = std::make_shared<const P::TileMap>(P::TileMap(c.image.format, 90, 60).fill({0, 25, 90, 10}, P::rgba(1, 1, 0)));
        edited.layers["L2"] = stripe;
        const auto pic2 = rt::picture_of(edited, d.image().format);
        check("roundtrip: Edit Contents -- what the tab draws, both layers, as one picture",
              pic2 && pic2->get(45, 30) == P::rgba(1, 1, 0) && pic2->get(45, 5) == pic.get(45, 5), "");
        const bool ok = pic2 && core::layers::replace_smart(j, d, id, *pic2, "inset.png", 240, "", 0, 0, "Edit Contents");
        const auto l2 = d.get(id);
        check("roundtrip: Edit Contents -- back in its smart layer, ONE step \"Edit Contents\", still smart, the same map",
              ok && j.undo_label().rfind("Edit Contents", 0) == 0 && l2 && l2->kind == core::Kind::Smart && l2->smart &&
                  same(*l2->smart->source, *pic2) && l2->smart->m == l->smart->m, j.undo_label());
        j.undo();
        const auto l3 = d.get(id);
        check("roundtrip: Edit Contents -- undone, the picture as it was", l3 && l3->smart && same(*l3->smart->source, pic), "");
        const auto gray = rt::picture_of(c, P::Format{P::Depth::U16, P::Mode::RGB, true});
        check("roundtrip: Edit Contents -- converted to the owner's format when they differ",
              gray && gray->format() == P::Format{P::Depth::U16, P::Mode::RGB, true}, "");
    }
    // ── a Klimt PDF placed as a smart layer: what the eye sees ─────────────
    {
        std::string err;
        const auto l = core::smart::load(pdf, err);
        check("roundtrip: a Klimt PDF places (smart::load) -- its picture, its size", l && l->pixels.width() == W && l->pixels.height() == H, err);
    }
}
