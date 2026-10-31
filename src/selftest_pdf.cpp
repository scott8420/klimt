// klimt_selftest -- s051: Export as PDF. The picture in bands, the text
// layers that can stay type kept as type (glyphs, fonts embedded), the rest
// baked -- and the plan saying which, and why, before it writes. Rules only:
// no test depends on which font this machine has or how its lines fall.
#include "core/File.hpp"
#include "core/Pdf.hpp"
#include "core/Text.hpp"
#include "formats/Formats.hpp"
#include "pixel/Pixel.hpp"
#include "text/Text.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

namespace P    = klimt::pixel;
namespace core = klimt::core;
namespace pdf  = klimt::core::pdf;
namespace fs   = std::filesystem;

using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
fs::path scratch() {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_pdf_" + std::to_string(::getpid()));
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
std::size_t count(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t i = hay.find(needle); i != std::string::npos; i = hay.find(needle, i + 1)) ++n;
    return n;
}
bool has(const std::vector<std::string>& v, const std::string& start) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.rfind(start, 0) == 0; });
}
}  // namespace

void selftest_pdf(const Check& check) {
    using L = core::Layer;
    const int W = 600, H = 400;
    const P::Format f8{P::Depth::U8, P::Mode::RGB, true};
    core::Image im;
    im.width = W; im.height = H; im.format = f8; im.dpi = 300;
    const auto fill = [&](P::Rect r, P::Px c) { return std::make_shared<const P::TileMap>(P::TileMap(f8, W, H).fill(r, c)); };
    const auto type = [&](const std::string& words, double x, double y) {
        L t;
        t.kind = core::Kind::Text;
        klimt::text::Spec s;
        s.text = words;
        s.size = 40;
        s.x = x; s.y = y;
        s.colour = {0.1f, 0.1f, 0.6f, 1.f};
        t.name = words;
        t.text = std::make_shared<const klimt::text::Spec>(s);
        t.pixels = core::text_pixels(im, s);
        return t;
    };
    L paper; paper.name = "Paper"; paper.pos = 1; paper.pixels = fill({0, 0, W, H}, P::rgba(0.98f, 0.95f, 0.88f));
    L title = type("Title", 40, 80);   title.pos = 2;
    L patch; patch.name = "Patch"; patch.pos = 3; patch.pixels = fill({400, 250, 100, 100}, P::rgba(0.9f, 0.2f, 0.1f));
    L caption = type("Caption", 40, 300); caption.pos = 4;
    std::map<std::string, L> ls{{"L1", paper}, {"L2", title}, {"L3", patch}, {"L4", caption}};

    // ── the plan: both titles stay type -- only Normal pixels above them.
    {
        const auto pl = pdf::plan(im, ls);
        check("pdf: the plan -- two text layers kept as type, in stack order",
              pl.live == std::vector<std::string>{"L2", "L4"} && has(pl.list.kept, "2 text layers as type") && pl.list.baked.empty(),
              pl.list.kept.empty() ? "" : pl.list.kept[0]);
        check("pdf: the plan -- nothing left out of a plain 8-bit RGB document", pl.list.left_out.empty(), "");
    }
    // ── written: a PDF, its fonts embedded, the picture in bands.
    {
        const std::string f = at("two.pdf");
        std::string err;
        std::vector<std::string> notes;
        const bool ok = pdf::save(f, im, ls, "two", err, notes);
        const std::string b = ok ? slurp(f) : std::string{};
        check("pdf: saves a PDF", ok && b.rfind("%PDF-", 0) == 0 && b.find("%%EOF") != std::string::npos, err);
        check("pdf: the type is type -- a font is embedded", b.find("/FontFile") != std::string::npos, "");
        // Bands: under Title (Paper), between the two (Patch); none above Caption.
        check("pdf: the picture in its bands -- two images (below Title, between)", count(b, "/Subtype /Image") >= 2, std::to_string(count(b, "/Subtype /Image")));
        check("pdf: the toast says what crossed", has(notes, "2 text layers as type"), notes.empty() ? "" : notes[0]);
        check("pdf: no .partial left behind", !fs::exists(f + ".partial"), "");
        // The composite beside it, for eyes and tools (pdftoppm against it).
        std::string e2;
        core::file::export_image(at("two.png"), klimt::formats::Kind::PNG, im, ls, e2);
    }
    // ── what bakes, and the reason given.
    {
        auto ls2 = ls;
        ls2["L5"] = patch; ls2["L5"].name = "Shade"; ls2["L5"].pos = 5; ls2["L5"].blend = core::Blend::Multiply;   // over Caption
        const auto pl = pdf::plan(im, ls2);
        check("pdf: under a Multiply layer, both titles bake (it would blend with them)",
              pl.live.empty() && has(pl.list.baked, "2 text layers as pixels -- under a layer that blends"), pl.list.baked.empty() ? "" : pl.list.baked[0]);
        auto ls3 = ls;
        ls3["L4"].styles.drop.on = true;   // Caption's own shadow
        const auto p3 = pdf::plan(im, ls3);
        check("pdf: a styled text layer bakes; the one under it bakes too (styles blend with what's below)",
              p3.live.empty() && has(p3.list.baked, "1 text layer as pixels -- its own blend") &&
                  has(p3.list.baked, "1 text layer as pixels -- under a layer"), "");
        auto ls4 = ls;
        ls4["G"] = L{}; ls4["G"].kind = core::Kind::Group; ls4["G"].name = "Words"; ls4["G"].pos = 5; ls4["G"].blend = core::Blend::PassThrough;
        ls4["L4"].parent = "G"; ls4["L4"].pos = 1;
        const auto p4 = pdf::plan(im, ls4);
        check("pdf: type inside a group bakes; Title under the plain group stays type",
              p4.live == std::vector<std::string>{"L2"} && has(p4.list.baked, "1 text layer as pixels -- inside a group"), "");
        auto ls5 = ls;
        ls5["L6"] = patch; ls5["L6"].name = "Clip"; ls5["L6"].pos = 4.5; ls5["L6"].clipped = true;   // clipped to Caption
        const auto p5 = pdf::plan(im, ls5);
        check("pdf: a clipping base bakes", p5.live == std::vector<std::string>{"L2"} && has(p5.list.baked, "1 text layer as pixels -- a layer is clipped"), "");
        auto ls6 = ls;
        ls6["L7"] = L{}; ls6["L7"].kind = core::Kind::Adjustment; ls6["L7"].name = "Levels"; ls6["L7"].pos = 9; ls6["L7"].visible = false;
        check("pdf: a HIDDEN adjustment layer above changes nothing -- both stay type", pdf::plan(im, ls6).live.size() == 2, "");
        ls6["L7"].visible = true;
        check("pdf: a visible one above bakes them (it adjusts what's below)", pdf::plan(im, ls6).live.empty(), "");
        // Baked type: no font in the file.
        const std::string f = at("baked.pdf");
        std::string err;
        std::vector<std::string> notes;
        const bool ok = pdf::save(f, im, ls2, "baked", err, notes);
        const std::string b = ok ? slurp(f) : std::string{};
        check("pdf: everything baked -- one picture, no font", ok && b.find("/FontFile") == std::string::npos && count(b, "/Subtype /Image") >= 1, err);
    }
    // ── 16-bit, a profile: said.
    {
        core::Image deep = im;
        deep.format = P::Format{P::Depth::U16, P::Mode::RGB, true};
        deep.icc = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{1, 2, 3});
        const auto pl = pdf::plan(deep, {});
        check("pdf: a 16-bit picture with a profile -- both said", has(pl.list.left_out, "the picture at 8 bits") &&
              has(pl.list.left_out, "the colour profile"), "");
    }
    // ── cancelled: nothing written.
    {
        klimt::formats::Progress stop;
        stop.cancelled = [] { return true; };
        const std::string f = at("cancelled.pdf");
        std::string err;
        std::vector<std::string> notes;
        const bool ok = pdf::save(f, im, ls, "x", err, notes, stop);
        check("pdf: cancelled -- false, no file", !ok && !fs::exists(f) && !fs::exists(f + ".partial"), err);
    }
}
