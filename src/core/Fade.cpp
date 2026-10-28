// core::fade -- Edit > Fade (s020). docs/fade.md.
#include "core/Fade.hpp"
#include "core/Selection.hpp"   // set_pixels_if

#include <algorithm>
#include <vector>

namespace klimt::core::fade {

namespace {
constexpr std::size_t kPx = static_cast<std::size_t>(pixel::kTile) * pixel::kTile;

void load(const pixel::TileMap::TileRef& t, int n, std::vector<float>& out) {
    if (t) t->premultiplied(out);
    else out.assign(kPx * static_cast<std::size_t>(n), 0.f);
}

// One cell: before (b) and after (a) -> the faded tile.
pixel::TileMap::TileRef cell(const pixel::Format& f, const pixel::TileMap::TileRef& tb, const pixel::TileMap::TileRef& ta,
                             float o, pixel::Blend mode, int col, int row) {
    const int n = f.channels();
    std::vector<float> b, a;
    load(tb, n, b);
    load(ta, n, a);
    if (mode == pixel::Blend::Normal) {
        for (std::size_t i = 0; i < b.size(); ++i) b[i] = b[i] * (1.f - o) + a[i] * o;
    } else {
        // The compositor's RGBA: no alpha channel -> opaque.
        const bool alpha = f.alpha;
        std::vector<float> b4(kPx * 4), a4(kPx * 4);
        for (std::size_t i = 0; i < kPx; ++i)
            for (int k = 0; k < 4; ++k) {
                const bool is_a = k == 3;
                const std::size_t src = i * static_cast<std::size_t>(n) + static_cast<std::size_t>(k);
                b4[i * 4 + static_cast<std::size_t>(k)] = is_a && !alpha ? 1.f : b[src];
                a4[i * 4 + static_cast<std::size_t>(k)] = is_a && !alpha ? 1.f : a[src];
            }
        for (int y = 0; y < pixel::kTile; ++y) {
            const std::size_t at = static_cast<std::size_t>(y) * pixel::kTile * 4;
            pixel::blend_span(mode, a4.data() + at, b4.data() + at, pixel::kTile, o, col * pixel::kTile, row * pixel::kTile + y);
        }
        for (std::size_t i = 0; i < kPx; ++i)
            for (int k = 0; k < n; ++k) b[i * static_cast<std::size_t>(n) + static_cast<std::size_t>(k)] = b4[i * 4 + static_cast<std::size_t>(k)];
    }
    pixel::TileWriter w(f, pixel::transparent());
    w.set_premultiplied(b);
    return w.freeze();
}
}  // namespace

std::optional<Source> source(const DocJournal& j, const Document& d) {
    const auto* st = j.last_done();
    if (!st || st->entries.size() != 1) return std::nullopt;
    if (st->label.rfind("Fade ", 0) == 0) return std::nullopt;   // not a fade of a fade
    const auto& e = st->entries.front();
    if (e.id.empty() || e.id[0] == '@' || !e.before || !e.after) return std::nullopt;
    if (e.after->kind != Kind::Pixel || e.before->pixels == e.after->pixels || !e.after->pixels) return std::nullopt;
    Layer only = *e.before;
    only.pixels = e.after->pixels;
    if (!(only == *e.after)) return std::nullopt;   // the step changed more than the pixels
    const auto now = d.get(e.id);
    if (!now || now->pixels != e.after->pixels) return std::nullopt;   // the layer moved on
    return Source{e.id, st->label, e.before->pixels, e.after->pixels};
}

bool supports(const pixel::Format& f, pixel::Blend mode) {
    if (mode == pixel::Blend::PassThrough) return false;
    return mode == pixel::Blend::Normal || f.mode == pixel::Mode::RGB;
}

std::optional<pixel::TileMap> faded(const MapPtr& before_in, const pixel::TileMap& after, double opacity,
                                    pixel::Blend mode, const std::function<bool()>& stop) {
    const auto& f = after.format();
    if (!supports(f, mode)) mode = pixel::Blend::Normal;
    const float o = static_cast<float>(std::clamp(opacity, 0.0, 1.0));
    const pixel::TileMap before = before_in ? *before_in : pixel::TileMap(f, after.width(), after.height());
    std::vector<pixel::TileMap::TileRef> grid;
    grid.reserve(static_cast<std::size_t>(after.cols()) * static_cast<std::size_t>(after.rows()));
    for (int r = 0; r < after.rows(); ++r)
        for (int c = 0; c < after.cols(); ++c) {
            if (stop && stop()) return std::nullopt;
            const auto ta = after.tile(c, r);
            const auto tb = before.tile(c, r);
            if (after.same_tile(before, c, r) || (o >= 1.f && mode == pixel::Blend::Normal)) { grid.push_back(ta); continue; }
            grid.push_back(cell(f, tb, ta, o, mode, c, r));
        }
    // The pixels beyond the image (s010): every outside cell either holds.
    pixel::TileMap::Outside out;
    std::vector<pixel::TileMap::Cell> cells;
    for (const auto& [k, t] : after.outside_tiles()) cells.push_back(k);
    for (const auto& [k, t] : before.outside_tiles()) cells.push_back(k);
    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    for (const auto& k : cells) {
        if (stop && stop()) return std::nullopt;
        const auto ta = after.outside(k.first, k.second);
        const auto tb = before.outside(k.first, k.second);
        out[k] = ta == tb ? ta : cell(f, tb, ta, o, mode, k.first, k.second);
    }
    return after.with_grid(std::move(grid)).with_outside(std::move(out));
}

}  // namespace klimt::core::fade

namespace klimt::core::layers {

bool fade(DocJournal& j, Document& d, const Id& id, const fade::MapPtr& before, const fade::MapPtr& expect,
          double opacity, pixel::Blend mode, const std::string& what) {
    if (!expect) return false;
    auto out = fade::faded(before, *expect, opacity, mode);
    return out && set_pixels_if(j, d, id, expect, std::move(*out), "Fade " + what);
}

}  // namespace klimt::core::layers
