// core/RoundTrip -- the Composix round-trip (s083). See core/RoundTrip.hpp
// and docs/round-trip.md.
#include "core/RoundTrip.hpp"
#include "core/Eps.hpp"
#include "core/Ora.hpp"
#include "core/Pdf.hpp"
#include "core/Psd.hpp"
#include "core/Transfer.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

namespace klimt::core::roundtrip {

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<std::uint8_t>;

bool slurp(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool is_ws(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0'; }

// The number after the LAST "startxref" (nullopt: none).
std::optional<std::size_t> startxref(const std::string& s) {
    const auto at = s.rfind("startxref");
    if (at == std::string::npos) return std::nullopt;
    std::size_t i = at + 9;
    while (i < s.size() && is_ws(s[i])) ++i;
    std::size_t v = 0;
    bool any = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { v = v * 10 + static_cast<std::size_t>(s[i] - '0'); ++i; any = true; }
    if (!any || v >= s.size()) return std::nullopt;
    return v;
}

// The dictionary "<< ... >>" starting at or after `from` (balanced; a hex
// string "<...>" and a literal "(...)" skipped whole). "" when none.
std::string dict_at(const std::string& s, std::size_t from) {
    const auto open = s.find("<<", from);
    if (open == std::string::npos) return {};
    int depth = 0;
    for (std::size_t i = open; i + 1 < s.size();) {
        if (s[i] == '<' && s[i + 1] == '<') { ++depth; i += 2; continue; }
        if (s[i] == '>' && s[i + 1] == '>') {
            --depth; i += 2;
            if (depth == 0) return s.substr(open, i - open);
            continue;
        }
        if (s[i] == '<') { const auto e = s.find('>', i + 1); if (e == std::string::npos) return {}; i = e + 1; continue; }
        if (s[i] == '(') {   // a literal string: balanced parentheses, backslash escapes
            int d = 0;
            for (; i < s.size(); ++i) {
                if (s[i] == '\\') { ++i; continue; }
                if (s[i] == '(') ++d;
                else if (s[i] == ')' && --d == 0) break;
            }
            ++i;
            continue;
        }
        ++i;
    }
    return {};
}

// "/Key N G R" in a dictionary's text: N (nullopt: absent).
std::optional<long> ref_of(const std::string& d, const std::string& key) {
    for (std::size_t at = d.find(key); at != std::string::npos; at = d.find(key, at + 1)) {
        std::size_t i = at + key.size();
        if (i < d.size() && (std::isalnum(static_cast<unsigned char>(d[i])) || d[i] == '#')) continue;   // a longer name
        while (i < d.size() && is_ws(d[i])) ++i;
        long n = 0;
        bool any = false;
        while (i < d.size() && std::isdigit(static_cast<unsigned char>(d[i]))) { n = n * 10 + (d[i] - '0'); ++i; any = true; }
        if (any) return n;
    }
    return std::nullopt;
}

// "/Key <value>" in a dictionary's text, its value's text whole ("12 0 R",
// "[<ab> <cd>]", "300"). "" when absent.
std::string value_of(const std::string& d, const std::string& key) {
    for (std::size_t at = d.find(key); at != std::string::npos; at = d.find(key, at + 1)) {
        std::size_t i = at + key.size();
        if (i < d.size() && (std::isalnum(static_cast<unsigned char>(d[i])) || d[i] == '#')) continue;
        while (i < d.size() && is_ws(d[i])) ++i;
        if (i >= d.size()) return {};
        if (d[i] == '[') {
            const auto e = d.find(']', i);
            return e == std::string::npos ? std::string{} : d.substr(i, e - i + 1);
        }
        // A name ("/FlateDecode"), a number, or "N G R".
        std::size_t e = d[i] == '/' ? i + 1 : i;
        while (e < d.size() && !is_ws(d[e]) && d[e] != '/' && d[e] != '>') ++e;
        std::string v = d.substr(i, e - i);
        std::size_t j = e;
        while (j < d.size() && is_ws(d[j])) ++j;
        std::size_t k = j;
        while (k < d.size() && std::isdigit(static_cast<unsigned char>(d[k]))) ++k;
        if (k > j) {
            std::size_t r = k;
            while (r < d.size() && is_ws(d[r])) ++r;
            if (r < d.size() && d[r] == 'R') v = d.substr(i, r + 1 - i);
        }
        return v;
    }
    return {};
}

bool inflate_all(const std::uint8_t* in, std::size_t n, Bytes& out) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(in);
    z.avail_in = static_cast<uInt>(n);
    std::uint8_t buf[1 << 16];
    int r = Z_OK;
    while (r == Z_OK) {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
    }
    inflateEnd(&z);
    return r == Z_STREAM_END;
}

// The stream of the object whose dictionary starts at `dict_at_pos` (its text
// `d`): raw, or inflated when FlateDecode'd. A /Length given directly only.
std::optional<Bytes> stream_of(const std::string& s, std::size_t dict_pos, const std::string& d) {
    const std::string len = value_of(d, "/Length");
    if (len.empty() || len.find('R') != std::string::npos) return std::nullopt;
    const long n = std::strtol(len.c_str(), nullptr, 10);
    if (n < 0) return std::nullopt;
    std::size_t i = s.find("stream", dict_pos + d.size());
    if (i == std::string::npos) return std::nullopt;
    i += 6;
    if (i < s.size() && s[i] == '\r') ++i;
    if (i < s.size() && s[i] == '\n') ++i;
    if (i + static_cast<std::size_t>(n) > s.size()) return std::nullopt;
    const auto* p = reinterpret_cast<const std::uint8_t*>(s.data()) + i;
    const std::string filter = value_of(d, "/Filter");
    if (filter.empty()) return Bytes(p, p + n);
    if (filter == "/FlateDecode" || filter == "[/FlateDecode]") {
        Bytes out;
        if (!inflate_all(p, static_cast<std::size_t>(n), out)) return std::nullopt;
        return out;
    }
    return std::nullopt;   // another filter: not ours to undo
}

// Is this the .klimt? (a zip whose first entry is "mimetype" = image/x-klimt)
bool looks_klimt(const Bytes& b) {
    return b.size() > 51 && b[0] == 'P' && b[1] == 'K' && std::memcmp(b.data() + 30, "mimetype", 8) == 0 &&
           std::memcmp(b.data() + 38, "image/x-klimt", 13) == 0;
}

std::optional<Bytes> klimt_object(const std::string& s, long n) {
    // The LAST definition of object n (an update supersedes).
    const std::string head = std::to_string(n) + " 0 obj";
    for (std::size_t at = s.rfind(head); at != std::string::npos; at = at ? s.rfind(head, at - 1) : std::string::npos) {
        if (at > 0 && !is_ws(s[at - 1])) continue;   // "10 0 obj" when looking for "0 0 obj"
        const std::string d = dict_at(s, at);
        if (d.empty()) return std::nullopt;
        auto b = stream_of(s, s.find("<<", at), d);
        if (b && looks_klimt(*b)) return b;
        return std::nullopt;
    }
    return std::nullopt;
}

std::string temp_beside(const std::string& path, const char* what) {
    return path + "." + what + "-" + std::to_string(::getpid());
}

bool replace_with(const std::string& tmp, const std::string& path, std::string& error) {
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { std::remove(tmp.c_str()); error = "can't be written: " + ec.message(); return false; }
    return true;
}

}  // namespace

// ── The document inside a PDF ──────────────────────────────────────────────
bool embed(const std::string& pdf, const Bytes& klimt, std::string& error) {
    std::string s;
    if (!slurp(pdf, s) || s.rfind("%PDF-", 0) != 0) { error = "not a PDF"; return false; }
    const auto sx = startxref(s);
    if (!sx) { error = "a PDF without a cross-reference"; return false; }
    // The last trailer: a classic table's "trailer << >>", or the xref stream's dictionary.
    const bool table = s.compare(*sx, 4, "xref") == 0;
    std::string trailer;
    if (table) {
        const auto t = s.find("trailer", *sx);
        if (t == std::string::npos) { error = "a PDF without a trailer"; return false; }
        trailer = dict_at(s, t);
    } else {
        trailer = dict_at(s, *sx);
        if (trailer.find("/XRef") == std::string::npos) trailer.clear();
    }
    const auto size = ref_of(trailer, "/Size");
    const std::string root = value_of(trailer, "/Root");
    if (trailer.empty() || !size || root.empty()) { error = "a PDF whose trailer can't be read"; return false; }
    const std::string info = value_of(trailer, "/Info");
    const std::string id = value_of(trailer, "/ID");

    const long n = *size;   // the document's object
    std::string up;
    if (s.back() != '\n') up += '\n';
    const std::size_t base = s.size();
    const std::size_t obj_at = base + up.size();
    up += std::to_string(n) + " 0 obj\n<< /Type /KlimtDocument /Subtype /application#2Fx-klimt /Length " +
          std::to_string(klimt.size()) + " >>\nstream\n";
    up.append(reinterpret_cast<const char*>(klimt.data()), klimt.size());
    up += "\nendstream\nendobj\n";
    std::string keys = " /Root " + root + (info.empty() ? "" : " /Info " + info) + (id.empty() ? "" : " /ID " + id) +
                       " /Prev " + std::to_string(*sx) + " /KlimtDocument " + std::to_string(n) + " 0 R";
    const std::size_t xref_at = base + up.size();
    if (table) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", obj_at);
        up += "xref\n" + std::to_string(n) + " 1\n" + line + "trailer\n<< /Size " + std::to_string(n + 1) + keys + " >>\n";
    } else {
        // An xref stream: n (the document) and n + 1 (itself), each 1 + 4 + 2 bytes.
        std::string rows;
        for (const std::size_t off : {obj_at, xref_at}) {
            rows += '\x01';
            for (int b = 3; b >= 0; --b) rows += static_cast<char>((off >> (8 * b)) & 0xFF);
            rows += std::string(2, '\0');
        }
        up += std::to_string(n + 1) + " 0 obj\n<< /Type /XRef /Size " + std::to_string(n + 2) + keys + " /Index [" +
              std::to_string(n) + " 2] /W [1 4 2] /Length " + std::to_string(rows.size()) + " >>\nstream\n" + rows +
              "\nendstream\nendobj\n";
    }
    if (xref_at > 0xFFFFFFFFu) { error = "a PDF past 4 GB"; return false; }
    up += "startxref\n" + std::to_string(xref_at) + "\n%%EOF\n";
    std::ofstream out(pdf, std::ios::binary | std::ios::app);
    if (!out) { error = "can't be written"; return false; }
    out.write(up.data(), static_cast<std::streamsize>(up.size()));
    out.close();
    if (!out) {   // a short write: back to what it was
        std::error_code ec;
        fs::resize_file(pdf, base, ec);
        error = "can't be written";
        return false;
    }
    return true;
}

std::optional<Bytes> embedded(const std::string& pdf) {
    std::string s;
    if (!slurp(pdf, s) || s.rfind("%PDF-", 0) != 0) return std::nullopt;
    // The last trailer naming it.
    if (const auto sx = startxref(s)) {
        const bool table = s.compare(*sx, 4, "xref") == 0;
        const std::string t = table ? dict_at(s, s.find("trailer", *sx)) : dict_at(s, *sx);
        if (const auto n = ref_of(t, "/KlimtDocument"))
            if (auto b = klimt_object(s, *n)) return b;
    }
    // Else the last stream that says it is one (a rewrite may have renumbered it).
    for (std::size_t at = s.rfind("/KlimtDocument"); at != std::string::npos; at = at ? s.rfind("/KlimtDocument", at - 1) : std::string::npos) {
        const auto open = s.rfind("<<", at);
        if (open == std::string::npos) break;
        const std::string d = dict_at(s, open);
        if (d.find("/Type") != std::string::npos && d.size() + open > at) {
            if (auto b = stream_of(s, open, d); b && looks_klimt(*b)) return b;
        }
        if (at == 0) break;
    }
    return std::nullopt;
}

std::optional<file::Contents> open_pdf(const std::string& path, std::string& error, const formats::Progress& p) {
    const auto b = embedded(path);
    if (!b) {
        error = "a PDF Klimt didn't write (Klimt opens the PDFs it writes -- their layers ride inside; it doesn't read PDF pages)";
        return std::nullopt;
    }
    const std::string tmp = (fs::temp_directory_path() / ("klimt-pdf-" + std::to_string(::getpid()) + "-" +
                                                          fs::path(path).stem().string() + ".klimt")).string();
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(b->data()), static_cast<std::streamsize>(b->size()));
        if (!out) { error = "no room to read it"; return std::nullopt; }
    }
    auto c = file::open(tmp, error, p);
    std::remove(tmp.c_str());
    if (!c) { error = "its Klimt layers: " + error; return std::nullopt; }
    c->kind = formats::Kind::PDF;
    c->view = {};   // the PDF's own view isn't kept
    c->notes.push_back("Klimt's layers, kept in the PDF");
    return c;
}

// ── Writing back ───────────────────────────────────────────────────────────
const char* writes_back(formats::Kind kind) {
    switch (kind) {
        case formats::Kind::PDF: return "PDF";
        case formats::Kind::PSD: return "PSD";
        case formats::Kind::TIFF: return "TIFF";
        case formats::Kind::PNG: return "PNG";
        case formats::Kind::JPEG: return "JPEG";
        case formats::Kind::WebP: return "WebP";
        case formats::Kind::ORA: return "OpenRaster";
        case formats::Kind::Klimt: return "Klimt";
        case formats::Kind::EPS: return "EPS";
        case formats::Kind::Unknown: break;
    }
    return nullptr;
}

formats::Kind kind_of(const std::string& path) {
    const auto k = formats::sniff(path);
    if (k != formats::Kind::Unknown) return k;
    std::ifstream in(path, std::ios::binary);
    char b[16] = {};
    in.read(b, sizeof b);
    if (std::memcmp(b, "%!PS-Adobe", 10) == 0 || (static_cast<unsigned char>(b[0]) == 0xC5 && static_cast<unsigned char>(b[1]) == 0xD0))
        return formats::Kind::EPS;
    return formats::Kind::Unknown;
}

bool save_back(const std::string& path, formats::Kind kind, const file::Contents& c, std::string& error,
               std::vector<std::string>& notes, const formats::Progress& p, const formats::WriteOptions& o) {
    if (!writes_back(kind)) { error = "Klimt can't write that kind of file"; return false; }
    const std::string title = fs::path(path).stem().string();
    const std::string tmp = temp_beside(path, "saving");
    std::remove(tmp.c_str());
    bool ok = false;
    switch (kind) {
        case formats::Kind::Klimt: ok = file::save_klimt(tmp, c, error, p); break;
        case formats::Kind::PSD: ok = psd::save(tmp, c.image, c.layers, error, notes, p); break;
        case formats::Kind::ORA: ok = ora::save(tmp, c.image, c.layers, error, notes, p); break;
        case formats::Kind::EPS: ok = eps::save(tmp, c.image, c.layers, title, o, error, notes, p); break;
        case formats::Kind::PDF: {
            // The page (the first 70 %), then the document inside it.
            formats::Progress page;
            page.cancelled = p.cancelled;
            page.progress = [&p](double f) { p.at(0.7 * f); };
            ok = pdf::save(tmp, c.image, c.layers, title, error, notes, page);
            if (!ok) break;
            const std::string doc = temp_beside(path, "doc");
            formats::Progress rest;
            rest.cancelled = p.cancelled;
            rest.progress = [&p](double f) { p.at(0.7 + 0.25 * f); };
            ok = file::save_klimt(doc, c, error, rest);
            std::string bytes;
            if (ok && !slurp(doc, bytes)) { ok = false; error = "can't be written"; }
            std::remove(doc.c_str());
            if (ok) ok = embed(tmp, Bytes(bytes.begin(), bytes.end()), error);
            if (ok) notes.push_back("Klimt's layers kept inside it");
            break;
        }
        default: {   // one picture
            ok = file::export_image(tmp, kind, c.image, c.layers, error, p, o);
            std::size_t drawn = 0;
            for (const auto& [id, L] : c.layers)
                if (!id.empty() && id[0] != '@' && L.parent.empty()) ++drawn;
            if (ok && drawn > 1) notes.push_back(std::string("flattened -- a ") + writes_back(kind) + " holds one picture");
            if (ok && kind == formats::Kind::JPEG) notes.push_back("JPEG compressed again");
            break;
        }
    }
    if (!ok) {
        std::remove(tmp.c_str());
        if (p.stop()) error = "cancelled";
        return false;
    }
    if (!replace_with(tmp, path, error)) return false;
    p.at(1);
    return true;
}

// ── Edit Contents ──────────────────────────────────────────────────────────
file::Contents contents_of(const smart::Smart& s, const Image& owner) {
    file::Contents c;
    c.kind = formats::Kind::Klimt;
    if (!s.source) return c;
    c.image.width = s.source->width();
    c.image.height = s.source->height();
    c.image.format = s.source->format();
    c.image.icc = owner.icc;
    c.image.dpi = s.dpi > 0 ? s.dpi : owner.dpi;
    Layer L;
    L.name = s.name.empty() ? std::string("Picture") : s.name;
    L.pos = 1;
    L.pixels = s.source;
    c.layers["L1"] = std::move(L);
    c.next_id = 2;
    return c;
}

std::optional<pixel::TileMap> picture_of(const file::Contents& c, const pixel::Format& f, const formats::Progress& p) {
    auto flat = file::flatten(c.image, c.layers, p);
    if (!flat) return std::nullopt;
    if (flat->format() == f) return flat;
    return transfer::converted(*flat, f);
}

}  // namespace klimt::core::roundtrip
