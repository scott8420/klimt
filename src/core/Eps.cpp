// core::eps -- Export as EPS, our own PostScript writer (s080). See
// core/Eps.hpp.
#include "core/Eps.hpp"
#include "core/Clip.hpp"
#include "tint/Tint.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace klimt::core::eps {

namespace P = klimt::pixel;
using Bytes = std::vector<std::uint8_t>;

namespace {

// ASCII85, streamed: groups of four bytes as five characters ('z' for four
// zeros), lines at most 76, a line never starting with '%'.
class A85 {
public:
    explicit A85(std::ostream& o) : m_o(o) {}
    void put(std::uint8_t b) {
        m_t = m_t << 8 | b;
        if (++m_n == 4) { group(4); m_t = 0; m_n = 0; }
    }
    void put(const std::uint8_t* d, std::size_t n) { for (std::size_t i = 0; i < n; ++i) put(d[i]); }
    void end() {
        if (m_n) {
            const int n = m_n;
            for (int i = n; i < 4; ++i) m_t <<= 8;
            group(n);
        }
        if (m_col > 74) { m_o << '\n'; m_col = 0; }   // "~>" never split across lines
        m_o << "~>\n";
    }

private:
    void group(int n) {
        if (n == 4 && m_t == 0) { chars("z"); return; }
        char c[5];
        std::uint32_t v = m_t;
        for (int i = 4; i >= 0; --i) { c[i] = static_cast<char>('!' + v % 85); v /= 85; }
        chars(std::string(c, static_cast<std::size_t>(n + 1)));
    }
    void chars(const std::string& s) {
        for (char ch : s) {
            if (m_col >= 76) { m_o << '\n'; m_col = 0; }
            if (m_col == 0 && ch == '%') { m_o << ' '; ++m_col; }
            m_o << ch;
            ++m_col;
        }
    }
    std::ostream& m_o;
    std::uint32_t m_t = 0;
    int           m_n = 0, m_col = 0;
};

void le16(Bytes& b, unsigned v) { b.push_back(static_cast<std::uint8_t>(v)); b.push_back(static_cast<std::uint8_t>(v >> 8)); }
void le32(Bytes& b, std::uint32_t v) { for (int s = 0; s < 32; s += 8) b.push_back(static_cast<std::uint8_t>(v >> s)); }
void le32_at(std::string& s, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) s[at + static_cast<std::size_t>(i)] = static_cast<char>(v >> (8 * i));
}

std::string num(double v) {
    std::ostringstream o;
    o.imbue(std::locale::classic());
    const double r = std::round(v * 1000) / 1000;
    o << std::fixed << std::setprecision(3) << (r == 0 ? 0.0 : r);
    std::string s = o.str();
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s.empty() ? "0" : s;
}

// PostScript strings: ( ) and \ escaped, 7-bit kept, the rest '?'.
std::string ps_text(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size() && o.size() < 200; ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x80) {
            while (i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) ++i;
            o.push_back('?');
        } else if (c < 0x20) {
            o.push_back(' ');
        } else {
            if (c == '(' || c == ')' || c == '\\') o.push_back('\\');
            o.push_back(static_cast<char>(c));
        }
    }
    return o;
}

// The CMYK profile the image goes into: the document's own (a CMYK
// document), else the working CMYK, else the plain conversion.
tint::Icc cmyk_profile(const Image& im) {
    if (im.icc && im.format.mode == P::Mode::CMYK && tint::inspect(*im.icc).ok) return *im.icc;
    if (const auto w = tint::working(tint::Space::CMYK)) return *w;
    return tint::plain_cmyk();
}
// What the flattened RGB numbers mean: an RGB document's profile, else sRGB.
tint::Icc rgb_profile(const Image& im) {
    if (im.icc && im.format.mode == P::Mode::RGB && tint::inspect(*im.icc).ok) return *im.icc;
    return tint::srgb();
}

std::uint8_t to8(float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); }

}  // namespace

// ── the pieces ──────────────────────────────────────────────────────────────

Bytes run_length(const Bytes& in) {
    Bytes out;
    out.reserve(in.size() + in.size() / 128 + 2);
    const std::size_t n = in.size();
    // A run worth a repeat: three alike (two alike cost as much either way,
    // and breaking a literal for them costs a length byte).
    const auto run_at = [&](std::size_t i) { return i + 2 < n && in[i] == in[i + 1] && in[i] == in[i + 2]; };
    std::size_t i = 0;
    while (i < n) {
        if (run_at(i)) {
            std::size_t r = 3;
            while (i + r < n && r < 128 && in[i + r] == in[i]) ++r;
            out.push_back(static_cast<std::uint8_t>(257 - r));
            out.push_back(in[i]);
            i += r;
            continue;
        }
        std::size_t j = i + 1;   // a literal stretch, up to the next run (or 128 bytes)
        while (j < n && j - i < 128 && !run_at(j)) ++j;
        out.push_back(static_cast<std::uint8_t>(j - i - 1));
        out.insert(out.end(), in.begin() + static_cast<long>(i), in.begin() + static_cast<long>(j));
        i = j;
    }
    out.push_back(128);
    return out;
}

Bytes un_run_length(const Bytes& in) {
    Bytes out;
    std::size_t i = 0;
    while (i < in.size()) {
        const unsigned l = in[i++];
        if (l == 128) break;
        if (l < 128) {
            const std::size_t k = std::min<std::size_t>(l + 1, in.size() - i);
            out.insert(out.end(), in.begin() + static_cast<long>(i), in.begin() + static_cast<long>(i + k));
            i += k;
        } else if (i < in.size()) {
            out.insert(out.end(), 257 - l, in[i++]);
        }
    }
    return out;
}

std::string ascii85(const Bytes& in) {
    std::ostringstream o;
    A85 a(o);
    a.put(in.data(), in.size());
    a.end();
    return o.str();
}

Bytes un_ascii85(const std::string& in) {
    Bytes out;
    std::uint32_t t = 0;
    int n = 0;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '~') break;
        if (c == 'z' && n == 0) { out.insert(out.end(), 4, 0); continue; }
        if (c < '!' || c > 'u') continue;   // whitespace
        t = t * 85 + static_cast<std::uint32_t>(c - '!');
        if (++n == 5) {
            for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(t >> s));
            t = 0; n = 0;
        }
    }
    if (n > 1) {
        const int k = n - 1;
        for (int i = n; i < 5; ++i) t = t * 85 + 84;
        for (int s = 24, j = 0; j < k; s -= 8, ++j) out.push_back(static_cast<std::uint8_t>(t >> s));
    }
    return out;
}

std::string clip_ps(const path::Path& p, int height, double dpi) {
    const double s = 72.0 / (dpi > 0 ? dpi : 72.0);
    const auto pt = [&](const path::P& q) { return num(q.x * s) + " " + num((height - q.y) * s); };
    std::string o = "newpath\n";
    for (const auto& sub : p.subs) {
        if (sub.nodes.size() < 2) continue;
        const auto& ns = sub.nodes;
        o += pt(ns[0].at) + " moveto\n";
        const std::size_t segs = ns.size();   // open subpaths close too (Photoshop's way, and the mask's)
        for (std::size_t i = 0; i < segs; ++i) {
            const auto& a = ns[i];
            const auto& b = ns[(i + 1) % ns.size()];
            if (a.out == a.at && b.in == b.at) o += pt(b.at) + " lineto\n";
            else o += pt(a.out) + " " + pt(b.in) + " " + pt(b.at) + " curveto\n";
        }
        o += "closepath\n";
    }
    o += "eoclip newpath\n";
    return o;
}

Bytes tiff_rgb(const Bytes& rgb, int w, int h, double dpi) {
    Bytes t;
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(w) * static_cast<std::uint32_t>(h) * 3;
    constexpr int kEntries = 13;
    const std::uint32_t ifd = 8, ifd_len = 2 + kEntries * 12 + 4;
    const std::uint32_t bps_at = ifd + ifd_len, xres_at = bps_at + 6, yres_at = xres_at + 8, data_at = yres_at + 8;
    t.insert(t.end(), {'I', 'I'});
    le16(t, 42);
    le32(t, ifd);
    le16(t, kEntries);
    const auto entry = [&](unsigned tag, unsigned type, std::uint32_t count, std::uint32_t value) {
        le16(t, tag); le16(t, type); le32(t, count);
        if (type == 3 && count == 1) { le16(t, value); le16(t, 0); }
        else le32(t, value);
    };
    entry(256, 4, 1, static_cast<std::uint32_t>(w));   // ImageWidth
    entry(257, 4, 1, static_cast<std::uint32_t>(h));   // ImageLength
    entry(258, 3, 3, bps_at);                          // BitsPerSample 8 8 8
    entry(259, 3, 1, 1);                               // no compression
    entry(262, 3, 1, 2);                               // RGB
    entry(273, 4, 1, data_at);                         // StripOffsets
    entry(274, 3, 1, 1);                               // top-left
    entry(277, 3, 1, 3);                               // SamplesPerPixel
    entry(278, 4, 1, static_cast<std::uint32_t>(h));   // RowsPerStrip: one strip
    entry(279, 4, 1, data_bytes);                      // StripByteCounts
    entry(282, 5, 1, xres_at);
    entry(283, 5, 1, yres_at);
    entry(296, 3, 1, 2);                               // inches
    le32(t, 0);                                        // no next IFD
    le16(t, 8); le16(t, 8); le16(t, 8);
    const auto r = static_cast<std::uint32_t>(std::lround(std::clamp(dpi, 1.0, 10000.0) * 100));
    le32(t, r); le32(t, 100);
    le32(t, r); le32(t, 100);
    t.insert(t.end(), rgb.begin(), rgb.begin() + std::min<std::size_t>(rgb.size(), data_bytes));
    if (rgb.size() < data_bytes) t.insert(t.end(), data_bytes - rgb.size(), 255);
    return t;
}

// ── the plan ────────────────────────────────────────────────────────────────

ora::Plan plan(const Image& im, const std::map<Document::Id, Layer>& layers, const formats::WriteOptions& o) {
    ora::Plan pl;
    if (const auto id = clip::of(im, layers); !id.empty()) {
        const double f = im.clip_flatness;
        pl.kept.push_back("the clipping path “" + layers.at(id).name + "”" +
                          (f > 0 ? " (flatness " + num(f) + ")" : "") + " -- the picture is cut out where it's placed");
    } else {
        pl.left_out.push_back("no clipping path -- the whole rectangle shows (Select > Clipping Path…)");
    }
    int n = 0, paths = 0;
    for (const auto& [id, l] : layers) {
        if (l.kind == Kind::Path) ++paths;
        else if (l.kind != Kind::Group) ++n;
    }
    if (n > 1) pl.baked.push_back(std::to_string(n) + " layers flattened into one picture");
    if (o.eps_cmyk) pl.baked.push_back("as CMYK (" + tint::inspect(cmyk_profile(im)).name + ")");
    if (paths) pl.kept.push_back(paths == 1 ? std::string("the path as a Photoshop path too") : std::to_string(paths) + " paths as Photoshop paths too");
    pl.left_out.push_back("transparency -- white behind (EPS has none)");
    if (im.format.depth != P::Depth::U8) pl.left_out.push_back("written as 8-bit");
    return pl;
}

// ── the file ────────────────────────────────────────────────────────────────

bool save(const std::string& path, const Image& im, const std::map<Document::Id, Layer>& layers,
          const std::string& title, const formats::WriteOptions& o, std::string& error,
          std::vector<std::string>& notes, const formats::Progress& p) {
    const int W = im.width, H = im.height;
    if (W < 1 || H < 1) { error = "an empty image"; return false; }
    formats::Progress first = p;
    first.progress = [&p](double f) { p.at(f * 0.5); };
    auto flat = file::flatten(im, layers, first);
    if (!flat) { error = "cancelled"; return false; }

    // The samples: 8-bit, matted with white; CMYK through the link.
    const int nc = o.eps_cmyk ? 4 : 3;
    Bytes data(static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * static_cast<std::size_t>(nc));
    std::shared_ptr<const tint::Link> link;
    std::string cmyk_name;
    if (o.eps_cmyk) {
        const auto to = cmyk_profile(im);
        link = tint::link(rgb_profile(im), tint::Space::RGB, to, tint::Space::CMYK);
        if (!link) { error = "the CMYK profile can't be used"; return false; }
        cmyk_name = tint::inspect(to).name;
    }
    std::vector<float> rgb(static_cast<std::size_t>(W) * 3), cmyk(static_cast<std::size_t>(W) * 4);
    for (int y = 0; y < H; ++y) {
        if (p.stop()) { error = "cancelled"; return false; }
        for (int x = 0; x < W; ++x) {
            const P::Px s = flat->get(x, y);
            const float a = std::clamp(s.c[3], 0.f, 1.f);
            for (int c = 0; c < 3; ++c) rgb[static_cast<std::size_t>(x) * 3 + static_cast<std::size_t>(c)] = std::clamp(s.c[static_cast<std::size_t>(c)], 0.f, 1.f) * a + 1 - a;
        }
        std::uint8_t* row = data.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(W) * static_cast<std::size_t>(nc);
        if (link) {
            link->run(rgb.data(), cmyk.data(), static_cast<std::size_t>(W));
            for (std::size_t i = 0; i < cmyk.size(); ++i) row[i] = to8(cmyk[i]);
        } else {
            for (std::size_t i = 0; i < rgb.size(); ++i) row[i] = to8(rgb[i]);
        }
        if ((y & 63) == 0) p.at(0.5 + 0.2 * y / H);
    }

    // The preview: 72 ppi, the long side at most 1024, box-filtered.
    Bytes preview;
    int pw = 0, ph = 0;
    if (o.eps_preview) {
        const double s = std::min({1.0, 72.0 / std::max(1.0, im.dpi), 1024.0 / std::max(W, H)});
        pw = std::max(1, static_cast<int>(std::lround(W * s)));
        ph = std::max(1, static_cast<int>(std::lround(H * s)));
        Bytes px(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph) * 3);
        for (int y = 0; y < ph; ++y) {
            const int y0 = y * H / ph, y1 = std::max(y0 + 1, (y + 1) * H / ph);
            for (int x = 0; x < pw; ++x) {
                const int x0 = x * W / pw, x1 = std::max(x0 + 1, (x + 1) * W / pw);
                double sum[3] = {0, 0, 0};
                const int step = std::max(1, std::max(x1 - x0, y1 - y0) / 4);   // at most ~16 samples a cell
                int cnt = 0;
                for (int yy = y0; yy < y1; yy += step)
                    for (int xx = x0; xx < x1; xx += step) {
                        const P::Px q = flat->get(xx, yy);
                        const float a = std::clamp(q.c[3], 0.f, 1.f);
                        for (int c = 0; c < 3; ++c) sum[c] += std::clamp(q.c[static_cast<std::size_t>(c)], 0.f, 1.f) * a + 1 - a;
                        ++cnt;
                    }
                for (int c = 0; c < 3; ++c)
                    px[(static_cast<std::size_t>(y) * static_cast<std::size_t>(pw) + static_cast<std::size_t>(x)) * 3 + static_cast<std::size_t>(c)] =
                        to8(static_cast<float>(sum[c] / std::max(1, cnt)));
            }
        }
        preview = tiff_rgb(px, pw, ph, im.dpi * pw / W);
    }
    p.at(0.75);

    // The encoded image data.
    Bytes packed;
    if (o.eps_flate) {
        uLongf n = compressBound(static_cast<uLong>(data.size()));
        packed.resize(n);
        if (compress2(packed.data(), &n, data.data(), static_cast<uLong>(data.size()), 6) != Z_OK) { error = "compression failed"; return false; }
        packed.resize(n);
    } else {
        packed = run_length(data);
    }
    Bytes().swap(data);
    if (p.stop()) { error = "cancelled"; return false; }

    // ── the PostScript ──
    const double s = 72.0 / (im.dpi > 0 ? im.dpi : 72.0);
    const double wpt = W * s, hpt = H * s;
    const auto clip_id = clip::of(im, layers);
    const auto res = clip::resources(im, layers);
    std::ostringstream ps;
    ps.imbue(std::locale::classic());
    ps << "%!PS-Adobe-3.0 EPSF-3.0\n"
       << "%%Creator: Klimt\n"
       << "%%Title: (" << ps_text(title) << ")\n"
       << "%%BoundingBox: 0 0 " << static_cast<long>(std::ceil(wpt - 1e-6)) << ' ' << static_cast<long>(std::ceil(hpt - 1e-6)) << '\n'
       << "%%HiResBoundingBox: 0 0 " << num(wpt) << ' ' << num(hpt) << '\n'
       << "%%LanguageLevel: " << (o.eps_flate ? 3 : 2) << '\n'
       << "%%DocumentData: Clean7Bit\n";
    if (o.eps_cmyk) ps << "%%DocumentProcessColors: Cyan Magenta Yellow Black\n";
    ps << "%%Pages: 1\n%%EndComments\n";
    if (!res.empty()) {   // Photoshop's resources, as Photoshop writes them: hex in comments
        ps << "%BeginPhotoshop: " << res.size() << '\n';
        static const char* hex = "0123456789ABCDEF";
        for (std::size_t i = 0; i < res.size(); i += 32) {
            ps << "% ";
            for (std::size_t k = i; k < std::min(res.size(), i + 32); ++k) ps << hex[res[k] >> 4] << hex[res[k] & 15];
            ps << '\n';
        }
        ps << "%EndPhotoshop\n";
    }
    ps << "%%BeginProlog\n%%EndProlog\n%%Page: 1 1\n"
       << "save\n10 dict begin\n";
    if (!clip_id.empty()) {
        ps << "% the clipping path: " << ps_text(layers.at(clip_id).name) << '\n';
        if (im.clip_flatness > 0) ps << num(im.clip_flatness) << " setflat\n";
        ps << clip_ps(*layers.at(clip_id).path, H, im.dpi);
    }
    ps << num(wpt) << ' ' << num(hpt) << " scale\n"
       << (o.eps_cmyk ? "/DeviceCMYK" : "/DeviceRGB") << " setcolorspace\n"
       << "{ /KlimtData currentfile /ASCII85Decode filter def\n"
       << "  << /ImageType 1 /Width " << W << " /Height " << H << " /BitsPerComponent 8\n"
       << "     /Decode [" << (o.eps_cmyk ? "0 1 0 1 0 1 0 1" : "0 1 0 1 0 1") << "]\n"
       << "     /ImageMatrix [" << W << " 0 0 -" << H << " 0 " << H << "]\n"
       << "     /DataSource KlimtData " << (o.eps_flate ? "/FlateDecode" : "/RunLengthDecode") << " filter >> image\n"
       << "  KlimtData flushfile } exec\n";
    {
        A85 a(ps);
        a.put(packed.data(), packed.size());
        a.end();
    }
    ps << "end\nrestore\nshowpage\n%%Trailer\n%%EOF\n";
    Bytes().swap(packed);
    p.at(0.95);

    // ── the file: [DOS EPS header] PostScript [TIFF] ──
    std::string body = ps.str();
    const std::string tmp = path + ".partial";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { error = "can't be written"; return false; }
        if (!preview.empty()) {
            std::string head(30, '\0');
            head[0] = static_cast<char>(0xC5); head[1] = static_cast<char>(0xD0); head[2] = static_cast<char>(0xD3); head[3] = static_cast<char>(0xC6);
            le32_at(head, 4, 30);
            le32_at(head, 8, static_cast<std::uint32_t>(body.size()));
            le32_at(head, 20, static_cast<std::uint32_t>(30 + body.size()));
            le32_at(head, 24, static_cast<std::uint32_t>(preview.size()));
            head[28] = static_cast<char>(0xFF); head[29] = static_cast<char>(0xFF);   // no checksum
            out.write(head.data(), static_cast<std::streamsize>(head.size()));
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!preview.empty()) out.write(reinterpret_cast<const char*>(preview.data()), static_cast<std::streamsize>(preview.size()));
        if (!out) { out.close(); std::remove(tmp.c_str()); error = "can't be written (the disk full?)"; return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::remove(tmp.c_str()); error = "can't be written: " + ec.message(); return false; }

    notes.push_back(o.eps_cmyk ? "CMYK (" + cmyk_name + ")" : "RGB");
    if (!clip_id.empty()) notes.push_back("clipping path “" + layers.at(clip_id).name + "”");
    else notes.push_back("no clipping path");
    if (!preview.empty()) notes.push_back("a TIFF preview " + std::to_string(pw) + " × " + std::to_string(ph));
    p.at(1);
    return true;
}

}  // namespace klimt::core::eps
