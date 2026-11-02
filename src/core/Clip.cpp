// core::clip -- the clipping path and Photoshop's path resources (s080).
// See core/Clip.hpp.
#include "core/Clip.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace klimt::core::clip {

namespace {

using Bytes = std::vector<std::uint8_t>;

void be16(Bytes& b, unsigned v) { b.push_back(static_cast<std::uint8_t>(v >> 8)); b.push_back(static_cast<std::uint8_t>(v)); }
void be32(Bytes& b, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
unsigned rd16(const std::uint8_t* p) { return static_cast<unsigned>(p[0]) << 8 | p[1]; }
std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

// A fraction of the side as signed 8.24.
std::uint32_t fixed824(double v, int side) {
    const double f = side > 0 ? v / side : 0;
    const double s = std::clamp(std::round(f * 16777216.0), -2147483648.0, 2147483647.0);
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(s));
}
double unfixed824(std::uint32_t v, int side) {
    return static_cast<double>(static_cast<std::int32_t>(v)) / 16777216.0 * side;
}

void point(Bytes& b, const path::P& p, int w, int h) {
    be32(b, fixed824(p.y, h));   // vertical first
    be32(b, fixed824(p.x, w));
}

// A layer's name as Photoshop's (Mac Roman) bytes: ASCII kept, the rest '?'.
// At most 63 -- a Pascal string, and Photoshop's own path-name limit.
std::string pascal_text(const std::string& utf8) {
    std::string out;
    for (std::size_t i = 0; i < utf8.size() && out.size() < 63; ++i) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) { out.push_back(c >= 0x20 ? static_cast<char>(c) : ' '); continue; }
        while (i + 1 < utf8.size() && (static_cast<unsigned char>(utf8[i + 1]) & 0xC0) == 0x80) ++i;   // one code point, one '?'
        out.push_back('?');
    }
    return out.empty() ? std::string("Path") : out;
}

// A Pascal string padded so length byte + text is even (8BIM names, 2999).
void pascal(Bytes& b, const std::string& s) {
    b.push_back(static_cast<std::uint8_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
    if ((s.size() + 1) % 2) b.push_back(0);
}

void block(Bytes& out, unsigned id, const std::string& name, const Bytes& data) {
    out.insert(out.end(), {'8', 'B', 'I', 'M'});
    be16(out, id);
    pascal(out, name);
    be32(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    if (data.size() % 2) out.push_back(0);
}

// The path layers, panel order (top first).
std::vector<Document::Id> path_layers(const Image& im, const std::map<Document::Id, Layer>& layers) {
    Document d;
    {
        Document::Loading load(d);
        d.replace(im, layers);
    }
    std::vector<Document::Id> out;
    for (const auto& id : d.walk()) {
        const auto& l = layers.at(id);
        if (l.kind == Kind::Path && l.path && !l.path->empty()) out.push_back(id);
    }
    return out;
}

}  // namespace

Document::Id of(const Image& im, const std::map<Document::Id, Layer>& layers) {
    if (im.clip_path.empty()) return {};
    const auto it = layers.find(im.clip_path);
    if (it == layers.end() || it->second.kind != Kind::Path || !it->second.path || it->second.path->empty()) return {};
    return im.clip_path;
}

Document::Id of(const Document& d) {
    const auto& im = d.image();
    if (im.clip_path.empty()) return {};
    const auto l = d.get(im.clip_path);
    if (!l || l->kind != Kind::Path || !l->path || l->path->empty()) return {};
    return im.clip_path;
}

bool set(DocJournal& j, Document& d, const Document::Id& id, double flatness) {
    if (d.image().width <= 0) return false;
    if (!id.empty()) {
        const auto l = d.get(id);
        if (!l || l->kind != Kind::Path || !l->path || l->path->empty()) return false;
    }
    Image next = d.image();
    const double f = std::round(std::clamp(std::isfinite(flatness) ? flatness : 0.0, 0.0, kMaxFlatness) * 10) / 10;
    if (id.empty()) {
        if (next.clip_path.empty()) return false;   // an inert mark (its layer deleted) is cleared too
        next.clip_path.clear();
    } else {
        if (next.clip_path == id && next.clip_flatness == f) return false;
        next.clip_path = id;
        next.clip_flatness = f;
    }
    return j.run(id.empty() ? "No Clipping Path" : "Clipping Path", {kImageId}, [&] {
        Layer im;
        im.name = "Image";
        im.image = std::make_shared<const Image>(next);
        d.put(kImageId, im);
    });
}

// ── path resources ──────────────────────────────────────────────────────────

std::vector<std::uint8_t> encode(const path::Path& p, int w, int h) {
    Bytes b;
    be16(b, 6); b.insert(b.end(), 24, 0);                  // the path fill rule record
    be16(b, 8); be16(b, 0); b.insert(b.end(), 22, 0);      // the initial fill rule: start empty
    for (const auto& s : p.subs) {
        if (s.nodes.empty()) continue;
        be16(b, s.closed ? 0 : 3);
        be16(b, static_cast<unsigned>(std::min<std::size_t>(s.nodes.size(), 0xFFFF)));
        b.insert(b.end(), 22, 0);
        std::size_t n = 0;
        for (const auto& k : s.nodes) {
            if (++n > 0xFFFF) break;
            be16(b, s.closed ? (k.smooth ? 1 : 2) : (k.smooth ? 4 : 5));
            point(b, k.in, w, h);
            point(b, k.at, w, h);
            point(b, k.out, w, h);
        }
    }
    return b;
}

std::optional<path::Path> decode(const std::uint8_t* data, std::size_t n, int w, int h) {
    if (!data || n < 26 || n % 26) return std::nullopt;
    path::Path p;
    std::size_t left = 0;   // knots still to come in the current subpath
    const auto pt = [&](const std::uint8_t* q) { return path::P{unfixed824(rd32(q + 4), w), unfixed824(rd32(q), h)}; };
    for (std::size_t o = 0; o + 26 <= n; o += 26) {
        const std::uint8_t* r = data + o;
        const unsigned kind = rd16(r);
        switch (kind) {
            case 0: case 3: {
                left = rd16(r + 2);
                path::Sub s;
                s.closed = kind == 0;
                p.subs.push_back(std::move(s));
                break;
            }
            case 1: case 2: case 4: case 5: {
                if (p.subs.empty() || left == 0) break;   // a knot outside any subpath: skipped
                path::Node k;
                k.in = pt(r + 2);
                k.at = pt(r + 10);
                k.out = pt(r + 18);
                k.smooth = kind == 1 || kind == 4;
                p.subs.back().nodes.push_back(k);
                --left;
                break;
            }
            case 6: case 7: case 8: break;
            default: return std::nullopt;
        }
    }
    std::erase_if(p.subs, [](const path::Sub& s) { return s.nodes.empty(); });
    return p;
}

std::vector<std::uint8_t> resources(const Image& im, const std::map<Document::Id, Layer>& layers) {
    Bytes out;
    const auto ids = path_layers(im, layers);
    if (ids.empty()) return out;
    const Document::Id clip = of(im, layers);
    std::set<std::string> used;
    std::string clip_name;
    unsigned rid = 2000;
    for (const auto& id : ids) {
        if (rid > 2997) break;
        const auto& l = layers.at(id);
        const std::string base = pascal_text(l.name);
        std::string name = base;
        for (int k = 2; used.count(name); ++k) name = base.substr(0, 56) + " (" + std::to_string(k) + ")";
        used.insert(name);
        block(out, rid++, name, encode(*l.path, im.width, im.height));
        if (id == clip) clip_name = name;
    }
    if (!clip_name.empty()) {
        Bytes c;
        pascal(c, clip_name);
        const auto fx = static_cast<std::uint32_t>(std::lround(std::clamp(im.clip_flatness, 0.0, kMaxFlatness) * 65536.0));
        be32(c, fx);    // flatness, 16.16 device pixels
        be16(c, 1);     // the fill rule (Photoshop ignores it)
        block(out, 2999, "", c);
    }
    return out;
}

Resources parse(const std::uint8_t* data, std::size_t n, int w, int h) {
    Resources r;
    std::size_t o = 0;
    while (data && o + 12 <= n) {
        if (std::memcmp(data + o, "8BIM", 4) != 0) break;
        const unsigned id = rd16(data + o + 4);
        std::size_t q = o + 6;
        const std::size_t nl = data[q];
        std::string name(reinterpret_cast<const char*>(data + q + 1), std::min(nl, n - q - 1));
        q += 1 + nl;
        if ((nl + 1) % 2) ++q;
        if (q + 4 > n) break;
        const std::size_t size = rd32(data + q);
        q += 4;
        if (q + size > n) break;
        const std::uint8_t* body = data + q;
        if (id >= 2000 && id <= 2997) {
            if (auto p = decode(body, size, w, h); p && !p->empty()) r.paths.push_back({name.empty() ? "Path" : name, *p});
        } else if (id == 2999 && size >= 1) {
            const std::size_t cl = body[0];
            if (1 + cl <= size) r.clip.assign(reinterpret_cast<const char*>(body + 1), cl);
            std::size_t f = 1 + cl;
            if (f % 2) ++f;
            if (f + 4 <= size) r.flatness = std::clamp(rd32(body + f) / 65536.0, 0.0, kMaxFlatness);
        }
        o = q + size + (size % 2);
    }
    // A clip naming no path we kept is no clip.
    if (!r.clip.empty() && std::none_of(r.paths.begin(), r.paths.end(), [&](const Named& p) { return p.name == r.clip; }))
        r.clip.clear();
    return r;
}

void adopt(file::Contents& c, const Resources& r) {
    if (r.paths.empty()) return;
    double top = 0;
    for (const auto& [id, l] : c.layers)
        if (l.parent.empty()) top = std::max(top, l.pos);
    // File order top first: the first path ends highest.
    for (std::size_t i = 0; i < r.paths.size(); ++i) {
        Layer l;
        l.kind = Kind::Path;
        l.name = r.paths[i].name;
        l.pos = top + static_cast<double>(r.paths.size() - i);
        l.path = std::make_shared<const path::Path>(r.paths[i].path);
        std::string id;
        do id = "L" + std::to_string(c.next_id++);
        while (c.layers.count(id));
        if (r.paths[i].name == r.clip && c.image.clip_path.empty()) {
            c.image.clip_path = id;
            c.image.clip_flatness = std::round(r.flatness * 10) / 10;
        }
        c.layers.emplace(id, std::move(l));
    }
    c.notes.push_back(r.paths.size() == 1 ? std::string("1 path") : std::to_string(r.paths.size()) + " paths");
    if (!c.image.clip_path.empty()) c.notes.push_back("a clipping path");
}

}  // namespace klimt::core::clip
