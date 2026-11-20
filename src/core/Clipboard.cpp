// core::clipboard -- Cut, Copy, Copy Merged, Paste, Paste in Place (s096;
// include/core/Clipboard.hpp, docs/clipboard.md).
#include "core/Clipboard.hpp"

#include "core/Selection.hpp"
#include "core/Transfer.hpp"

#include <algorithm>
#include <cmath>

namespace klimt::core::clipboard {

namespace P = pixel;

namespace {

float alpha_of(const P::TileMap& m, const P::Px& p) {
    return m.format().alpha ? p.c[static_cast<std::size_t>(m.format().colour_channels())] : 1.f;
}

bool same_family(P::Mode a, P::Mode b) {
    const auto easy = [](P::Mode m) { return m == P::Mode::RGB || m == P::Mode::Gray; };
    return a == b || (easy(a) && easy(b));
}

}  // namespace

P::Rect area(const Document& d) {
    const P::Rect all{0, 0, d.image().width, d.image().height};
    if (const auto* sel = d.selection()) return select::bounds(*sel).intersect(all);
    return all;
}

std::optional<Piece> cut_out(const P::TileMap& src, const P::TileMap* sel, const P::Rect& r_in,
                             const std::function<bool()>& cancelled) {
    const P::Rect r = r_in.intersect({0, 0, src.width(), src.height()});
    if (r.empty()) return std::nullopt;
    const auto cover = [&](int x, int y) {
        float a = alpha_of(src, src.get(x, y));
        if (sel) a *= sel->get(x, y).c[0];
        return a;
    };
    // What isn't clear: its bounds (a row at a time, so Cancel is heard).
    int x0 = r.x + r.w, y0 = r.y + r.h, x1 = r.x - 1, y1 = r.y - 1;
    for (int y = r.y; y < r.y + r.h; ++y) {
        if (cancelled && cancelled()) return std::nullopt;
        for (int x = r.x; x < r.x + r.w; ++x)
            if (cover(x, y) > 0.f) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    }
    if (x1 < x0 || y1 < y0) return std::nullopt;
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    const P::Format f{src.format().depth, src.format().mode, true};
    const auto cc = static_cast<std::size_t>(f.colour_channels());
    auto out = P::TileMap(f, w, h).paint({0, 0, w, h}, [&](int x, int y, const P::Px&) {
        const P::Px s = src.get(x + x0, y + y0);
        P::Px p{};
        for (std::size_t i = 0; i < cc; ++i) p.c[i] = s.c[i];
        p.c[cc] = cover(x + x0, y + y0);
        return p;
    });
    if (cancelled && cancelled()) return std::nullopt;
    Piece pc;
    pc.pixels = std::make_shared<const P::TileMap>(std::move(out));
    pc.x = x0;
    pc.y = y0;
    return pc;
}

std::vector<std::uint8_t> rgba8(const Piece& p) {
    if (!p.pixels) return {};
    const auto m = p.pixels->format().mode;
    if (m != P::Mode::RGB && m != P::Mode::Gray) return {};
    const P::TileMap rgb = transfer::converted(*p.pixels, P::Format{P::Depth::U8, P::Mode::RGB, true});
    const int w = rgb.width(), h = rgb.height();
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    const auto b = [](float v) { return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const P::Px px = rgb.get(x, y);
            auto* o = &out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4];
            for (int i = 0; i < 4; ++i) o[i] = b(px.c[static_cast<std::size_t>(i)]);
        }
    return out;
}

Piece from_rgba8(const std::uint8_t* data, int w, int h, int stride) {
    Piece pc;
    if (!data || w <= 0 || h <= 0) return pc;
    pc.pixels = std::make_shared<const P::TileMap>(
        P::TileMap(P::Format{P::Depth::U8, P::Mode::RGB, true}, w, h).paint({0, 0, w, h}, [&](int x, int y, const P::Px&) {
            const std::uint8_t* s = data + static_cast<std::ptrdiff_t>(y) * stride + static_cast<std::ptrdiff_t>(x) * 4;
            return P::rgba(s[0] / 255.f, s[1] / 255.f, s[2] / 255.f, s[3] / 255.f);
        }));
    return pc;
}

std::string refusal(const Document& d, const Piece& p) {
    if (d.count() == 0) return "no image -- File > New (Ctrl+N) or Open (Ctrl+O)";
    if (!p.pixels || p.width() <= 0 || p.height() <= 0) return "the clipboard holds no picture";
    if (!same_family(p.pixels->format().mode, d.image().format.mode))
        return "a picture of another colour mode -- a CMYK or Lab picture takes its own pixels only";
    return {};
}

std::optional<std::shared_ptr<const P::TileMap>> placed(const Image& im, const Piece& p, bool in_place) {
    if (!p.pixels || !same_family(p.pixels->format().mode, im.format.mode)) return std::nullopt;
    const P::TileMap piece = p.pixels->format() == im.format ? *p.pixels : transfer::converted(*p.pixels, im.format);
    const int ox = in_place ? p.x : (im.width - piece.width()) / 2;
    const int oy = in_place ? p.y : (im.height - piece.height()) / 2;
    const P::Rect r = P::Rect{ox, oy, piece.width(), piece.height()}.intersect({0, 0, im.width, im.height});
    P::TileMap blank(im.format, im.width, im.height);
    if (r.empty()) return std::make_shared<const P::TileMap>(std::move(blank));
    return std::make_shared<const P::TileMap>(
        blank.paint(r, [&](int x, int y, const P::Px&) { return piece.get(x - ox, y - oy); }));
}

Id land(DocJournal& j, Document& d, std::shared_ptr<const P::TileMap> px, const Id& above, bool in_place) {
    if (!px || px->format() != d.image().format || px->width() != d.image().width || px->height() != d.image().height) return {};
    return layers::add_layer_of(j, d, Kind::Pixel, "Pasted", above, [&](Layer& l) { l.pixels = px; },
                                in_place ? "Paste in Place" : "Paste");
}

Id paste(DocJournal& j, Document& d, const Piece& p, const Id& above, bool in_place) {
    if (!refusal(d, p).empty()) return {};
    const auto px = placed(d.image(), p, in_place);
    return px ? land(j, d, *px, above, in_place) : Id{};
}

}  // namespace klimt::core::clipboard
