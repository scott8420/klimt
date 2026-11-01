#include "core/Profile.hpp"
#include "core/Cores.hpp"
#include "tint/Tint.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

namespace klimt::core::profile {

namespace {
using pixel::kTile;
constexpr std::size_t kPx = static_cast<std::size_t>(kTile) * kTile;

tint::Space space_of(pixel::Mode m) {
    switch (m) {
        case pixel::Mode::Gray: return tint::Space::Gray;
        case pixel::Mode::CMYK: return tint::Space::CMYK;
        case pixel::Mode::Lab:  return tint::Space::Lab;
        case pixel::Mode::RGB:  break;
    }
    return tint::Space::RGB;
}
pixel::Mode mode_of_space(tint::Space s) {
    switch (s) {
        case tint::Space::Gray: return pixel::Mode::Gray;
        case tint::Space::CMYK: return pixel::Mode::CMYK;
        case tint::Space::Lab:  return pixel::Mode::Lab;
        case tint::Space::RGB:  break;
    }
    return pixel::Mode::RGB;
}

// One tile through the link, into format `to`.
pixel::TileMap::TileRef tile_through(const pixel::TileMap::TileRef& t, const tint::Link& link, const pixel::Format& to) {
    if (!t) return nullptr;
    const pixel::Format& f = t->format();
    const int n = f.channels(), cc = f.colour_channels(), on = to.channels(), occ = to.colour_channels();
    if (t->is_uniform()) {
        const pixel::Px p = t->get(0, 0);
        float in[4] = {}, out[4] = {};
        for (int c = 0; c < cc; ++c) in[c] = p.c[static_cast<std::size_t>(c)];
        link.run(in, out, 1);
        pixel::Px q;
        for (int c = 0; c < occ; ++c) q.c[static_cast<std::size_t>(c)] = out[c];
        if (to.alpha) q.c[static_cast<std::size_t>(on - 1)] = f.alpha ? p.c[static_cast<std::size_t>(n - 1)] : 1.f;
        return pixel::Tile::uniform(to, q);
    }
    thread_local std::vector<float> raw, src, dst, pm;
    t->premultiplied(raw);
    src.resize(kPx * static_cast<std::size_t>(cc));
    dst.resize(kPx * static_cast<std::size_t>(occ));
    for (std::size_t i = 0; i < kPx; ++i) {
        const float* s = raw.data() + i * static_cast<std::size_t>(n);
        const float a = f.alpha ? s[n - 1] : 1.f;
        for (int c = 0; c < cc; ++c) src[i * static_cast<std::size_t>(cc) + static_cast<std::size_t>(c)] = a > 0.f ? s[c] / a : 0.f;
    }
    link.run(src.data(), dst.data(), kPx);
    pm.resize(kPx * static_cast<std::size_t>(on));
    for (std::size_t i = 0; i < kPx; ++i) {
        const float a = f.alpha ? raw[i * static_cast<std::size_t>(n) + static_cast<std::size_t>(n - 1)] : 1.f;
        float* d = pm.data() + i * static_cast<std::size_t>(on);
        for (int c = 0; c < occ; ++c) d[c] = dst[i * static_cast<std::size_t>(occ) + static_cast<std::size_t>(c)] * a;
        if (to.alpha) d[on - 1] = a;
    }
    pixel::TileWriter w(to, pixel::transparent());
    w.set_premultiplied(pm);
    return w.freeze();
}
}  // namespace

std::optional<pixel::Mode> mode_of(const std::vector<std::uint8_t>& icc) {
    const auto i = tint::inspect(icc);
    if (!i.ok) return std::nullopt;
    return mode_of_space(i.space);
}

std::string assign_refusal(const Image& im, const IccPtr& icc) {
    if (!icc) return {};
    const auto i = tint::inspect(*icc);
    if (!i.ok) return "not a profile Klimt can read";
    if (mode_of_space(i.space) != im.format.mode) return "a profile for another colour mode -- use Convert to change the mode";
    return {};
}

bool assign(DocJournal& j, Document& d, const IccPtr& icc) {
    if (!assign_refusal(d.image(), icc).empty()) return false;
    if (d.image().icc == icc || (d.image().icc && icc && *d.image().icc == *icc)) return false;   // nothing to do
    Image im = d.image();
    im.icc = icc;
    const auto name = icc ? tint::inspect(*icc).name : std::string("no profile");
    return j.run("Assign Profile “" + name + "”", {kImageId}, [&] {
        Layer rec;
        rec.name = "Image";
        rec.image = std::make_shared<const Image>(std::move(im));
        d.put(kImageId, rec);
    });
}

std::string convert_refusal(const Image& im, const IccPtr& to) {
    if (!to) return "no profile to convert to";
    const auto i = tint::inspect(*to);
    if (!i.ok) return "not a profile Klimt can read";
    if (im.format.depth == pixel::Depth::F32 && i.space != tint::Space::RGB && i.space != tint::Space::Gray)
        return "32-bit documents stay RGB or Gray";
    return {};
}

std::optional<canvas::Reshape> convert_plan(const canvas::Before& b, const IccPtr& to, tint::Render render,
                                            const std::function<bool()>& cancelled,
                                            const std::function<void(double)>& progress) {
    if (!b.image || !convert_refusal(*b.image, to).empty()) return std::nullopt;
    const Image& im = *b.image;
    const tint::Space fs = space_of(im.format.mode);
    const tint::Space ts = tint::inspect(*to).space;
    // The document's profile -- unusable or of another space: its mode's plain meaning.
    tint::Icc from = im.icc ? *im.icc : tint::Icc{};
    if (from.empty() && !im.icc)   // s070: untagged -- the working space's meaning, when set
        if (const auto w = tint::working(fs)) from = *w;
    if (from.empty() || !tint::inspect(from).ok || tint::inspect(from).space != fs) from = tint::implicit(fs);
    const auto link = tint::link(from, fs, *to, ts, render);
    if (!link) return std::nullopt;
    const pixel::Format nf{im.format.depth, mode_of_space(ts), im.format.alpha};
    // Every tile of every pixel layer, grid and outside, an item for the cores.
    struct Item { Document::Id id; bool outside; int col, row; std::pair<int, int> key; };
    std::vector<Item> items;
    std::map<Document::Id, std::vector<pixel::TileMap::TileRef>> grids;
    std::map<Document::Id, pixel::TileMap::Outside> outs;
    for (const auto& [id, m] : b.pixels) {
        if (!m) continue;
        grids[id].resize(static_cast<std::size_t>(m->cols()) * static_cast<std::size_t>(m->rows()));
        for (int r = 0; r < m->rows(); ++r)
            for (int c = 0; c < m->cols(); ++c) items.push_back({id, false, c, r, {}});
        for (const auto& [k, t] : m->outside_tiles()) { outs[id][k] = nullptr; items.push_back({id, true, 0, 0, k}); }
    }
    formats::Progress p;
    p.cancelled = cancelled;
    p.progress = progress;
    const bool ok = on_cores(items.size(), [&](std::size_t i) {
        const Item& it = items[i];
        const auto& m = *b.pixels.at(it.id);
        if (it.outside) outs.at(it.id).at(it.key) = tile_through(m.outside_tiles().at(it.key), *link, nf);
        else grids.at(it.id)[static_cast<std::size_t>(it.row) * static_cast<std::size_t>(m.cols()) + static_cast<std::size_t>(it.col)] =
                 tile_through(m.tile(it.col, it.row), *link, nf);
        return true;
    }, p);
    if (!ok) return std::nullopt;
    canvas::Reshape r;
    Image ni = im;
    ni.format = nf;
    ni.icc = to;
    r.image = std::make_shared<const Image>(std::move(ni));
    for (const auto& [id, m] : b.pixels) {
        if (!m) { r.pixels[id] = nullptr; continue; }
        r.pixels[id] = std::make_shared<const pixel::TileMap>(
            pixel::TileMap::from_tiles(nf, m->width(), m->height(), std::move(grids.at(id))).with_outside(std::move(outs[id])));
    }
    r.mask = b.mask;
    r.masks = b.masks;
    return r;
}

namespace {
std::vector<Choice> builtins() {
    static const std::vector<Choice> v = [] {
        std::vector<Choice> o;
        const auto add = [&](tint::Icc icc) {
            auto p = std::make_shared<const std::vector<std::uint8_t>>(std::move(icc));
            const auto i = tint::inspect(*p);
            o.push_back({i.name, p, mode_of_space(i.space), {}});
        };
        add(tint::srgb());
        add(tint::adobe_rgb());
        add(tint::gray(2.2));
        add(tint::implicit(tint::Space::Gray));
        add(tint::plain_cmyk());
        add(tint::implicit(tint::Space::Lab));
        return o;
    }();
    return v;
}
}  // namespace

std::optional<Choice> load(const std::string& path, std::string& why) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { why = "can't be read"; return std::nullopt; }
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() > (32u << 20)) { why = "too big for a profile"; return std::nullopt; }
    const auto i = tint::inspect(b);
    if (!i.ok) { why = "not an ICC profile Klimt can use (RGB, Gray, CMYK or Lab)"; return std::nullopt; }
    Choice c;
    c.name = i.name.empty() ? std::filesystem::path(path).stem().string() : i.name;
    c.icc = std::make_shared<const std::vector<std::uint8_t>>(std::move(b));
    c.mode = mode_of_space(i.space);
    c.path = path;
    return c;
}

std::vector<Choice> catalog(const std::vector<std::string>& dirs_in) {
    static std::mutex mu;
    static std::map<std::vector<std::string>, std::vector<Choice>> made;
    std::vector<std::string> dirs = dirs_in;
    if (dirs.empty()) {
        dirs = {"/usr/share/color/icc", "/usr/local/share/color/icc", "/usr/share/ghostscript"};   // s069: + Ghostscript's
        if (const char* h = std::getenv("HOME")) {
            dirs.push_back(std::string(h) + "/.local/share/icc");
            dirs.push_back(std::string(h) + "/.color/icc");
        }
    }
    std::lock_guard lk(mu);
    if (auto it = made.find(dirs); it != made.end()) return it->second;
    std::vector<Choice> found;
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(d, ec)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(d, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            auto ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".icc" && ext != ".icm") continue;
            std::string why;
            if (auto c = load(it->path().string(), why)) found.push_back(std::move(*c));
            if (found.size() > 400) break;
        }
    }
    std::sort(found.begin(), found.end(), [](const Choice& a, const Choice& b) {
        return a.mode != b.mode ? static_cast<int>(a.mode) < static_cast<int>(b.mode) : a.name < b.name;
    });
    found.erase(std::unique(found.begin(), found.end(), [](const Choice& a, const Choice& b) { return a.name == b.name && a.mode == b.mode; }),
                found.end());
    std::vector<Choice> all = builtins();
    all.insert(all.end(), found.begin(), found.end());
    made[dirs] = all;
    return all;
}

std::optional<Choice> find(const std::string& remembered, const std::vector<std::string>& dirs) {
    if (remembered.empty()) return std::nullopt;
    std::string why;
    if (remembered.front() == '/') return load(remembered, why);   // a file: read it (no folder scan at startup)
    for (const auto& c : builtins())
        if (c.name == remembered) return c;
    (void)dirs;
    return std::nullopt;
}

IccPtr mode_profile(pixel::Mode m) {
    if (m == pixel::Mode::Gray || m == pixel::Mode::CMYK)
        if (auto w = tint::working(m == pixel::Mode::Gray ? tint::Space::Gray : tint::Space::CMYK)) return w;
    return builtin(m);
}

Choice proof_choice(const std::string& remembered, const std::vector<std::string>& dirs) {
    const auto all = catalog(dirs);
    if (auto c = find(remembered, dirs)) return *c;
    if (const auto w = tint::working(tint::Space::CMYK)) {   // s070: the working CMYK
        for (const auto& c : all)
            if (c.icc && *c.icc == *w) return c;
        const auto i = tint::inspect(*w);
        return {i.name, w, pixel::Mode::CMYK, {}};
    }
    for (const auto& c : all)
        if (!c.path.empty() && c.mode == pixel::Mode::CMYK && c.name.find("SWOP") != std::string::npos) return c;
    for (const auto& c : all)
        if (c.path.empty() && c.mode == pixel::Mode::CMYK) return c;
    return all.front();
}

std::string remembered_as(const Choice& c) { return c.path.empty() ? c.name : c.path; }

IccPtr builtin(pixel::Mode m) {
    const auto& b = builtins();
    switch (m) {
        case pixel::Mode::RGB:  return b[0].icc;
        case pixel::Mode::Gray: return b[2].icc;
        case pixel::Mode::CMYK: return b[4].icc;
        case pixel::Mode::Lab:  return b[5].icc;
    }
    return b[0].icc;
}

bool convert(DocJournal& j, Document& d, const IccPtr& to, tint::Render r) {
    const auto b = canvas::before(d);
    const auto p = convert_plan(b, to, r);
    return p && canvas::land(j, d, b, *p, "Convert to Profile “" + tint::inspect(*to).name + "”");
}

}  // namespace klimt::core::profile
