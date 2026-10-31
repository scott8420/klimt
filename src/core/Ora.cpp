// core::ora -- OpenRaster in and out (s049). See include/core/Ora.hpp.
#include "core/Ora.hpp"
#include "core/Composite.hpp"
#include "core/Transfer.hpp"

#include <zip.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <sstream>

namespace klimt::core::ora {

namespace {
namespace P = pixel;
constexpr std::size_t kMaxXml = 16u << 20;
constexpr std::size_t kMaxPng = std::size_t{1} << 31;

// ── a small XML reader: elements and attributes, nothing else ─────────────
// stack.xml is elements with attributes; text, comments, processing
// instructions and a DOCTYPE are skipped. Entities: the five named ones and
// numeric references.
struct Elem {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Elem> kids;
    std::string attr(const char* k, const std::string& dflt = {}) const {
        for (const auto& [n, v] : attrs) if (n == k) return v;
        return dflt;
    }
    bool has(const char* k) const {
        for (const auto& a : attrs) if (a.first == k) return true;
        return false;
    }
};

void utf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
std::string unescape(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const auto semi = s.find(';', i);
        if (semi == std::string_view::npos) { out += s[i]; continue; }
        const auto ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            const bool hexa = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
            const std::string digits(ent.substr(hexa ? 2 : 1));
            char* end = nullptr;
            const unsigned long cp = std::strtoul(digits.c_str(), &end, hexa ? 16 : 10);
            if (digits.empty() || *end) { out += s.substr(i, semi - i + 1); }
            else utf8(out, cp);
        } else out += s.substr(i, semi - i + 1);
        i = semi;
    }
    return out;
}
std::string escape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20 && ch != '\t' && ch != '\n') out += ' ';
                else out += ch;
        }
    }
    return out;
}

class XmlReader {
public:
    explicit XmlReader(std::string_view s) : m_s(s) {}
    // The document's root element; nullopt (and `error`) if it isn't XML.
    std::optional<Elem> root(std::string& error) {
        std::vector<Elem> stack;
        std::optional<Elem> top;
        while (m_i < m_s.size()) {
            const auto lt = m_s.find('<', m_i);
            if (lt == std::string_view::npos) break;
            m_i = lt;
            if (starts("<?")) { if (!skip_to("?>")) return bad(error); continue; }
            if (starts("<!--")) { if (!skip_to("-->")) return bad(error); continue; }
            if (starts("<!")) { if (!skip_to(">")) return bad(error); continue; }
            if (starts("</")) {
                const auto gt = m_s.find('>', m_i);
                if (gt == std::string_view::npos || stack.empty()) return bad(error);
                m_i = gt + 1;
                Elem e = std::move(stack.back());
                stack.pop_back();
                if (stack.empty()) { top = std::move(e); break; }
                stack.back().kids.push_back(std::move(e));
                continue;
            }
            ++m_i;
            Elem e;
            e.name = name();
            if (e.name.empty()) return bad(error);
            bool closed = false;
            for (;;) {
                spaces();
                if (m_i >= m_s.size()) return bad(error);
                if (starts("/>")) { m_i += 2; closed = true; break; }
                if (m_s[m_i] == '>') { ++m_i; break; }
                std::string k = name();
                if (k.empty()) return bad(error);
                spaces();
                if (m_i >= m_s.size() || m_s[m_i] != '=') return bad(error);
                ++m_i;
                spaces();
                if (m_i >= m_s.size() || (m_s[m_i] != '"' && m_s[m_i] != '\'')) return bad(error);
                const char q = m_s[m_i++];
                const auto end = m_s.find(q, m_i);
                if (end == std::string_view::npos) return bad(error);
                e.attrs.emplace_back(std::move(k), unescape(m_s.substr(m_i, end - m_i)));
                m_i = end + 1;
            }
            if (stack.size() > 256) { error = "a stack.xml nested too deep"; return std::nullopt; }
            if (closed) {
                if (stack.empty()) { top = std::move(e); break; }
                stack.back().kids.push_back(std::move(e));
            } else {
                stack.push_back(std::move(e));
            }
        }
        if (!top) return bad(error);
        return top;
    }

private:
    std::optional<Elem> bad(std::string& error) { error = "a damaged stack.xml"; return std::nullopt; }
    bool starts(std::string_view t) const { return m_s.substr(m_i, t.size()) == t; }
    bool skip_to(std::string_view t) {
        const auto at = m_s.find(t, m_i);
        if (at == std::string_view::npos) return false;
        m_i = at + t.size();
        return true;
    }
    void spaces() { while (m_i < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_i]))) ++m_i; }
    std::string name() {
        const auto b = m_i;
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '=' || c == '>' || c == '/' || c == '<') break;
            ++m_i;
        }
        return std::string(m_s.substr(b, m_i - b));
    }
    std::string_view m_s;
    std::size_t      m_i = 0;
};

struct Zip {
    zip_t* z = nullptr;
    ~Zip() { if (z) zip_discard(z); }
};
bool read_entry(zip_t* z, const std::string& name, std::vector<std::uint8_t>& out, std::size_t limit) {
    const zip_int64_t idx = zip_name_locate(z, name.c_str(), 0);
    if (idx < 0) return false;
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat_index(z, static_cast<zip_uint64_t>(idx), 0, &st) != 0 || !(st.valid & ZIP_STAT_SIZE) || st.size > limit)
        return false;
    out.resize(static_cast<std::size_t>(st.size));
    zip_file_t* f = zip_fopen_index(z, static_cast<zip_uint64_t>(idx), 0);
    if (!f) return false;
    const zip_int64_t got = st.size ? zip_fread(f, out.data(), st.size) : 0;
    zip_fclose(f);
    return got == static_cast<zip_int64_t>(st.size);
}
// A PNG entry's bit depth, from its first 25 bytes (IHDR); 0: not a PNG.
int png_depth(zip_t* z, const std::string& name) {
    zip_file_t* f = zip_fopen(z, name.c_str(), 0);
    if (!f) return 0;
    unsigned char b[25] = {};
    const zip_int64_t got = zip_fread(f, b, sizeof b);
    zip_fclose(f);
    if (got != 25 || std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) != 0) return 0;
    return b[24];
}

// ── blend modes ───────────────────────────────────────────────────────────
// ORA's own (the SVG compositing names) where one fits; Krita's ids for the
// rest, which Krita writes as "krita:<id>" and reads back.
struct OpName { Blend b; const char* op; };
constexpr OpName kOps[] = {
    {Blend::Normal,       "svg:src-over"},
    {Blend::Multiply,     "svg:multiply"},
    {Blend::Screen,       "svg:screen"},
    {Blend::Overlay,      "svg:overlay"},
    {Blend::Darken,       "svg:darken"},
    {Blend::Lighten,      "svg:lighten"},
    {Blend::ColorDodge,   "svg:color-dodge"},
    {Blend::ColorBurn,    "svg:color-burn"},
    {Blend::HardLight,    "svg:hard-light"},
    {Blend::SoftLight,    "svg:soft-light"},
    {Blend::Difference,   "svg:difference"},
    {Blend::Color,        "svg:color"},
    {Blend::Luminosity,   "svg:luminosity"},
    {Blend::Hue,          "svg:hue"},
    {Blend::Saturation,   "svg:saturation"},
    {Blend::LinearDodge,  "svg:plus"},
    {Blend::Dissolve,     "krita:dissolve"},
    {Blend::LinearBurn,   "krita:linear_burn"},
    {Blend::DarkerColor,  "krita:darker color"},
    {Blend::LighterColor, "krita:lighter color"},
    {Blend::VividLight,   "krita:vivid_light"},
    {Blend::LinearLight,  "krita:linear light"},
    {Blend::PinLight,     "krita:pin_light"},
    {Blend::HardMix,      "krita:hard mix"},
    {Blend::Exclusion,    "krita:exclusion"},
    {Blend::Subtract,     "krita:subtract"},
    {Blend::Divide,       "krita:divide"},
};
std::string fold(std::string_view s) {
    std::string out;
    for (char c : s) out += (c == ' ' || c == '_') ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string fmt_num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    std::string s = b;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// The tiles a map holds inside the image, as a tile-aligned rectangle.
struct TileBox { int c0 = 0, r0 = 0, c1 = -1, r1 = -1; bool empty() const { return c1 < c0; } };
TileBox held(const P::TileMap& m) {
    TileBox b{m.cols(), m.rows(), -1, -1};
    const int ach = m.format().alpha ? m.format().channels() - 1 : -1;
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c) {
            const auto t = m.tile(c, r);
            if (!t) continue;
            if (t->is_uniform() && ach >= 0 && t->get(0, 0).c[static_cast<std::size_t>(ach)] <= 0.f) continue;
            b.c0 = std::min(b.c0, c); b.r0 = std::min(b.r0, r);
            b.c1 = std::max(b.c1, c); b.r1 = std::max(b.r1, r);
        }
    return b;
}
// The map cut to a tile-aligned box: the same tiles, shared, no copying.
P::TileMap cut(const P::TileMap& m, const TileBox& b) {
    const int x0 = b.c0 * P::kTile, y0 = b.r0 * P::kTile;
    const int w = std::min((b.c1 + 1) * P::kTile, m.width()) - x0;
    const int h = std::min((b.r1 + 1) * P::kTile, m.height()) - y0;
    std::vector<P::TileMap::TileRef> grid;
    for (int r = b.r0; r <= b.r1; ++r)
        for (int c = b.c0; c <= b.c1; ++c) grid.push_back(m.tile(c, r));
    return P::TileMap::from_tiles(m.format(), w, h, std::move(grid));
}

// A map of another size placed at (ox, oy) on a W x H canvas, cut at its edge.
P::TileMap place(const P::TileMap& src, int ox, int oy, int W, int H) {
    if (ox == 0 && oy == 0 && src.width() == W && src.height() == H) return src;
    const P::Format f = src.format();
    const auto bpp = static_cast<std::size_t>(f.bytes_per_pixel());
    formats::BandBuilder band(f, W, H);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(src.width()) * bpp);
    const int sx0 = std::max(0, -ox), sx1 = std::min(src.width(), W - ox);
    for (int y = 0; y < H; ++y) {
        std::uint8_t* d = band.row(y);
        std::memset(d, 0, band.row_bytes());
        const int sy = y - oy;
        if (sy >= 0 && sy < src.height() && sx1 > sx0) {
            src.row_bytes(sy, row.data());
            std::memcpy(d + static_cast<std::size_t>(sx0 + ox) * bpp, row.data() + static_cast<std::size_t>(sx0) * bpp,
                        static_cast<std::size_t>(sx1 - sx0) * bpp);
        }
        band.done(y);
    }
    return band.take();
}

std::string check_tree(const file::Contents& c) {
    Document d;
    Document::Loading load(d);
    d.replace(c.image, c.layers, c.next_id);
    return d.check();
}

struct ZipState { const formats::Progress* p; };
void zip_progress(zip_t*, double f, void* ud) { static_cast<ZipState*>(ud)->p->at(0.6 + 0.4 * f); }
int  zip_cancel(zip_t*, void* ud) { return static_cast<ZipState*>(ud)->p->stop() ? 1 : 0; }

std::string plural(int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}
}  // namespace

std::string composite_op(Blend b) {
    for (const auto& o : kOps) if (o.b == b) return o.op;
    return "svg:src-over";
}

std::optional<Blend> blend_of_op(const std::string& op) {
    if (op.empty()) return Blend::Normal;
    const auto colon = op.find(':');
    const std::string bare = fold(colon == std::string::npos ? op : op.substr(colon + 1));
    for (const auto& o : kOps) {
        const std::string_view s = o.op;
        if (fold(s.substr(s.find(':') + 1)) == bare) return o.b;
    }
    if (bare == "pass-through") return Blend::PassThrough;
    return blend_from_key(bare);   // "klimt:linear-burn", or a Krita id that is Klimt's key
}

pixel::TileMap thumbnail(const pixel::TileMap& flat, int side) {
    const int W = flat.width(), H = flat.height();
    const P::Format out{P::Depth::U8, P::Mode::RGB, true};
    if (W <= 0 || H <= 0) return P::TileMap(out, 1, 1);
    const double s = std::min(1.0, static_cast<double>(side) / std::max(W, H));
    const int tw = std::max(1, static_cast<int>(std::lround(W * s))), th = std::max(1, static_cast<int>(std::lround(H * s)));
    // Halve (premultiplied box means) while the result stays at least the size.
    int k = 0;
    while (k < 6 && std::max(W, H) / (1 << (k + 1)) >= std::max(tw, th)) ++k;
    const P::TileMap src = k ? reduced_map(flat, k) : flat;
    const int sw = src.width(), sh = src.height();
    const int ach = src.format().channels() - 1;
    const bool alpha = src.format().alpha;
    const bool gray = src.format().mode == P::Mode::Gray;
    const double fx = static_cast<double>(sw) / tw, fy = static_cast<double>(sh) / th;
    return P::TileMap(out, tw, th).paint({0, 0, tw, th}, [&](int x, int y, const P::Px&) {
        // The box this pixel covers in `src`, averaged premultiplied.
        const int x0 = static_cast<int>(x * fx), x1 = std::max(x0 + 1, std::min(sw, static_cast<int>(std::ceil((x + 1) * fx))));
        const int y0 = static_cast<int>(y * fy), y1 = std::max(y0 + 1, std::min(sh, static_cast<int>(std::ceil((y + 1) * fy))));
        double r = 0, g = 0, b = 0, a = 0;
        int n = 0;
        for (int yy = y0; yy < y1; ++yy)
            for (int xx = x0; xx < x1; ++xx) {
                const P::Px p = src.get(xx, yy);
                const double pa = alpha ? p.c[static_cast<std::size_t>(ach)] : 1.0;
                const double pr = p.c[0], pg = gray ? p.c[0] : p.c[1], pb = gray ? p.c[0] : p.c[2];
                r += pr * pa; g += pg * pa; b += pb * pa; a += pa;
                ++n;
            }
        if (a <= 0) return P::transparent();
        return P::rgba(static_cast<float>(r / a), static_cast<float>(g / a), static_cast<float>(b / a), static_cast<float>(a / n));
    });
}

bool png_bytes(const pixel::TileMap& m, std::vector<std::uint8_t>& out, std::string& error,
               const formats::Progress& p, int level, double dpi) {
    formats::Raster r;
    r.pixels = m;
    r.dpi = dpi;
    return formats::write_png_bytes(r, out, error, p, level);
}

// ── reading ───────────────────────────────────────────────────────────────
std::optional<file::Contents> open(const std::string& path, std::string& error, const formats::Progress& p) {
    int code = 0;
    Zip zip;
    zip.z = zip_open(path.c_str(), ZIP_RDONLY, &code);
    if (!zip.z) { error = "not a readable OpenRaster file"; return std::nullopt; }
    zip_t* z = zip.z;
    std::vector<std::uint8_t> raw;
    if (!read_entry(z, "stack.xml", raw, kMaxXml)) { error = "a damaged OpenRaster file (no stack.xml)"; return std::nullopt; }
    XmlReader xr(std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));
    auto root = xr.root(error);
    if (!root) return std::nullopt;
    if (root->name != "image") { error = "a damaged stack.xml (no <image>)"; return std::nullopt; }
    const auto num = [](const std::string& s, double dflt) {
        if (s.empty()) return dflt;
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        return (end && *end == 0 && std::isfinite(v)) ? v : dflt;
    };
    const int W = static_cast<int>(num(root->attr("w"), 0)), H = static_cast<int>(num(root->attr("h"), 0));
    if (W < 1 || H < 1 || W > formats::kMaxSide || H > formats::kMaxSide ||
        static_cast<std::uint64_t>(W) * static_cast<std::uint64_t>(H) > formats::kMaxPixels) {
        error = "an image size Klimt can't hold (" + std::to_string(W) + " x " + std::to_string(H) + ")";
        return std::nullopt;
    }
    const Elem* top = nullptr;
    for (const auto& k : root->kids) if (k.name == "stack") { top = &k; break; }
    if (!top) { error = "a damaged stack.xml (no <stack>)"; return std::nullopt; }

    // First pass: the layers to read, and their depth (16 if any PNG is).
    int total = 0;
    bool deep = false;
    std::function<void(const Elem&)> scan = [&](const Elem& s) {
        for (const auto& k : s.kids) {
            if (k.name == "stack") scan(k);
            else if (k.name == "layer") {
                ++total;
                if (png_depth(z, k.attr("src")) == 16) deep = true;
            }
        }
    };
    scan(*top);

    file::Contents c;
    c.kind = formats::Kind::ORA;
    c.image.width = W;
    c.image.height = H;
    c.image.format = P::Format{deep ? P::Depth::U16 : P::Depth::U8, P::Mode::RGB, true};
    const double xres = num(root->attr("xres"), 72);
    c.image.dpi = (xres >= 1 && xres <= 10000) ? xres : 72;
    int next = 1, done = 0, skipped = 0, unknown_blend = 0, beyond = 0;
    std::string fail;
    std::function<bool(const Elem&, const std::string&)> walk = [&](const Elem& s, const std::string& parent) -> bool {
        // Top first in the file; Klimt's pos ascends bottom -> top.
        std::vector<const Elem*> items;
        for (const auto& k : s.kids) if (k.name == "stack" || k.name == "layer") items.push_back(&k);
        int pos = static_cast<int>(items.size());
        for (const Elem* e : items) {
            if (p.stop()) { fail = "cancelled"; return false; }
            Layer L;
            L.parent = parent;
            L.pos = pos--;
            L.visible = e->attr("visibility", "visible") != "hidden";
            L.opacity = static_cast<int>(std::lround(std::clamp(num(e->attr("opacity"), 1.0), 0.0, 1.0) * 100));
            const std::string op = e->attr("composite-op");
            auto blend = blend_of_op(op);
            if (!blend) { ++unknown_blend; blend = Blend::Normal; }
            const std::string id = "L" + std::to_string(next);
            if (e->name == "stack") {
                L.kind = Kind::Group;
                L.name = e->attr("name", "Group " + std::to_string(next));
                // ORA's "auto" isolation with plain src-over is Pass Through;
                // "isolate", or any other mode, composites the group alone.
                const bool isolate = e->attr("isolation", "auto") == "isolate";
                L.blend = (!isolate && *blend == Blend::Normal) ? Blend::PassThrough : *blend;
                ++next;
                c.layers[id] = L;
                if (!walk(*e, id)) return false;
                continue;
            }
            const std::string src = e->attr("src");
            const std::string low = fold(src);
            if (low.size() < 4 || low.substr(low.size() - 4) != ".png") { ++skipped; ++pos; continue; }   // an SVG / text layer
            std::vector<std::uint8_t> bytes;
            if (!read_entry(z, src, bytes, kMaxPng)) { fail = "a damaged OpenRaster file (" + src + ")"; return false; }
            std::string err;
            auto r = formats::read_png_bytes(bytes.data(), bytes.size(), err);
            if (!r) { fail = src + ": " + err; return false; }
            bytes = {};
            const int ox = static_cast<int>(num(e->attr("x"), 0)), oy = static_cast<int>(num(e->attr("y"), 0));
            if (ox < 0 || oy < 0 || ox + r->pixels.width() > W || oy + r->pixels.height() > H) ++beyond;
            P::TileMap m = r->pixels.format() == c.image.format ? std::move(r->pixels) : transfer::converted(r->pixels, c.image.format);
            L.kind = Kind::Pixel;
            L.name = e->attr("name", "Layer " + std::to_string(next));
            L.blend = *blend == Blend::PassThrough ? Blend::Normal : *blend;
            L.pixels = std::make_shared<const P::TileMap>(place(m, ox, oy, W, H));
            ++next;
            c.layers[id] = std::move(L);
            ++done;
            p.at(0.05 + 0.95 * done / std::max(1, total));
        }
        return true;
    };
    if (!walk(*top, "")) { error = fail; return std::nullopt; }
    // Positions skipped by a left-out layer leave gaps; gaps are harmless
    // (pos only orders), but keep them 1..n for a tidy tree.
    {
        std::map<std::string, std::vector<std::pair<double, std::string>>> by;
        for (const auto& [id, L] : c.layers) by[L.parent].emplace_back(L.pos, id);
        for (auto& [par, v] : by) {
            std::sort(v.begin(), v.end());
            for (std::size_t i = 0; i < v.size(); ++i) c.layers[v[i].second].pos = static_cast<double>(i + 1);
        }
    }
    if (c.layers.empty()) {   // an ORA with nothing Klimt reads: one empty layer
        Layer L;
        L.name = "Layer 1";
        L.pos = 1;
        L.pixels = std::make_shared<const P::TileMap>(P::TileMap(c.image.format, W, H));
        c.layers["L1"] = std::move(L);
        next = 2;
    }
    c.next_id = next;
    if (skipped) c.notes.push_back(plural(skipped, "layer", "layers") + " of a kind Klimt doesn't read left out");
    if (unknown_blend) c.notes.push_back(plural(unknown_blend, "unknown blend mode", "unknown blend modes") + " shown as Normal");
    if (beyond) c.notes.push_back(plural(beyond, "layer", "layers") + " cut at the canvas edge");
    const std::string why = check_tree(c);
    if (!why.empty()) { error = "a layer tree Klimt can't hold: " + why; return std::nullopt; }
    p.at(1);
    return c;
}

// ── writing ───────────────────────────────────────────────────────────────
// s050: the rules save() follows, counted without writing.
Plan plan(const Image& im, const std::map<Document::Id, Layer>& layers) {
    int n_adjust = 0, n_paths = 0, n_clip = 0, n_group_fx = 0;
    int n_mask = 0, n_filters = 0, n_styles = 0, n_text = 0, n_fill = 0;
    const bool rgb = im.format.mode == P::Mode::RGB;
    for (const auto& [id, L] : layers) {
        if (id.empty() || id[0] == '@') continue;
        if (L.kind == Kind::Group) {
            if ((L.mask && L.mask_on) || L.styles.any()) ++n_group_fx;
            if (L.clipped) ++n_clip;
            continue;
        }
        if (L.kind == Kind::Adjustment || L.kind == Kind::Filter) { ++n_adjust; continue; }
        if (!draws_pixels(L.kind)) { ++n_paths; continue; }
        if (L.clipped) ++n_clip;
        if (L.mask && L.mask_on) ++n_mask;
        if (!L.filters.empty()) ++n_filters;
        if (L.styles.any()) ++n_styles;
        if (L.kind == Kind::Text) ++n_text;
        const bool plain = L.kind == Kind::Pixel && rgb && !(L.mask && L.mask_on) && L.filters.empty() && !L.styles.any();
        if (!plain && L.fill < 100) ++n_fill;
    }
    Plan pl;
    if (n_mask) pl.baked.push_back(plural(n_mask, "layer mask", "layer masks"));
    if (n_filters) pl.baked.push_back(plural(n_filters, "layer's live filters", "layers' live filters"));
    if (n_styles) pl.baked.push_back(plural(n_styles, "layer's styles", "layers' styles"));
    if (n_fill) pl.baked.push_back(plural(n_fill, "layer's Fill", "layers' Fill"));
    if (n_text) pl.baked.push_back(plural(n_text, "text layer", "text layers") + " as pixels");
    if (n_adjust) pl.left_out.push_back(plural(n_adjust, "adjustment / filter layer", "adjustment / filter layers") + " left out (ORA has none)");
    if (n_paths) pl.left_out.push_back(plural(n_paths, "path layer", "path layers") + " left out");
    if (n_clip) pl.left_out.push_back(plural(n_clip, "clipped layer", "clipped layers") + " written unclipped");
    if (n_group_fx) pl.left_out.push_back(plural(n_group_fx, "group's mask / styles", "groups' masks / styles") + " left out");
    if (!rgb) pl.left_out.push_back("written as RGB");
    return pl;
}

bool save(const std::string& path, const Image& im, const std::map<Document::Id, Layer>& layers,
          std::string& error, std::vector<std::string>& notes, const formats::Progress& p) {
    if (im.width < 1 || im.height < 1) { error = "an empty image"; return false; }
    const P::Format fmt{im.format.depth == P::Depth::U8 ? P::Depth::U8 : P::Depth::U16, P::Mode::RGB, true};
    const std::string tmp = path + ".partial";
    std::remove(tmp.c_str());
    int code = 0;
    Zip zip;
    zip.z = zip_open(tmp.c_str(), ZIP_CREATE | ZIP_EXCL, &code);
    if (!zip.z) { error = "can't be written"; return false; }
    zip_t* z = zip.z;
    std::vector<std::vector<std::uint8_t>> keep;   // every buffer libzip reads at close
    const auto add = [&](const std::string& name, std::vector<std::uint8_t> data, zip_int32_t method, zip_uint32_t level) {
        keep.push_back(std::move(data));
        const auto& d = keep.back();
        zip_source_t* s = zip_source_buffer(z, d.data(), d.size(), 0);
        if (!s) return false;
        const zip_int64_t i = zip_file_add(z, name.c_str(), s, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (i < 0) { zip_source_free(s); return false; }
        return zip_set_file_compression(z, static_cast<zip_uint64_t>(i), method, level) == 0;
    };
    const auto text = [](const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); };
    if (!add("mimetype", text("image/openraster"), ZIP_CM_STORE, 0)) { error = zip_strerror(z); return false; }

    // The tree, top first.
    std::map<std::string, std::vector<std::pair<double, std::string>>> kids;
    for (const auto& [id, L] : layers)
        if (!id.empty() && id[0] != '@') kids[L.parent].emplace_back(L.pos, id);
    for (auto& [par, v] : kids) std::sort(v.rbegin(), v.rend());
    int drawn = 0;
    for (const auto& [id, L] : layers) if (draws_pixels(L.kind)) ++drawn;

    int n_png = 0, done = 0;
    const bool rgb = im.format.mode == P::Mode::RGB;
    formats::Progress cancel_only;
    cancel_only.cancelled = p.cancelled;
    std::ostringstream xml;
    xml << "<?xml version='1.0' encoding='UTF-8'?>\n<image version=\"0.0.5\" w=\"" << im.width << "\" h=\"" << im.height
        << "\" xres=\"" << fmt_num(im.dpi) << "\" yres=\"" << fmt_num(im.dpi) << "\">\n";
    std::function<bool(const std::string&, int)> stack = [&](const std::string& parent, int depth) -> bool {
        const auto it = kids.find(parent);
        if (it == kids.end()) return true;
        const std::string ind(static_cast<std::size_t>(depth) * 2, ' ');
        for (const auto& [pos, id] : it->second) {
            if (p.stop()) { error = "cancelled"; return false; }
            const Layer& L = layers.at(id);
            const std::string common = " name=\"" + escape(L.name) + "\" visibility=\"" + (L.visible ? "visible" : "hidden") + "\"";
            if (L.kind == Kind::Group) {
                const bool pass = L.blend == Blend::PassThrough;
                xml << ind << "<stack" << common << " opacity=\"" << fmt_num(L.opacity / 100.0) << "\" composite-op=\""
                    << composite_op(pass ? Blend::Normal : L.blend) << "\" isolation=\"" << (pass ? "auto" : "isolate")
                    << "\" x=\"0\" y=\"0\">\n";
                if (!stack(id, depth + 1)) return false;
                xml << ind << "</stack>\n";
                continue;
            }
            if (L.kind == Kind::Adjustment || L.kind == Kind::Filter) continue;   // plan(): left out
            if (!draws_pixels(L.kind)) continue;
            // Its pixels as ORA holds them: straight RGBA. A plain RGB pixel
            // layer goes as it is; anything with a look of its own (a mask,
            // live filters, styles, text, another mode) is drawn alone, at
            // full opacity in Normal, and that picture goes.
            const bool plain = L.kind == Kind::Pixel && rgb && !(L.mask && L.mask_on) && L.filters.empty() && !L.styles.any();
            P::TileMap m;
            double opacity = L.opacity / 100.0;
            if (plain) {
                if (L.pixels) m = L.pixels->format() == fmt ? L.pixels->without_outside() : transfer::converted(L.pixels->without_outside(), fmt);
                opacity *= L.fill / 100.0;
            } else {
                Layer solo = L;
                solo.parent = "";
                solo.pos = 1;
                solo.visible = true;
                solo.opacity = 100;
                solo.blend = Blend::Normal;
                solo.clipped = false;
                Image one = im;
                const std::map<Document::Id, Layer> alone{{id, solo}};
                auto flat = file::flatten(one, alone, cancel_only);
                if (!flat) { error = "cancelled"; return false; }
                m = flat->format() == fmt ? std::move(*flat) : transfer::converted(*flat, fmt);
            }
            int x = 0, y = 0;
            P::TileMap part(fmt, 1, 1);
            if (m.width() > 0) {
                const TileBox b = held(m);
                if (!b.empty()) { part = cut(m, b); x = b.c0 * P::kTile; y = b.r0 * P::kTile; }
            }
            std::vector<std::uint8_t> png;
            std::string err;
            if (!png_bytes(part, png, err, cancel_only, 6)) { error = err; return false; }
            const std::string src = "data/layer" + std::to_string(n_png++) + ".png";
            if (!add(src, std::move(png), ZIP_CM_STORE, 0)) { error = zip_strerror(z); return false; }
            xml << ind << "<layer" << common << " opacity=\"" << fmt_num(opacity) << "\" composite-op=\""
                << composite_op(L.blend) << "\" src=\"" << src << "\" x=\"" << x << "\" y=\"" << y << "\"/>\n";
            p.at(0.5 * ++done / std::max(1, drawn));
        }
        return true;
    };
    xml << " <stack>\n";
    if (!stack("", 2)) return false;
    xml << " </stack>\n</image>\n";
    if (!add("stack.xml", text(xml.str()), ZIP_CM_DEFLATE, 6)) { error = zip_strerror(z); return false; }

    // The picture, and its thumbnail.
    auto flat = file::flatten(im, layers, cancel_only);
    if (!flat) { error = "cancelled"; return false; }
    p.at(0.55);
    {
        std::string err;
        std::vector<std::uint8_t> merged, thumb;
        const P::TileMap m8 = flat->format() == fmt ? *flat : transfer::converted(*flat, fmt);
        if (!png_bytes(m8, merged, err, cancel_only, 6, im.dpi) || !png_bytes(thumbnail(*flat, 256), thumb, err, cancel_only, 6)) {
            error = err;
            return false;
        }
        if (!add("mergedimage.png", std::move(merged), ZIP_CM_STORE, 0) ||
            !add("Thumbnails/thumbnail.png", std::move(thumb), ZIP_CM_STORE, 0)) {
            error = zip_strerror(z);
            return false;
        }
    }
    p.at(0.6);
    ZipState st{&p};
    zip_register_progress_callback_with_state(z, 0.01, zip_progress, nullptr, &st);
    zip_register_cancel_callback_with_state(z, zip_cancel, nullptr, &st);
    if (zip_close(z) != 0) {
        error = p.stop() ? "cancelled" : std::string("can't be written: ") + zip_strerror(z);
        std::remove(tmp.c_str());
        return false;
    }
    zip.z = nullptr;
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::remove(tmp.c_str()); error = "can't be written: " + ec.message(); return false; }
    for (auto& n : plan(im, layers).left_out) notes.push_back(std::move(n));   // s050: what the dialog said
    p.at(1);
    return true;
}

}  // namespace klimt::core::ora
