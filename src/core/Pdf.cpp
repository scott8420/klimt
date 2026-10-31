// core/Pdf -- Export as PDF: the picture, text layers kept as type (s051).
// See core/Pdf.hpp and docs/file-format.md "PDF".
#include "core/Pdf.hpp"
#include "core/Transfer.hpp"
#include "text/Text.hpp"

#include <cairo-pdf.h>
#include <cairo.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>

namespace klimt::core::pdf {

namespace P = pixel;

namespace {

using Layers = std::map<Document::Id, Layer>;
using Kids = std::map<std::string, std::vector<std::pair<double, std::string>>>;

std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

Kids kids_of(const Layers& ls) {
    Kids k;
    for (const auto& [id, L] : ls)
        if (!id.empty() && id[0] != '@') k[L.parent].emplace_back(L.pos, id);
    for (auto& [par, v] : k) std::sort(v.begin(), v.end());   // bottom first
    return k;
}

// Composited ALONE (onto nothing) and laid over what's below with Normal, does
// it look as it does in place? Yes for a layer that blends Normal with no
// styles (their effects blend with what's below), an isolated group that
// blends Normal, a Pass Through group whose every drawn child is so, and a
// path layer (it draws nothing; its mask on the layer below is the CANDIDATE's
// test). Never for an adjustment or filter layer: it acts on what's below.
bool safe(const Layers& ls, const Kids& kids, const Document::Id& id) {
    const Layer& L = ls.at(id);
    if (!L.visible) return true;
    switch (L.kind) {
        case Kind::Adjustment:
        case Kind::Filter: return false;
        case Kind::Path: return true;
        case Kind::Group: {
            if (L.blend != Blend::PassThrough) return L.blend == Blend::Normal && !L.styles.any();
            const auto it = kids.find(id);
            if (it == kids.end()) return true;
            return std::all_of(it->second.begin(), it->second.end(), [&](const auto& k) { return safe(ls, kids, k.second); });
        }
        default: return L.blend == Blend::Normal && !L.styles.any();
    }
}

enum class Why { Live, Group, Look, Base, Above, Empty, Mode };

// Every text layer's fate: kept as type, or why not.
std::map<Document::Id, Why> judge(const Image& im, const Layers& ls, const Kids& kids) {
    std::map<Document::Id, Why> out;
    const bool rgb = im.format.mode == P::Mode::RGB;
    for (const auto& [id, L] : ls) {
        if (id.empty() || id[0] == '@' || L.kind != Kind::Text || !L.visible) continue;
        if (!L.text || !L.pixels) { out[id] = Why::Empty; continue; }
        if (!rgb) { out[id] = Why::Mode; continue; }
        if (!L.parent.empty()) { out[id] = Why::Group; continue; }
        if (L.blend != Blend::Normal || L.styles.any() || (L.mask && L.mask_on) || L.clipped) { out[id] = Why::Look; continue; }
        out[id] = Why::Live;
    }
    const auto it = kids.find("");
    if (it == kids.end()) return out;
    const auto& top = it->second;
    for (std::size_t i = 0; i < top.size(); ++i) {
        const auto f = out.find(top[i].second);
        if (f == out.end() || f->second != Why::Live) continue;
        // The layers directly above: a clipped one makes this its base; a
        // path layer masking the layer below it masks this.
        for (std::size_t j = i + 1; j < top.size(); ++j) {
            const Layer& A = ls.at(top[j].second);
            if (A.kind == Kind::Path && A.path_mask && j == i + 1) { f->second = Why::Base; break; }
            if (!A.clipped) break;
            if (A.visible) { f->second = Why::Base; break; }
        }
        if (f->second != Why::Live) continue;
        for (std::size_t j = i + 1; j < top.size(); ++j)
            if (!safe(ls, kids, top[j].second)) { f->second = Why::Above; break; }
    }
    return out;
}

// The smallest box holding a pixel whose alpha isn't 0 (8-bit RGBA map).
struct Box { int x = 0, y = 0, w = 0, h = 0; };
Box ink_box(const P::TileMap& m) {
    const int W = m.width(), H = m.height();
    std::vector<std::uint8_t> row(static_cast<std::size_t>(W) * 4);
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y) {
        bool any_tile = false;
        for (int c = 0; c < m.cols() && !any_tile; ++c) any_tile = m.tile(c, y / P::kTile) != nullptr;
        if (!any_tile) continue;
        m.row_bytes(y, row.data());
        for (int x = 0; x < W; ++x)
            if (row[static_cast<std::size_t>(x) * 4 + 3]) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
    }
    if (x1 < 0) return {};
    return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// A band's composite laid on the page as an image (premultiplied ARGB, cut to its ink).
void lay_image(cairo_t* cr, const P::TileMap& band8) {
    const Box b = ink_box(band8);
    if (b.w <= 0) return;
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, b.w, b.h);
    unsigned char* d = cairo_image_surface_get_data(s);
    const int stride = cairo_image_surface_get_stride(s);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(band8.width()) * 4);
    for (int y = 0; y < b.h; ++y) {
        band8.row_bytes(b.y + y, row.data());
        auto* out = reinterpret_cast<std::uint32_t*>(d + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride));
        for (int x = 0; x < b.w; ++x) {
            const std::uint8_t* px = row.data() + static_cast<std::size_t>(b.x + x) * 4;
            const unsigned a = px[3];
            const auto pm = [a](unsigned v) { return (v * a + 127) / 255; };
            out[x] = (a << 24) | (pm(px[0]) << 16) | (pm(px[1]) << 8) | pm(px[2]);
        }
    }
    cairo_surface_mark_dirty(s);
    cairo_set_source_surface(cr, s, b.x, b.y);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_surface_destroy(s);
}

// The layer and everything under it (a group's tree) into `out`.
void take(const Layers& ls, const Kids& kids, const Document::Id& id, Layers& out) {
    out[id] = ls.at(id);
    const auto it = kids.find(id);
    if (it == kids.end()) return;
    for (const auto& [pos, k] : it->second) take(ls, kids, k, out);
}

}  // namespace

Plan plan(const Image& im, const std::map<Document::Id, Layer>& layers) {
    const Kids kids = kids_of(layers);
    const auto fate = judge(im, layers, kids);
    Plan pl;
    int live = 0, group = 0, look = 0, base = 0, above = 0, mode = 0;
    for (const auto& [id, w] : fate) {
        switch (w) {
            case Why::Live: ++live; break;
            case Why::Group: ++group; break;
            case Why::Look: ++look; break;
            case Why::Base: ++base; break;
            case Why::Above: ++above; break;
            case Why::Mode: ++mode; break;
            case Why::Empty: break;
        }
    }
    if (const auto it = kids.find(""); it != kids.end())
        for (const auto& [pos, id] : it->second)
            if (const auto f = fate.find(id); f != fate.end() && f->second == Why::Live) pl.live.push_back(id);
    auto& L = pl.list;
    if (live) L.kept.push_back(plural(live, "text layer", "text layers") + " as type -- sharp at any size, its fonts embedded");
    if (above) L.baked.push_back(plural(above, "text layer", "text layers") + " as pixels -- under a layer that blends, has styles or adjusts");
    if (look) L.baked.push_back(plural(look, "text layer", "text layers") + " as pixels -- its own blend, styles, mask or clipping");
    if (base) L.baked.push_back(plural(base, "text layer", "text layers") + " as pixels -- a layer is clipped or path-masked to it");
    if (group) L.baked.push_back(plural(group, "text layer", "text layers") + " as pixels -- inside a group");
    if (mode) L.baked.push_back(plural(mode, "text layer", "text layers") + " as pixels -- not an RGB document");
    if (im.format.mode != P::Mode::RGB) L.left_out.push_back("written as RGB");
    if (im.format.depth != P::Depth::U8) L.left_out.push_back("the picture at 8 bits (a PDF image's depth here)");
    if (im.icc && !im.icc->empty()) L.left_out.push_back("the colour profile (the picture is plain RGB)");
    return pl;
}

bool save(const std::string& path, const Image& im, const std::map<Document::Id, Layer>& layers,
          const std::string& title, std::string& error, std::vector<std::string>& notes, const formats::Progress& p) {
    const int W = im.width, H = im.height;
    if (W < 1 || H < 1) { error = "an empty image"; return false; }
    const double dpi = im.dpi > 0 ? im.dpi : 72;
    const double k = 72.0 / dpi;   // pixels -> points
    const double wpt = W * k, hpt = H * k;
    if (wpt > 14400 || hpt > 14400) {   // 200 in: PDF 1.x's page limit
        error = "a page past 200 inches -- raise the resolution (Image > Image Size, without resampling)";
        return false;
    }
    const Plan pl = plan(im, layers);
    const Kids kids = kids_of(layers);
    formats::Progress cancel_only;
    cancel_only.cancelled = p.cancelled;
    const P::Format f8{P::Depth::U8, P::Mode::RGB, true};

    // The bands: the top-level layers between the live text layers.
    std::vector<Layers> bands(1);
    std::vector<Document::Id> types;   // the type after each band
    if (const auto it = kids.find(""); it != kids.end())
        for (const auto& [pos, id] : it->second) {
            if (std::find(pl.live.begin(), pl.live.end(), id) != pl.live.end()) {
                types.push_back(id);
                bands.emplace_back();
                continue;
            }
            take(layers, kids, id, bands.back());
        }

    const std::string tmp = path + ".partial";
    std::remove(tmp.c_str());
    cairo_surface_t* surf = cairo_pdf_surface_create(tmp.c_str(), wpt, hpt);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        error = "can't be written";
        return false;
    }
    cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_TITLE, title.c_str());
    cairo_pdf_surface_set_metadata(surf, CAIRO_PDF_METADATA_CREATOR, "Klimt");
    cairo_t* cr = cairo_create(surf);
    cairo_scale(cr, k, k);   // image pixels from here on
    cairo_rectangle(cr, 0, 0, W, H);
    cairo_clip(cr);
    bool ok = true;
    for (std::size_t b = 0; b < bands.size() && ok; ++b) {
        if (p.stop()) { ok = false; error = "cancelled"; break; }
        if (!bands[b].empty()) {
            auto flat = file::flatten(im, bands[b], cancel_only);
            if (!flat) { ok = false; error = "cancelled"; break; }
            const P::TileMap m = flat->format() == f8 ? std::move(*flat) : transfer::converted(*flat, f8);
            lay_image(cr, m);
        }
        if (b < types.size()) {
            const Layer& T = layers.at(types[b]);
            text::draw(cr, *T.text, T.opacity / 100.0 * T.fill / 100.0);
        }
        p.at(0.9 * static_cast<double>(b + 1) / static_cast<double>(bands.size()));
    }
    cairo_destroy(cr);
    cairo_surface_finish(surf);
    const cairo_status_t st = cairo_surface_status(surf);
    cairo_surface_destroy(surf);
    if (!ok || st != CAIRO_STATUS_SUCCESS) {
        std::remove(tmp.c_str());
        if (ok) error = std::string("can't be written: ") + cairo_status_to_string(st);
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::remove(tmp.c_str()); error = "can't be written: " + ec.message(); return false; }
    for (const auto& n : pl.list.kept) notes.push_back(n.substr(0, n.find(" -- ")));
    for (const auto& n : pl.list.baked) notes.push_back(n.substr(0, n.find(" -- ")));
    p.at(1);
    return true;
}

}  // namespace klimt::core::pdf
