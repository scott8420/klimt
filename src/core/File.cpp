#include "core/File.hpp"
#include "core/Composite.hpp"

#include <zip.h>
#include "json.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <filesystem>
#include <unordered_map>

namespace klimt::core::file {

namespace {
using json = nlohmann::json;
namespace P = pixel;
constexpr std::size_t kMaxJson = 64u << 20;   // a document.json past 64 MB is not ours

const char* depth_key(P::Depth d) { return d == P::Depth::U8 ? "8" : d == P::Depth::U16 ? "16" : "32f"; }
const char* mode_key(P::Mode m) {
    switch (m) { case P::Mode::Gray: return "gray"; case P::Mode::RGB: return "rgb";
                 case P::Mode::CMYK: return "cmyk"; case P::Mode::Lab: return "lab"; }
    return "rgb";
}
std::optional<P::Depth> depth_from(const std::string& s) {
    if (s == "8") return P::Depth::U8;
    if (s == "16") return P::Depth::U16;
    if (s == "32f") return P::Depth::F32;
    return std::nullopt;
}
std::optional<P::Mode> mode_from(const std::string& s) {
    if (s == "gray") return P::Mode::Gray;
    if (s == "rgb") return P::Mode::RGB;
    if (s == "cmyk") return P::Mode::CMYK;
    if (s == "lab") return P::Mode::Lab;
    return std::nullopt;
}

// The file is little-endian; a big-endian host swaps each sample.
void to_little(const P::Format& f, std::uint8_t* d, std::size_t n) {
    if constexpr (std::endian::native == std::endian::little) { (void)f; (void)d; (void)n; return; }
    const auto b = static_cast<std::size_t>(f.bytes_per_sample());
    if (b == 1) return;
    for (std::size_t i = 0; i + b <= n; i += b) std::reverse(d + i, d + i + b);
}

std::string hex(const std::uint8_t* d, std::size_t n) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) { s += k[d[i] >> 4]; s += k[d[i] & 15]; }
    return s;
}
std::optional<std::vector<std::uint8_t>> unhex(const std::string& s) {
    if (s.size() % 2) return std::nullopt;
    std::vector<std::uint8_t> out;
    const auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); i += 2) {
        const int a = v(s[i]), b = v(s[i + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(a * 16 + b));
    }
    return out;
}

// An id the rest of Klimt can live with: "L<digits>" (at most 9) or any
// short plain name.
bool good_id(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    if (id[0] == 'L' && id.size() > 1 && std::all_of(id.begin() + 1, id.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return id.size() <= 10;
    return std::all_of(id.begin(), id.end(), [](char c) { return c > ' ' && c != '"' && c != '\\'; });
}

// The tree a file holds must be a tree Klimt can hold: Document::check.
// s013: an adjustment layer's settings as JSON, and back. A file is trusted
// only after Document::check() (adjust::refusal) has passed it.
json adjust_json(const Adjust& a) {
    json j = {{"kind", std::string(adjust::key(a.kind))}};
    switch (a.kind) {
        case AdjustKind::Levels: {
            json ch = json::array();
            for (const auto& c : a.levels) ch.push_back({c.in_black, c.in_white, c.gamma, c.out_black, c.out_white});
            j["levels"] = std::move(ch);
            break;
        }
        case AdjustKind::Curves: {
            json ch = json::array();
            for (const auto& c : a.curves) {
                json pts = json::array();
                for (const auto& p : c) pts.push_back({p.x, p.y});
                ch.push_back(std::move(pts));
            }
            j["curves"] = std::move(ch);
            break;
        }
        case AdjustKind::HueSat:
            j["hue"] = a.hue; j["saturation"] = a.saturation; j["lightness"] = a.lightness; j["colorize"] = a.colorize;
            break;
        case AdjustKind::BrightCon:
            j["brightness"] = a.brightness; j["contrast"] = a.contrast;
            break;
        case AdjustKind::None: break;
    }
    return j;
}

// s018: a live filter -- the stable key and every setting.
json live_json(const LiveFilter& lf) {
    const Filter& f = lf.f;
    return {{"kind", std::string(filter::key(f.kind))}, {"on", lf.on}, {"radius", f.radius}, {"angle", f.angle},
            {"amount", f.amount}, {"threshold", f.threshold}, {"gaussian", f.gaussian}, {"mono", f.mono}, {"seed", f.seed}};
}
std::optional<LiveFilter> live_from(const json& j) {
    const std::string key = j.at("kind").get<std::string>();
    LiveFilter lf;
    for (const auto k : filter::kinds()) if (filter::key(k) == key) lf.f.kind = k;
    if (lf.f.kind == FilterKind::None) return std::nullopt;
    lf.on = j.value("on", true);
    lf.f.radius = j.value("radius", 1.0);
    lf.f.angle = j.value("angle", 0);
    lf.f.amount = j.value("amount", 0.0);
    lf.f.threshold = j.value("threshold", 0);
    lf.f.gaussian = j.value("gaussian", false);
    lf.f.mono = j.value("mono", false);
    lf.f.seed = j.value("seed", 1u);
    const std::uint32_t seed = lf.f.seed;
    lf.f = filter::normalised(lf.f);
    lf.f.seed = lf.f.kind == FilterKind::AddNoise ? seed : lf.f.seed;
    return lf;
}

std::optional<Adjust> adjust_from(const json& j) {
    const auto k = adjust::from_key(j.at("kind").get<std::string>());
    if (!k) return std::nullopt;
    Adjust a = adjust::fresh(*k);
    if (j.contains("levels")) {
        const auto& ch = j.at("levels");
        if (!ch.is_array() || ch.size() != static_cast<std::size_t>(kAdjustChannels)) return std::nullopt;
        for (std::size_t i = 0; i < ch.size(); ++i) {
            const auto& v = ch[i];
            if (!v.is_array() || v.size() != 5) return std::nullopt;
            a.levels[i] = {v[0].get<double>(), v[1].get<double>(), v[2].get<double>(), v[3].get<double>(), v[4].get<double>()};
        }
    }
    if (j.contains("curves")) {
        const auto& ch = j.at("curves");
        if (!ch.is_array() || ch.size() != static_cast<std::size_t>(kAdjustChannels)) return std::nullopt;
        for (std::size_t i = 0; i < ch.size(); ++i) {
            if (!ch[i].is_array() || ch[i].size() > 16) return std::nullopt;
            a.curves[i].clear();
            for (const auto& p : ch[i]) {
                if (!p.is_array() || p.size() != 2) return std::nullopt;
                a.curves[i].push_back({p[0].get<double>(), p[1].get<double>()});
            }
        }
    }
    a.hue = j.value("hue", 0);
    a.saturation = j.value("saturation", 0);
    a.lightness = j.value("lightness", 0);
    a.colorize = j.value("colorize", false);
    a.brightness = j.value("brightness", 0);
    a.contrast = j.value("contrast", 0);
    return a;
}

std::string check_tree(const Contents& c) {
    Document d;
    Document::Loading load(d);
    d.replace(c.image, c.layers, c.next_id);
    return d.check();
}

struct Zip {
    zip_t* z = nullptr;
    ~Zip() { if (z) zip_discard(z); }
};

bool read_entry(zip_t* z, zip_int64_t idx, std::vector<std::uint8_t>& out, std::size_t limit) {
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

std::optional<Contents> open_klimt(const std::string& path, std::string& error, const formats::Progress& p) {
    int code = 0;
    Zip zip;
    zip.z = zip_open(path.c_str(), ZIP_RDONLY, &code);
    if (!zip.z) { error = "not a readable Klimt file"; return std::nullopt; }
    zip_t* z = zip.z;
    const auto entry = [&](const char* name) { return zip_name_locate(z, name, 0); };
    std::vector<std::uint8_t> raw;
    const zip_int64_t ji = entry("document.json");
    if (ji < 0 || !read_entry(z, ji, raw, kMaxJson)) { error = "a damaged Klimt file (no document.json)"; return std::nullopt; }
    json doc = json::parse(raw.begin(), raw.end(), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) { error = "a damaged Klimt file (document.json)"; return std::nullopt; }

    Contents c;
    c.kind = formats::Kind::Klimt;
    try {
        const int version = doc.at("klimt").get<int>();
        if (version > kVersion) { error = "made by a newer Klimt (format " + std::to_string(version) + ")"; return std::nullopt; }
        if (doc.value("tile", P::kTile) != P::kTile) { error = "tiles of another size"; return std::nullopt; }
        const json& im = doc.at("image");
        const auto depth = depth_from(im.at("depth").get<std::string>());
        const auto mode = mode_from(im.at("mode").get<std::string>());
        if (!depth || !mode) { error = "an unknown depth or mode"; return std::nullopt; }
        c.image.width = im.at("width").get<int>();
        c.image.height = im.at("height").get<int>();
        c.image.format = {*depth, *mode, im.at("alpha").get<bool>()};
        c.image.dpi = im.value("dpi", 72.0);
        if (c.image.width <= 0 || c.image.height <= 0 || c.image.width > formats::kMaxSide ||
            c.image.height > formats::kMaxSide ||
            static_cast<std::uint64_t>(c.image.width) * static_cast<std::uint64_t>(c.image.height) > formats::kMaxPixels) {
            error = "an image size Klimt can't hold";
            return std::nullopt;
        }
        if (const zip_int64_t pi = entry("profile.icc"); pi >= 0) {
            std::vector<std::uint8_t> icc;
            if (read_entry(z, pi, icc, 16u << 20) && !icc.empty())
                c.image.icc = std::make_shared<const std::vector<std::uint8_t>>(std::move(icc));
        }
        c.next_id = doc.value("next_id", 1);

        const P::Format f = c.image.format;
        const P::TileMap blank(f, c.image.width, c.image.height);
        const int cols = blank.cols(), rows = blank.rows();
        std::unordered_map<std::string, P::TileMap::TileRef> loaded;   // a shared tile, read once

        const json& layers = doc.at("layers");
        std::size_t total = 0, done = 0;
        for (const auto& l : layers) {
            if (l.contains("tiles")) total += l.at("tiles").size();
            if (l.contains("outside")) total += l.at("outside").size();
            if (l.contains("mask")) total += l.at("mask").size();
            if (l.contains("filter_mask")) total += l.at("filter_mask").size();
        }
        // One map's tiles (s012: a layer's pixels, or its mask): the image's
        // and those beyond it, in the format `fm`. false: damaged (error set).
        const auto read_map = [&](const json& l, const char* in_key, const char* out_key, const P::Format& fm,
                                  P::TileMap& result) -> bool {
            const std::size_t tb = static_cast<std::size_t>(P::kTile) * P::kTile * static_cast<std::size_t>(fm.bytes_per_pixel());
            std::vector<P::TileMap::TileRef> grid(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows));
            P::TileMap::Outside outside;   // s010: the pixels beyond the image
            const json none = json::array();
            const json& in_list = l.contains(in_key) ? l.at(in_key) : none;
            const json& out_list = l.contains(out_key) ? l.at(out_key) : none;
            for (int pass = 0; pass < 2; ++pass)
            for (const auto& t : pass == 0 ? in_list : out_list) {
                if (p.stop()) { error = "cancelled"; return false; }
                const int col = t.at(0).get<int>(), row = t.at(1).get<int>();
                const std::string ref = t.at(2).get<std::string>();
                if (pass == 0 && (col < 0 || row < 0 || col >= cols || row >= rows)) { error = "a damaged Klimt file (a tile outside the image)"; return false; }
                // An outside cell is one the image doesn't wholly cover, within reach.
                if (pass == 1 && (std::abs(col) > 1'200'000 || std::abs(row) > 1'200'000 ||
                                  (col >= 0 && row >= 0 && (col + 1) * P::kTile <= c.image.width && (row + 1) * P::kTile <= c.image.height))) {
                    error = "a damaged Klimt file (an outside tile inside the image)";
                    return false;
                }
                P::TileMap::TileRef tile;
                if (ref.rfind("u:", 0) == 0) {
                    auto px = unhex(ref.substr(2));
                    if (!px || px->size() != static_cast<std::size_t>(fm.bytes_per_pixel())) { error = "a damaged Klimt file (a uniform tile)"; return false; }
                    to_little(fm, px->data(), px->size());
                    std::vector<std::uint8_t> full(tb);
                    for (std::size_t i = 0; i < tb; i += px->size()) std::memcpy(full.data() + i, px->data(), px->size());
                    tile = P::Tile::from_bytes(fm, std::move(full));
                } else if (auto it = loaded.find(ref); it != loaded.end()) {
                    tile = it->second;
                } else {
                    const zip_int64_t ti = entry(ref.c_str());
                    std::vector<std::uint8_t> bytes;
                    if (ti < 0 || !read_entry(z, ti, bytes, tb) || bytes.size() != tb) {
                        error = "a damaged Klimt file (" + ref + ")";
                        return false;
                    }
                    to_little(fm, bytes.data(), bytes.size());
                    tile = P::Tile::from_bytes(fm, std::move(bytes));
                    loaded[ref] = tile;
                }
                if (pass == 0) grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(col)] = tile;
                else if (tile) outside[{col, row}] = tile;
                ++done;
                if (total && (done & 15) == 0) p.at(static_cast<double>(done) / static_cast<double>(total));
            }
            result = P::TileMap::from_tiles(fm, c.image.width, c.image.height, std::move(grid)).with_outside(std::move(outside));
            return true;
        };
        for (const auto& l : layers) {
            const std::string id = l.at("id").get<std::string>();
            if (!good_id(id) || c.layers.count(id)) { error = "a damaged Klimt file (layer id \"" + id + "\")"; return std::nullopt; }
            Layer L;
            const std::string kind = l.at("kind").get<std::string>();
            if (kind == "pixel") L.kind = Kind::Pixel;
            else if (kind == "group") L.kind = Kind::Group;
            else if (kind == "adjustment") {   // s013
                L.kind = Kind::Adjustment;
                const auto a = adjust_from(l.at("adjust"));
                if (!a) { error = "an adjustment this Klimt doesn't know"; return std::nullopt; }
                L.adjust = *a;
            }
            else { error = "a layer kind this Klimt doesn't know (" + kind + ")"; return std::nullopt; }
            if (l.contains("filters") && L.kind == Kind::Pixel)   // s018: live filters
                for (const auto& jf : l.at("filters")) {
                    const auto lf = live_from(jf);
                    if (!lf) { error = "a live filter this Klimt doesn't know"; return std::nullopt; }
                    L.filters.push_back(*lf);
                }
            if (l.contains("filter_mask") && L.kind == Kind::Pixel) {   // s018 m3
                P::TileMap m;
                if (!read_map(l, "filter_mask", "filter_mask_outside", P::Format{f.depth, P::Mode::Gray, false}, m)) return std::nullopt;
                L.filter_mask = std::make_shared<const P::TileMap>(std::move(m));
            }
            L.name = l.value("name", std::string{});
            L.parent = l.value("parent", std::string{});
            L.pos = l.at("pos").get<double>();
            L.visible = l.value("visible", true);
            L.locks = static_cast<std::uint8_t>(l.value("locks", 0) & lock::Mask);
            L.opacity = l.value("opacity", 100);
            L.fill = l.value("fill", 100);
            const auto blend = blend_from_key(l.value("blend", std::string("normal")));
            if (!blend) { error = "a blend mode this Klimt doesn't know"; return std::nullopt; }
            L.blend = *blend;
            L.clipped = l.value("clipped", false);
            if ((l.contains("tiles") || l.contains("outside")) && L.kind == Kind::Pixel) {
                P::TileMap m;
                if (!read_map(l, "tiles", "outside", f, m)) return std::nullopt;
                L.pixels = std::make_shared<const P::TileMap>(std::move(m));
            }
            // s012: the layer mask (stored as what it hides), and whether it's on.
            if (l.contains("mask") && L.kind != Kind::Group) {
                P::TileMap m;
                if (!read_map(l, "mask", "mask_outside", P::Format{f.depth, P::Mode::Gray, false}, m)) return std::nullopt;
                L.mask = std::make_shared<const P::TileMap>(std::move(m));
                L.mask_on = l.value("mask_on", true);
            }
            c.layers[id] = std::move(L);
        }
        // s016: the saved selections.
        if (doc.contains("channels"))
            for (const auto& ch : doc.at("channels")) {
                const std::string id = ch.at("id").get<std::string>();
                if (!is_channel(id) || c.channels.count(id) || !ch.contains("mask")) {
                    error = "a damaged Klimt file (channel \"" + id + "\")";
                    return std::nullopt;
                }
                Layer C;
                C.name = ch.value("name", std::string("Alpha"));
                C.pos = ch.value("pos", 0.0);
                P::TileMap m;
                if (!read_map(ch, "mask", "mask_outside", P::Format{f.depth, P::Mode::Gray, false}, m)) return std::nullopt;
                C.mask = std::make_shared<const P::TileMap>(std::move(m));
                c.channels[id] = std::move(C);
            }
    } catch (const json::exception& e) {
        error = std::string("a damaged Klimt file (") + e.what() + ")";
        return std::nullopt;
    }
    if (c.layers.empty()) { error = "a Klimt file with no layers"; return std::nullopt; }
    if (const auto bad = check_tree(c); !bad.empty()) { error = "a damaged Klimt file (" + bad + ")"; return std::nullopt; }
    p.at(1);
    return c;
}

// libzip's callbacks: progress while it writes, and a cancel it asks about.
struct ZipState { const formats::Progress* p; };
void zip_progress(zip_t*, double f, void* ud) { static_cast<ZipState*>(ud)->p->at(f); }
int  zip_cancel(zip_t*, void* ud) { return static_cast<ZipState*>(ud)->p->stop() ? 1 : 0; }
}  // namespace

std::optional<Contents> open(const std::string& path, std::string& error, const formats::Progress& p) {
    const auto kind = formats::sniff(path);
    if (kind == formats::Kind::Klimt) return open_klimt(path, error, p);
    auto r = formats::read_image(path, error, p);
    if (!r) return std::nullopt;
    Contents c;
    c.kind = kind;
    c.notes = r->notes;
    c.image.width = r->pixels.width();
    c.image.height = r->pixels.height();
    c.image.format = r->pixels.format();
    c.image.dpi = r->dpi;
    if (!r->icc.empty()) c.image.icc = std::make_shared<const std::vector<std::uint8_t>>(std::move(r->icc));
    Layer bg;
    bg.name = "Background";
    bg.pos = 1;
    bg.pixels = std::make_shared<const P::TileMap>(std::move(r->pixels));
    c.layers["L1"] = std::move(bg);
    c.next_id = 2;
    return c;
}

bool save_klimt(const std::string& path, const Contents& c, std::string& error, const formats::Progress& p) {
    const P::Format f = c.image.format;
    const std::string tmp = path + ".partial";
    std::remove(tmp.c_str());
    int code = 0;
    Zip zip;
    zip.z = zip_open(tmp.c_str(), ZIP_CREATE | ZIP_EXCL, &code);
    if (!zip.z) { error = "can't be written"; return false; }
    zip_t* z = zip.z;

    // Everything libzip reads at close must live until then: these.
    static const char kMime[] = "image/x-klimt";
    std::vector<std::vector<std::uint8_t>> swapped;   // big-endian hosts only
    std::string doc_text;
    const auto add = [&](const std::string& name, const void* data, std::size_t n, zip_int32_t method, zip_uint32_t level) {
        zip_source_t* s = zip_source_buffer(z, data, n, 0);
        if (!s) return false;
        const zip_int64_t i = zip_file_add(z, name.c_str(), s, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (i < 0) { zip_source_free(s); return false; }
        return zip_set_file_compression(z, static_cast<zip_uint64_t>(i), method, level) == 0;
    };
    if (!add("mimetype", kMime, sizeof kMime - 1, ZIP_CM_STORE, 0)) { error = zip_strerror(z); return false; }

    json doc;
    doc["klimt"] = kVersion;
    doc["app"] = "Klimt";
    doc["tile"] = P::kTile;
    doc["endian"] = "little";
    doc["image"] = {{"width", c.image.width}, {"height", c.image.height}, {"depth", depth_key(f.depth)},
                    {"mode", mode_key(f.mode)}, {"alpha", f.alpha}, {"dpi", c.image.dpi}};
    doc["next_id"] = c.next_id;
    std::unordered_map<const P::Tile*, std::string> named;   // a shared tile, written once
    json layers = json::array();
    int next_tile = 0;
// One map's tiles into `l` under in_key / out_key (s012: pixels, or
    // the mask). false: the zip failed (error set).
    const auto write_map = [&](json& l, const P::TileMap& m, const char* in_key, const char* out_key) -> bool {
        const P::Format fm = m.format();
        json tiles = json::array();
        json outside = json::array();   // s010: the pixels beyond the image
        std::vector<std::tuple<int, int, P::TileMap::TileRef, bool>> all;
        for (int row = 0; row < m.rows(); ++row)
            for (int col = 0; col < m.cols(); ++col)
                if (auto t = m.tile(col, row)) all.emplace_back(col, row, t, false);
        for (const auto& [cell, t] : m.outside_tiles()) all.emplace_back(cell.first, cell.second, t, true);
        for (const auto& [col, row, t, out] : all) {
            json& list = out ? outside : tiles;
            if (t->is_uniform()) {
                std::vector<std::uint8_t> px(t->data(), t->data() + t->bytes());
                to_little(fm, px.data(), px.size());
                list.push_back({col, row, "u:" + hex(px.data(), px.size())});
                continue;
            }
            auto it = named.find(t.get());
            if (it == named.end()) {
                const std::string name = "tiles/" + std::to_string(next_tile++);
                const std::uint8_t* data = t->data();
                if constexpr (std::endian::native != std::endian::little) {
                    swapped.emplace_back(t->data(), t->data() + t->bytes());
                    to_little(fm, swapped.back().data(), swapped.back().size());
                    data = swapped.back().data();
                }
                if (!add(name, data, t->bytes(), ZIP_CM_DEFLATE, 1)) { error = zip_strerror(z); return false; }
                it = named.emplace(t.get(), name).first;
            }
            list.push_back({col, row, it->second});
        }
        l[in_key] = std::move(tiles);
        if (!outside.empty()) l[out_key] = std::move(outside);
        return true;
    };
    for (const auto& [id, L] : c.layers) {
        json l = {{"id", id}, {"kind", L.kind == Kind::Group ? "group" : L.kind == Kind::Adjustment ? "adjustment" : "pixel"}, {"name", L.name},
                  {"parent", L.parent}, {"pos", L.pos}, {"visible", L.visible}, {"locks", L.locks},
                  {"opacity", L.opacity}, {"fill", L.fill}, {"blend", std::string(blend_key(L.blend))},
                  {"clipped", L.clipped}};
        if (L.pixels && L.kind == Kind::Pixel && !write_map(l, *L.pixels, "tiles", "outside")) return false;
        if (L.kind == Kind::Adjustment) l["adjust"] = adjust_json(L.adjust);   // s013
        if (L.kind == Kind::Pixel && !L.filters.empty()) {                      // s018: live filters
            json fs = json::array();
            for (const auto& lf : L.filters) fs.push_back(live_json(lf));
            l["filters"] = std::move(fs);
            if (L.filter_mask && !write_map(l, *L.filter_mask, "filter_mask", "filter_mask_outside")) return false;   // m3
        }
        if (L.mask && L.kind != Kind::Group) {   // s012: the layer mask
            if (!write_map(l, *L.mask, "mask", "mask_outside")) return false;
            l["mask_on"] = L.mask_on;
        }
        layers.push_back(std::move(l));
    }
    doc["layers"] = std::move(layers);
    // s016: the saved selections -- a name, an order, a mask.
    if (!c.channels.empty()) {
        json chans = json::array();
        for (const auto& [id, C] : c.channels) {
            if (!C.mask) continue;
            json ch = {{"id", id}, {"name", C.name}, {"pos", C.pos}};
            if (!write_map(ch, *C.mask, "mask", "mask_outside")) return false;
            chans.push_back(std::move(ch));
        }
        doc["channels"] = std::move(chans);
    }
    doc_text = doc.dump(1);
    if (!add("document.json", doc_text.data(), doc_text.size(), ZIP_CM_DEFLATE, 6)) { error = zip_strerror(z); return false; }
    if (c.image.icc && !c.image.icc->empty() &&
        !add("profile.icc", c.image.icc->data(), c.image.icc->size(), ZIP_CM_DEFLATE, 6)) {
        error = zip_strerror(z);
        return false;
    }

    ZipState st{&p};
    zip_register_progress_callback_with_state(z, 0.01, zip_progress, nullptr, &st);
    zip_register_cancel_callback_with_state(z, zip_cancel, nullptr, &st);
    if (zip_close(z) != 0) {
        error = p.stop() ? "cancelled" : std::string("can't be written: ") + zip_strerror(z);
        std::remove(tmp.c_str());
        return false;   // ~Zip discards
    }
    zip.z = nullptr;
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::remove(tmp.c_str()); error = "can't be written: " + ec.message(); return false; }
    p.at(1);
    return true;
}

std::string check(const NewSpec& s) {
    if (s.width < 1 || s.height < 1) return "the size must be at least 1 x 1";
    if (s.width > formats::kMaxSide || s.height > formats::kMaxSide)
        return "no side over " + std::to_string(formats::kMaxSide) + " px";
    if (static_cast<std::uint64_t>(s.width) * static_cast<std::uint64_t>(s.height) > formats::kMaxPixels)
        return "more pixels than Klimt holds";
    if (!(s.dpi >= 1 && s.dpi <= 10000)) return "resolution from 1 to 10000";
    if (s.background == Background::Transparent && !s.format.alpha) return "a transparent background needs alpha";
    return {};
}

Contents make_new(const NewSpec& s) {
    Contents c;
    c.image.width = s.width;
    c.image.height = s.height;
    c.image.format = s.format;
    c.image.dpi = s.dpi;
    Layer bg;
    bg.name = "Background";
    bg.pos = 1;
    P::TileMap m(s.format, s.width, s.height);
    if (s.background != Background::Transparent) {
        P::Px px;
        const bool white = s.background == Background::White;
        switch (s.format.mode) {
            case P::Mode::Gray: px.c[0] = white ? 1.f : 0.f; break;
            case P::Mode::RGB:  px = white ? P::rgba(1, 1, 1) : P::rgba(0, 0, 0); break;
            case P::Mode::CMYK: px.c = {0, 0, 0, white ? 0.f : 1.f, 0}; break;   // no ink / K only
            case P::Mode::Lab:  px.c = {white ? 1.f : 0.f, 128.f / 255.f, 128.f / 255.f, 0, 0}; break;
        }
        if (s.background == Background::Colour) px = s.colour;
        if (s.format.alpha) px.c[static_cast<std::size_t>(s.format.channels() - 1)] = 1.f;
        m = m.fill({0, 0, s.width, s.height}, px);
    }
    bg.pixels = std::make_shared<const P::TileMap>(std::move(m));
    c.layers["L1"] = std::move(bg);
    c.next_id = 2;
    return c;
}

std::optional<P::TileMap> flatten(const Image& im, const std::map<Document::Id, Layer>& layers,
                                  const formats::Progress& p) {
    const P::Format f{im.format.depth, P::Mode::RGB, true};
    P::TileMap blank(f, im.width, im.height);
    std::vector<P::TileMap::TileRef> grid(static_cast<std::size_t>(blank.cols()) * static_cast<std::size_t>(blank.rows()));
    std::vector<float> tile;
    const std::size_t n = grid.size();
    for (int row = 0; row < blank.rows(); ++row)
        for (int col = 0; col < blank.cols(); ++col) {
            if (p.stop()) return std::nullopt;
            composite_tile(im, layers, col, row, tile);
            P::TileWriter w(f, P::transparent());
            w.set_premultiplied(tile);
            grid[static_cast<std::size_t>(row) * static_cast<std::size_t>(blank.cols()) + static_cast<std::size_t>(col)] = w.freeze();
            p.at(static_cast<double>(row * blank.cols() + col + 1) / static_cast<double>(n) * 0.5);
        }
    return P::TileMap::from_tiles(f, im.width, im.height, std::move(grid));
}

bool export_image(const std::string& path, formats::Kind kind, const Image& im,
                  const std::map<Document::Id, Layer>& layers, std::string& error,
                  const formats::Progress& p, const formats::WriteOptions& o) {
    auto flat = flatten(im, layers, p);
    if (!flat) { error = "cancelled"; return false; }
    formats::Raster r;
    r.dpi = im.dpi;
    // The profile describes the document's mode; the flat image is RGB, so
    // only an RGB document's profile still fits it.
    if (im.icc && im.format.mode == P::Mode::RGB) r.icc = *im.icc;
    if (kind == formats::Kind::JPEG) {
        // No alpha in a JPEG: what shows through is white, as on paper.
        const P::Format f{P::Depth::U8, P::Mode::RGB, false};
        r.pixels = P::TileMap(f, im.width, im.height).paint({0, 0, im.width, im.height}, [&](int x, int y, const P::Px&) {
            const P::Px s = flat->get(x, y);
            const float a = s.c[3];
            return P::rgba(s.c[0] * a + 1 - a, s.c[1] * a + 1 - a, s.c[2] * a + 1 - a);
        });
    } else {
        r.pixels = std::move(*flat);
    }
    formats::Progress rest = p;
    rest.progress = [&p](double f) { p.at(0.5 + f * 0.5); };
    switch (kind) {
        case formats::Kind::PNG:  return formats::write_png(path, r, error, rest);
        case formats::Kind::JPEG: return formats::write_jpeg(path, r, error, rest, o);
        case formats::Kind::TIFF: return formats::write_tiff(path, r, error, rest, o);
        default: break;
    }
    error = "export as PNG, JPEG or TIFF";
    return false;
}

}  // namespace klimt::core::file
