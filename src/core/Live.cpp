// core::live -- live filters (s018). See core/Live.hpp.
#include "core/Live.hpp"
#include "core/StylesRender.hpp"   // s019
#include "core/Fade.hpp"           // s023: a live filter's blending
#include "core/Paths.hpp"          // s035: path layers as masks
#include "core/Matte.hpp"          // s036: Defringe

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>

namespace klimt::core::live {

namespace {
struct Entry {
    std::shared_ptr<const pixel::TileMap> src;   // held: the pointer stays unique while cached
    std::shared_ptr<const pixel::TileMap> mask;  // m3: the filter mask (nullptr: none)
    std::shared_ptr<const pixel::TileMap> depth; // s039: the layer mask, when a Lens Blur reads it
    std::vector<LiveFilter> stack;
    Styles styles;   // s019 (and the fill, which styles keep off their effects)
    int fill = 100;
    std::shared_future<std::shared_ptr<const pixel::TileMap>> result;
    std::uint64_t used = 0;
};
std::mutex g_mutex;
std::atomic<int> g_partly{0};   // s021: looks made in part (the selftest reads it)
std::vector<Entry> g_cache;
std::uint64_t g_clock = 0;
constexpr std::size_t kKeep = 16;   // s025: a split style keeps a look per part

std::vector<LiveFilter> on_only(const std::vector<LiveFilter>& s) {
    std::vector<LiveFilter> v;
    for (const auto& f : s) if (f.on && f.f.kind != FilterKind::None) v.push_back(f);
    return v;
}

// The whole look of `src`: the stack, the filter mask, the styles.
std::shared_ptr<const pixel::TileMap> whole(const pixel::TileMap& src, const std::vector<LiveFilter>& stack,
                                            const pixel::TileMap* mask, const Styles& st, int fill,
                                            const pixel::TileMap* depth = nullptr) {
    auto out = run(src, stack, nullptr, {}, depth);
    if (out && mask) out = masked(src, *out, *mask);
    if (out && st.any()) out = styles::drawn(*out, st, fill);
    return out ? std::make_shared<const pixel::TileMap>(std::move(*out)) : nullptr;
}

// s021 -- the cells [c0, c1) x [r0, r1) of `m` as a map of their own (the
// same tiles, the grid starting at (c0, r0)); the last ones keep the image's
// own edge.
pixel::TileMap cells_of(const pixel::TileMap& m, int c0, int r0, int c1, int r1) {
    const int w = std::min(c1 * pixel::kTile, m.width()) - c0 * pixel::kTile;
    const int h = std::min(r1 * pixel::kTile, m.height()) - r0 * pixel::kTile;
    std::vector<pixel::TileMap::TileRef> grid;
    grid.reserve(static_cast<std::size_t>(c1 - c0) * static_cast<std::size_t>(r1 - r0));
    for (int r = r0; r < r1; ++r)
        for (int c = c0; c < c1; ++c) grid.push_back(m.tile(c, r));
    return pixel::TileMap::from_tiles(m.format(), w, h, std::move(grid));
}

// s021 (WEAKNESSES 38) -- PAINTING A LIVE LAYER. A stroke changes a few
// tiles; the look before it (`was`, of `was_src`) is right everywhere the
// change can't reach. So: the cells that changed, grown by the look's reach
// (the cells whose look may differ), worked in a WINDOW grown by the reach
// again (so their every input is in it -- or the window meets the image's
// own edge, where the whole run clamps the same way); the rest of the look
// is `was`'s tiles, shared. nullptr: do it whole (too much changed, pixels
// beyond the image, or a filter that depends on where it is -- Add Noise).
std::shared_ptr<const pixel::TileMap> partly(const pixel::TileMap& src, const pixel::TileMap& was_src,
                                             const pixel::TileMap& was, const std::vector<LiveFilter>& stack,
                                             const pixel::TileMap* mask, const Styles& st, int fill) {
    if (!(src.format() == was_src.format()) || src.width() != was_src.width() || src.height() != was_src.height() ||
        was.width() != src.width() || was.height() != src.height())
        return nullptr;
    if (src.has_outside() || was_src.has_outside() || was.has_outside()) return nullptr;
    for (const auto& f : stack)   // they depend on where they are: Add Noise, s023 a Dissolve blend
        if (filter::placed(f.f) || f.mode == pixel::Blend::Dissolve) return nullptr;   // s041: Add Noise, Lens's Noise, Radial, Distort
    int reach_px = 0;
    for (const auto& f : stack) reach_px += filter::reach(f.f);
    if (st.any()) reach_px += styles::reach(st);
    const int k = (reach_px + 8 + pixel::kTile - 1) / pixel::kTile;   // cells; the guard covers rounding at the edges
    const int cols = src.cols(), rows = src.rows();
    int changed = 0;
    int c0 = cols, r0 = rows, c1 = -1, r1 = -1;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            if (!src.same_tile(was_src, c, r)) {
                ++changed;
                c0 = std::min(c0, c); c1 = std::max(c1, c);
                r0 = std::min(r0, r); r1 = std::max(r1, r);
            }
    if (changed == 0) return std::make_shared<const pixel::TileMap>(was);
    // The cells whose look may change: the changed ones' box, grown by k.
    const int a0 = std::max(0, c0 - k), a1 = std::min(cols, c1 + k + 1);
    const int b0 = std::max(0, r0 - k), b1 = std::min(rows, r1 + k + 1);
    if ((a1 - a0) * (b1 - b0) * 10 > cols * rows * 6) return nullptr;   // most of it: whole is as cheap
    // The window: those, grown by k again.
    const int w0 = std::max(0, a0 - k), w1 = std::min(cols, a1 + k);
    const int v0 = std::max(0, b0 - k), v1 = std::min(rows, b1 + k);
    const auto win_src = cells_of(src, w0, v0, w1, v1);
    std::optional<pixel::TileMap> win_mask;
    if (mask) win_mask = cells_of(*mask, w0, v0, w1, v1);
    const auto look_win = whole(win_src, stack, win_mask ? &*win_mask : nullptr, st, fill);
    if (!look_win || look_win->has_outside()) return nullptr;
    std::vector<pixel::TileMap::TileRef> grid;
    grid.reserve(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows));
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            grid.push_back(c >= a0 && c < a1 && r >= b0 && r < b1 ? look_win->tile(c - w0, r - v0) : was.tile(c, r));
    return std::make_shared<const pixel::TileMap>(was.with_grid(std::move(grid)));
}
}  // namespace

bool wants_mask(const std::vector<LiveFilter>& stack) {
    for (const auto& f : stack) if (f.on && filter::wants_mask(f.f)) return true;
    return false;
}

std::optional<pixel::TileMap> run(const pixel::TileMap& src, const std::vector<LiveFilter>& stack, const pixel::Rect* only,
                                  const std::function<bool()>& cancelled, const pixel::TileMap* depth) {
    const auto on = on_only(stack);
    pixel::TileMap cur = src;
    for (std::size_t i = 0; i < on.size(); ++i) {
        std::optional<pixel::Rect> r;
        if (only) {   // this stage must be right wherever the later stages read
            int later = 0;
            for (std::size_t k = i + 1; k < on.size(); ++k) later += filter::reach(on[k].f);
            r = pixel::Rect{only->x - later, only->y - later, only->w + 2 * later, only->h + 2 * later};
        }
        // s041 m4: the layer's mask only to a Lens Blur reading it (a Channel
        // one sees no channel here -- even).
        auto next = filter::filtered(cur, on[i].f, nullptr, false, r ? &*r : nullptr, cancelled, {}, filter::wants_mask(on[i].f) ? depth : nullptr);
        if (!next) return std::nullopt;
        // s023: its blending -- the result faded over what went in.
        if (on[i].opacity < 100 || on[i].mode != pixel::Blend::Normal) {
            auto blended = fade::faded(std::make_shared<const pixel::TileMap>(cur), *next,
                                       std::clamp(on[i].opacity, 0, 100) / 100.0, on[i].mode, cancelled);
            if (!blended) return std::nullopt;
            next = std::move(blended);
        }
        cur = std::move(*next);
    }
    return cur;
}

pixel::TileMap masked(const pixel::TileMap& raw, const pixel::TileMap& filtered, const pixel::TileMap& hide) {
    const auto& f = raw.format();
    const int C = f.channels(), cc = f.colour_channels();
    const bool alpha = f.alpha;
    pixel::TileMap out = filtered;
    // Every tile either map holds (a blur grows past the pixels; the mask may hide there too).
    for (int row = 0; row < raw.rows(); ++row)
        for (int col = 0; col < raw.cols(); ++col) {
            const auto mt = hide.tile(col, row);
            if (!mt) continue;                                            // hides nothing here
            if (!raw.tile(col, row) && !filtered.tile(col, row)) continue;
            const pixel::Rect cell = pixel::Rect{col * pixel::kTile, row * pixel::kTile, pixel::kTile, pixel::kTile}
                                         .intersect({0, 0, raw.width(), raw.height()});
            out = out.paint(cell, [&](int x, int y, const pixel::Px& fp) {
                const float h = std::clamp(mt->get(x - col * pixel::kTile, y - row * pixel::kTile).c[0], 0.f, 1.f);
                if (h <= 0.f) return fp;
                const pixel::Px rp = raw.get(x, y);
                // Premultiplied mix, so a hidden blur's spread fades to nothing cleanly.
                const float fa = alpha ? fp.c[static_cast<std::size_t>(cc)] : 1.f, ra = alpha ? rp.c[static_cast<std::size_t>(cc)] : 1.f;
                const float a = fa + (ra - fa) * h;
                pixel::Px p{};
                for (int k = 0; k < cc; ++k) {
                    const float v = fp.c[static_cast<std::size_t>(k)] * fa + (rp.c[static_cast<std::size_t>(k)] * ra - fp.c[static_cast<std::size_t>(k)] * fa) * h;
                    p.c[static_cast<std::size_t>(k)] = a > 1e-6f ? std::clamp(v / a, 0.f, 1.f) : 0.f;
                }
                if (alpha) p.c[static_cast<std::size_t>(cc)] = a;
                (void)C;
                return p;
            });
        }
    return out;
}

bool active(const Layer& l) {
    if (l.kind != Kind::Pixel || !l.pixels) return false;
    if (l.styles.any()) return true;   // s019
    for (const auto& f : l.filters) if (f.on && f.f.kind != FilterKind::None) return true;
    return false;
}

bool any(const Layers& ls) {
    for (const auto& [id, l] : ls) {
        if (active(l)) return true;
        if (l.kind == Kind::Filter) return true;   // s030
        if (l.kind == Kind::Path && l.path_mask && l.visible) return true;   // s035: a path masking its neighbour
        if (l.kind != Kind::Path && l.mask && l.mask_on && l.edges.any()) return true;   // s036: a mask's edges
        if (l.kind == Kind::Group && l.styles.any()) return true;   // s024
    }
    return false;
}

int reach(const Layer& l) {
    int r = 0;
    for (const auto& f : on_only(l.filters)) r += filter::reach(f.f);
    return r;
}

std::shared_ptr<const pixel::TileMap> look(const Layer& l) {
    if (!active(l)) return l.pixels;
    const auto stack = on_only(l.filters);
    const auto depth = wants_mask(stack) ? l.mask : nullptr;   // s039
    std::shared_future<std::shared_ptr<const pixel::TileMap>> fut;
    std::promise<std::shared_ptr<const pixel::TileMap>> mine;
    bool compute = false;
    std::shared_ptr<const pixel::TileMap> parent_src;
    std::shared_future<std::shared_ptr<const pixel::TileMap>> parent;
    {
        std::lock_guard g(g_mutex);
        for (auto& e : g_cache)
            if (e.src == l.pixels && e.mask == l.filter_mask && e.depth == depth && e.stack == stack && e.styles == l.styles &&
                (!l.styles.any() || e.fill == l.fill)) { e.used = ++g_clock; fut = e.result; break; }
        if (!fut.valid()) {
            if (g_cache.size() >= kKeep) {   // the least used, finished entry goes
                auto victim = g_cache.end();
                for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
                    if (it->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
                        (victim == g_cache.end() || it->used < victim->used)) victim = it;
                if (victim != g_cache.end()) g_cache.erase(victim);
            }
            // s021: the latest look with this stack / mask / styles, of other
            // pixels of the same size -- likely the same layer a dab ago.
            std::uint64_t best = 0;
            for (const auto& e : g_cache)
                if (e.mask == l.filter_mask && e.depth == depth && e.stack == stack && e.styles == l.styles && (!l.styles.any() || e.fill == l.fill) &&
                    e.src && e.src->width() == l.pixels->width() && e.src->height() == l.pixels->height() && e.used > best) {
                    best = e.used;
                    parent_src = e.src;
                    parent = e.result;
                }
            fut = mine.get_future().share();
            g_cache.push_back({l.pixels, l.filter_mask, depth, stack, l.styles, l.fill, fut, ++g_clock});
            compute = true;
        }
    }
    if (compute) {
        // s021: a look of the same stack, mask and styles over pixels that
        // share most tiles with these (the stroke before) -- only the change
        // and its reach are worked.
        std::shared_ptr<const pixel::TileMap> out;
        if (parent_src && parent.valid() && !depth) {   // s039: a depth map -- whole
            const auto was = parent.get();
            if (was) out = partly(*l.pixels, *parent_src, *was, stack, l.filter_mask.get(), l.styles, l.fill);
            if (out) g_partly.fetch_add(1, std::memory_order_relaxed);
        }
        if (!out) out = whole(*l.pixels, stack, l.filter_mask.get(), l.styles, l.fill, depth.get());   // s019: the effects, the fill on the content
        mine.set_value(out ? out : l.pixels);
    }
    return fut.get();
}

namespace {
// s024: the cache of styled groups' looks -- keyed by the group's id, its
// whole subtree (values: pixels by pointer), the image.
struct GroupEntry {
    Document::Id id;
    Layers sub;
    Layer group;
    int w = 0, h = 0;
    std::shared_future<std::shared_ptr<const pixel::TileMap>> result;
    std::uint64_t used = 0;
};
std::mutex g_gmutex;
std::vector<GroupEntry> g_groups;
std::uint64_t g_gclock = 0;

bool inside(const Layers& ls, const Document::Id& id, const Document::Id& gid) {
    for (Document::Id p = ls.at(id).parent; !p.empty();) {
        if (p == gid) return true;
        const auto it = ls.find(p);
        if (it == ls.end()) return false;
        p = it->second.parent;
    }
    return false;
}
}  // namespace

std::shared_ptr<const pixel::TileMap> group_look(const Image& im, const Layers& ls, const Document::Id& gid) {
    const auto git = ls.find(gid);
    if (git == ls.end() || im.format.mode != pixel::Mode::RGB || !im.format.alpha || im.width <= 0 || im.height <= 0) return nullptr;
    const Layer& g = git->second;
    // Its contents, the group's place taken by the top level.
    Layers sub;
    for (const auto& [id, l] : ls)
        if (inside(ls, id, gid)) {
            Layer v = l;
            if (v.parent == gid) v.parent.clear();
            sub[id] = std::move(v);
        }
    std::shared_future<std::shared_ptr<const pixel::TileMap>> fut;
    std::promise<std::shared_ptr<const pixel::TileMap>> mine;
    bool compute = false;
    {
        std::lock_guard lk(g_gmutex);
        for (auto& e : g_groups)
            if (e.id == gid && e.w == im.width && e.h == im.height && e.sub == sub) {   // s025: the contents only
                e.used = ++g_gclock;
                fut = e.result;
                break;
            }
        if (!fut.valid()) {
            if (g_groups.size() >= 6) {
                auto victim = g_groups.end();
                for (auto it = g_groups.begin(); it != g_groups.end(); ++it)
                    if (it->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
                        (victim == g_groups.end() || it->used < victim->used)) victim = it;
                if (victim != g_groups.end()) g_groups.erase(victim);
            }
            fut = mine.get_future().share();
            g_groups.push_back({gid, sub, g, im.width, im.height, fut, ++g_gclock});
            compute = true;
        }
    }
    if (compute) {
        // The contents composited, isolated, tile by tile (live layers and
        // styled groups inside it as they look -- composite_tile sees to that).
        pixel::TileMap content(im.format, im.width, im.height);
        std::vector<pixel::TileMap::TileRef> grid;
        grid.reserve(static_cast<std::size_t>(content.cols()) * static_cast<std::size_t>(content.rows()));
        std::vector<float> px;
        for (int r = 0; r < content.rows(); ++r)
            for (int c = 0; c < content.cols(); ++c) {
                composite_tile(im, sub, c, r, px);
                bool any_px = false;
                for (std::size_t i = 3; i < px.size(); i += 4) if (px[i] > 0.f) { any_px = true; break; }
                if (!any_px) { grid.push_back(nullptr); continue; }
                pixel::TileWriter w(im.format, pixel::transparent());
                w.set_premultiplied(px);
                grid.push_back(w.freeze());
            }
        const auto flat = pixel::TileMap::from_tiles(im.format, im.width, im.height, std::move(grid));
        mine.set_value(std::make_shared<const pixel::TileMap>(flat));   // s025: flat; look() draws the styles (cached there)
        (void)g;
    }
    return fut.get();
}

namespace {
// s030: the cache of FILTER LAYERS' looks -- keyed by the layer's id, what it
// reads (the layers below it in its scope; values, pixels by pointer), its
// filter and the image's size.
struct FilterEntry {
    Document::Id id;
    Layers sub;
    std::vector<LiveFilter> stack;
    int w = 0, h = 0;
    std::shared_future<std::shared_ptr<const pixel::TileMap>> result;
    std::uint64_t used = 0;
};
std::mutex g_fmutex;
std::vector<FilterEntry> g_filters;
std::uint64_t g_fclock = 0;

// What a filter layer reads: unclipped, the picture BELOW it in its scope --
// the nearest enclosing group that isn't Pass Through (an isolated group
// composites alone), or the whole document -- the layer and everything above
// it hidden; clipped, its clip base alone, as it looks.
Layers filter_source(const Layers& ls, const Document::Id& id) {
    const Layer& f = ls.at(id);
    if (f.clipped) {   // the nearest unclipped sibling below
        const Layer* base = nullptr;
        Document::Id bid;
        for (const auto& [oid, o] : ls)
            if (o.parent == f.parent && o.pos < f.pos && !o.clipped && (!base || o.pos > base->pos)) { base = &o; bid = oid; }
        if (base && base->kind == Kind::Pixel) {
            Layer b = *base;
            b.parent.clear();
            b.clipped = false;
            b.opacity = 100;
            b.fill = b.styles.any() ? b.fill : 100;
            b.blend = pixel::Blend::Normal;
            b.mask = nullptr;   // the compositor clips by the base's own alpha and mask afterwards
            return Layers{{bid, b}};
        }
    }
    Document::Id scope;
    for (Document::Id p = f.parent; !p.empty();) {
        const auto it = ls.find(p);
        if (it == ls.end()) break;
        if (it->second.blend != pixel::Blend::PassThrough) { scope = p; break; }
        p = it->second.parent;
    }
    const Layers hid = below(ls, id);   // the layer and those above it, at every level, hidden
    Layers sub;
    for (const auto& [oid, o] : hid) {
        if (!scope.empty() && !inside(hid, oid, scope)) continue;
        Layer v = o;
        if (!scope.empty() && v.parent == scope) v.parent.clear();
        sub[oid] = std::move(v);
    }
    // Hidden ones change nothing: drop them, so the key is what shows.
    for (auto it = sub.begin(); it != sub.end();) it = it->second.visible ? std::next(it) : sub.erase(it);
    return sub;
}

std::shared_ptr<const pixel::TileMap> filter_look(const Image& im, const Layers& ls, const Document::Id& id) {
    const Layer& f = ls.at(id);
    if (im.format.mode != pixel::Mode::RGB || im.width <= 0 || im.height <= 0 || f.filters.size() != 1) return nullptr;
    Layers sub = filter_source(ls, id);
    std::vector<LiveFilter> stack = f.filters;
    stack[0].on = true;
    stack[0].opacity = 100;
    stack[0].mode = pixel::Blend::Normal;
    std::shared_future<std::shared_ptr<const pixel::TileMap>> fut;
    std::promise<std::shared_ptr<const pixel::TileMap>> mine;
    bool compute = false;
    {
        std::lock_guard lk(g_fmutex);
        for (auto& e : g_filters)
            if (e.id == id && e.w == im.width && e.h == im.height && e.stack == stack && e.sub == sub) {
                e.used = ++g_fclock;
                fut = e.result;
                break;
            }
        if (!fut.valid()) {
            if (g_filters.size() >= 8) {
                auto victim = g_filters.end();
                for (auto it = g_filters.begin(); it != g_filters.end(); ++it)
                    if (it->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
                        (victim == g_filters.end() || it->used < victim->used)) victim = it;
                if (victim != g_filters.end()) g_filters.erase(victim);
            }
            fut = mine.get_future().share();
            g_filters.push_back({id, sub, stack, im.width, im.height, fut, ++g_fclock});
            compute = true;
        }
    }
    if (compute) {
        // What it reads, flattened (RGB with alpha, the image's depth) -- the
        // layers in it as they look (composite_tile sees to that, filter
        // layers below this one included) -- then the filter over it.
        const pixel::Format ff{im.format.depth, pixel::Mode::RGB, true};
        pixel::TileMap flat(ff, im.width, im.height);
        std::vector<pixel::TileMap::TileRef> grid;
        grid.reserve(static_cast<std::size_t>(flat.cols()) * static_cast<std::size_t>(flat.rows()));
        std::vector<float> px;
        Image fim = im;
        fim.format = ff;
        for (int r = 0; r < flat.rows(); ++r)
            for (int c = 0; c < flat.cols(); ++c) {
                composite_tile(fim, sub, c, r, px);
                bool any_px = false;
                for (std::size_t i = 3; i < px.size(); i += 4) if (px[i] > 0.f) { any_px = true; break; }
                if (!any_px) { grid.push_back(nullptr); continue; }
                pixel::TileWriter w(ff, pixel::transparent());
                w.set_premultiplied(px);
                grid.push_back(w.freeze());
            }
        const auto src = pixel::TileMap::from_tiles(ff, im.width, im.height, std::move(grid));
        auto out = run(src, stack);
        mine.set_value(out ? std::make_shared<const pixel::TileMap>(std::move(*out)) : nullptr);
    }
    return fut.get();
}
// s035: PATH LAYERS AS MASKS. A path layer with its Mask switch on (and
// visible) hides what lies outside its outline on the sibling just below it:
// that layer's mask becomes max(its own hide, 1 - the path's coverage). The
// coverage is made once per outline (by pointer) and image size, and cached.
std::shared_ptr<const pixel::TileMap> path_cover(const std::shared_ptr<const path::Path>& p, const pixel::Format& mf, int w, int h) {
    struct Entry { std::shared_ptr<const path::Path> p; int w, h; pixel::Format f; std::shared_ptr<const pixel::TileMap> m; };
    static std::mutex mu;
    static std::vector<Entry> cache;
    std::lock_guard lock(mu);
    for (const auto& e : cache)
        if (e.p == p && e.w == w && e.h == h && e.f == mf) return e.m;
    auto m = std::make_shared<const pixel::TileMap>(paths::coverage(*p, mf, w, h));
    cache.insert(cache.begin(), {p, w, h, mf, m});
    if (cache.size() > 8) cache.pop_back();
    return m;
}

std::shared_ptr<const pixel::TileMap> path_hide(const std::shared_ptr<const path::Path>& p, const std::shared_ptr<const pixel::TileMap>& own,
                                                const pixel::Format& mf, int w, int h, const MaskEdges& ed) {
    struct Entry { std::shared_ptr<const path::Path> p; std::shared_ptr<const pixel::TileMap> own; int w, h; pixel::Format f;
                   double density, feather; std::shared_ptr<const pixel::TileMap> hide; };
    static std::mutex mu;
    static std::vector<Entry> cache;
    const auto same = [&](const Entry& e) {
        return e.p == p && e.own == own && e.w == w && e.h == h && e.f == mf && e.density == ed.density && e.feather == ed.feather;
    };
    {
        std::lock_guard lock(mu);
        for (const auto& e : cache)
            if (same(e)) return e.hide;
    }
    const auto cover = path_cover(p, mf, w, h);
    pixel::TileMap hide = select::inverted(cover.get(), mf, w, h);
    // s036: the path's own edges -- its outline softened, its hold weakened.
    if (ed.feather > 0)
        if (auto f = select::feathered(hide, ed.feather)) hide = std::move(*f);
    if (ed.density < 100) hide = select::scaled(hide, static_cast<float>(ed.density / 100));
    if (own) hide = select::combine(&hide, *own, select::Op::Add);
    auto made = std::make_shared<const pixel::TileMap>(std::move(hide));
    std::lock_guard lock(mu);
    for (const auto& e : cache)   // another tile made it meanwhile: share that one
        if (same(e)) return e.hide;
    cache.insert(cache.begin(), {p, own, w, h, mf, ed.density, ed.feather, made});
    if (cache.size() > 8) cache.pop_back();
    return made;
}

}  // namespace

// s036: a MASK'S EDGES. The mask as it acts -- feathered (three box passes,
// select::feathered), then weakened by its density -- made once per mask (by
// pointer) and setting, and cached like a look.
std::shared_ptr<const pixel::TileMap> edged_mask(const std::shared_ptr<const pixel::TileMap>& m, const MaskEdges& ed) {
    if (!m || (ed.density >= 100 && ed.feather <= 0)) return m;
    using Ptr = std::shared_ptr<const pixel::TileMap>;
    struct Entry { Ptr m; double density, feather; std::shared_future<Ptr> out; };
    static std::mutex mu;
    static std::vector<Entry> cache;
    std::promise<Ptr> mine;
    std::shared_future<Ptr> fut;
    bool make = false;
    {
        std::lock_guard lock(mu);
        for (const auto& e : cache)
            if (e.m == m && e.density == ed.density && e.feather == ed.feather) { fut = e.out; break; }
        if (!fut.valid()) {
            fut = mine.get_future().share();
            cache.insert(cache.begin(), {m, ed.density, ed.feather, fut});
            if (cache.size() > 8) cache.pop_back();
            make = true;
        }
    }
    if (make) {   // the first to ask makes it; the others wait for it
        pixel::TileMap out = *m;
        if (ed.feather > 0 && m->tile_count())
            if (auto f = select::feathered(m->without_outside(), ed.feather)) out = std::move(*f);
        if (ed.density < 100) out = select::scaled(out, static_cast<float>(ed.density / 100));
        mine.set_value(std::make_shared<const pixel::TileMap>(std::move(out)));
    }
    return fut.get();
}

// s036: DEFRINGE -- a pixel layer's colours with the old background taken
// out at its mask's edge (core/matte). Made once per (pixels, acting mask,
// width) -- tiles that ask at once wait for the one computation.
std::shared_ptr<const pixel::TileMap> defringed_pixels(const Layer& l) {
    if (l.kind != Kind::Pixel || !l.pixels || !l.mask || !l.mask_on || l.edges.defringe <= 0) return l.pixels;
    const auto hide = edged_mask(l.mask, l.edges);
    using Ptr = std::shared_ptr<const pixel::TileMap>;
    struct Entry { Ptr px, hide; double width; std::shared_future<Ptr> out; };
    static std::mutex mu;
    static std::vector<Entry> cache;
    std::promise<Ptr> mine;
    std::shared_future<Ptr> fut;
    bool make = false;
    {
        std::lock_guard lock(mu);
        for (const auto& e : cache)
            if (e.px == l.pixels && e.hide == hide && e.width == l.edges.defringe) { fut = e.out; break; }
        if (!fut.valid()) {
            fut = mine.get_future().share();
            cache.insert(cache.begin(), {l.pixels, hide, l.edges.defringe, fut});
            if (cache.size() > 6) cache.pop_back();
            make = true;
        }
    }
    if (make) {
        auto out = matte::defringed(*l.pixels, *hide, l.edges.defringe);
        mine.set_value(out ? std::make_shared<const pixel::TileMap>(std::move(*out)) : l.pixels);
    }
    return fut.get();
}

namespace {
// Each layer's own mask as it acts (its edges applied), and a defringed
// layer's colours; path layers' edges are path_masked's.
Layers edged(const Layers& ls) {
    bool any = false;
    for (const auto& [id, l] : ls) any = any || (l.kind != Kind::Path && l.mask && l.mask_on && l.edges.any());
    if (!any) return ls;
    Layers out = ls;
    for (auto& [id, l] : out)
        if (l.kind != Kind::Path && l.mask && l.mask_on) {
            l.pixels = defringed_pixels(l);   // before the mask is replaced: it reads the layer's own
            l.mask = edged_mask(l.mask, l.edges);
        }
    return out;
}

Layers path_masked(const Layers& ls, const Image* im) {
    if (!im || im->width <= 0) return ls;
    bool any = false;
    for (const auto& [id, l] : ls) any = any || (l.kind == Kind::Path && l.path_mask && l.visible && l.path);
    if (!any) return ls;
    Layers out = ls;
    const pixel::Format mf{im->format.depth, pixel::Mode::Gray, false};
    for (const auto& [id, l] : ls) {
        if (l.kind != Kind::Path || !l.path_mask || !l.visible || !l.path) continue;
        // The sibling just below: the highest place under this one.
        // Every layer below it in its group (Scott: "run the whole group").
        for (const auto& below : paths::masked_by(ls, id)) {
            Layer& t = out.at(below);
            // hide = max(own hide (when on), 1 - coverage) -- made ONCE per
            // outline and own mask, not per tile (s035, Scott: "sluggishness").
            t.mask = path_hide(l.path, t.mask && t.mask_on ? t.mask : nullptr, mf, im->width, im->height, l.edges);
            t.mask_on = true;
        }
    }
    return out;
}
}  // namespace

Layers effective(const Layers& ls_in, const Image* im) {
    const Layers ls = path_masked(edged(ls_in), im);   // s035: first, so every other look sees the masks (s036: their edges first)
    Layers out = ls;
    // s024: styled groups first (the outermost; the inner ones are done
    // inside their look): each becomes one pixel layer, its contents gone.
    if (im) {
        std::vector<Document::Id> styled;
        for (const auto& [id, l] : ls)
            if (l.kind == Kind::Group && l.styles.any()) {
                bool outer = true;
                for (const auto& [oid, o] : ls)
                    if (oid != id && o.kind == Kind::Group && o.styles.any() && inside(ls, id, oid)) { outer = false; break; }
                if (outer) styled.push_back(id);
            }
        for (const auto& gid : styled) {
            const auto flat = group_look(*im, ls, gid);
            if (!flat) continue;
            for (auto it = out.begin(); it != out.end();)
                it = inside(ls, it->first, gid) ? out.erase(it) : std::next(it);
            Layer& g = out.at(gid);
            g.kind = Kind::Pixel;   // its contents, flat: the styles drawn below, as a pixel layer's
            g.pixels = flat;
            g.filters.clear();
            if (g.blend == pixel::Blend::PassThrough) g.blend = pixel::Blend::Normal;   // a styled group is isolated
        }
    }
    // s030: FILTER LAYERS become pixel layers of what they read, filtered
    // (their mask, opacity, blend, clip kept). Without the image (or not
    // RGB) they show nothing.
    for (auto& [id, l] : out)
        if (l.kind == Kind::Filter) {
            l.pixels = im && l.visible && ls.count(id) ? filter_look(*im, ls, id) : nullptr;
            l.kind = Kind::Pixel;
            l.filters.clear();
        }
    std::vector<std::pair<Document::Id, Layer>> parts;   // s025: effects composited apart, in their own mode
    for (auto& [id, l] : out)
        if (active(l)) {
            // s025: a Drop Shadow / Outer Glow in a mode other than Normal
            // blends with what is BELOW the layer: drawn apart, as a layer of
            // its own just under this one (not for a clipped layer -- its
            // clip base would change).
            Styles main = l.styles;
            if (!l.clipped) {
                const auto split = [&](bool on, pixel::Blend mode, const char* tag, double below, auto only) {
                    if (!on || mode == pixel::Blend::Normal) return;
                    Layer fx = l;
                    fx.styles = Styles{};
                    only(fx.styles);
                    fx.fill = 0;   // the effect alone
                    Layer p = l;
                    p.pixels = look(fx);
                    p.filters.clear();
                    p.styles = Styles{};
                    p.fill = 100;
                    p.blend = mode;
                    p.clipped = false;
                    p.pos = l.pos - below;
                    parts.emplace_back(id + "\x1f" + tag, std::move(p));
                };
                split(l.styles.drop.on, l.styles.drop.mode, "drop", 0.002, [&](Styles& s) { s.drop = l.styles.drop; });
                split(l.styles.outer.on, l.styles.outer.mode, "outer", 0.001, [&](Styles& s) { s.outer = l.styles.outer; });
                if (l.styles.drop.mode != pixel::Blend::Normal) main.drop.on = false;
                if (l.styles.outer.mode != pixel::Blend::Normal) main.outer.on = false;
            }
            Layer m = l;
            m.styles = main;
            l.pixels = main.any() || !l.filters.empty() || l.filter_mask ? look(m) : l.pixels;
            l.filters.clear();
            l.fill = main.any() ? 100 : l.fill;   // s019: the fill is in the look (the content's alone)
            l.styles = Styles{};
        }
    for (auto& [pid, p] : parts) out[pid] = std::move(p);
    return out;
}

void clear_cache() {
    {
        std::lock_guard g(g_mutex);
        g_cache.clear();
    }
    {
        std::lock_guard g(g_gmutex);
        g_groups.clear();
    }
    std::lock_guard g(g_fmutex);   // s030
    g_filters.clear();
}

int partly_count() { return g_partly.load(); }

}  // namespace klimt::core::live

namespace klimt::core::layers {

namespace {
std::optional<Layer> stack_layer(const Document& d, const Id& id) {
    const auto l = d.get(id);
    if (!l || l->kind != Kind::Pixel || d.locked_all(id) || (l->locks & lock::Pixels)) return std::nullopt;
    return l;
}
std::string named(const Layer& l, const char* what) { return std::string(what) + " on “" + l.name + "”"; }
}  // namespace

namespace {
std::optional<Layer> styled_layer(const Document& d, const Id& id, const Styles& s) {
    const auto l = d.get(id);
    if (!l || (l->kind != Kind::Pixel && l->kind != Kind::Group) || d.locked_all(id) || l->styles == s) return std::nullopt;   // s024: groups too
    Layer v = *l;
    v.styles = s;
    return v;
}
}  // namespace

bool set_styles(DocJournal& j, Document& d, const Id& id, const Styles& s, const std::string& label) {
    const auto v = styled_layer(d, id, s);
    if (!v) return false;
    return j.run(label + " on “" + v->name + "”", {id}, [&] { d.put(id, *v); }, "styles:" + id);
}

bool set_styles_on(DocJournal& j, Document& d, const std::vector<Id>& ids, const Styles& s, const std::string& label) {
    std::vector<std::pair<Id, Layer>> to;
    for (const auto& id : ids)
        if (const auto v = styled_layer(d, id, s)) to.emplace_back(id, *v);
    if (to.empty()) return false;
    std::vector<Id> touched;
    for (const auto& [id, v] : to) touched.push_back(id);
    return j.run(to.size() == 1 ? label + " on “" + to.front().second.name + "”" : label, touched, [&] {
        for (const auto& [id, v] : to) d.put(id, v);
    });
}

void styles_begin(DocJournal& j, Document& d, const Id& id) {
    const auto l = d.get(id);
    if (l && (l->kind == Kind::Pixel || l->kind == Kind::Group) && !d.locked_all(id)) j.begin("Layer Style on “" + l->name + "”", {id});
}

void styles_to(DocJournal& j, Document& d, const Id& id, const Styles& s) {
    if (!j.in_gesture()) return;
    const auto v = styled_layer(d, id, s);
    if (v) j.run("Layer Style", {id}, [&] { d.put(id, *v); });
}

bool styles_end(DocJournal& j) { return j.commit(); }

bool set_filter_mask(DocJournal& j, Document& d, const Id& id, std::shared_ptr<const pixel::TileMap> m, const std::string& label) {
    auto l = stack_layer(d, id);
    if (!l || l->filters.empty()) return false;
    if (m && (!(m->format() == d.mask_format()) || m->width() != d.image().width || m->height() != d.image().height)) return false;
    if (l->filter_mask == m) return false;
    l->filter_mask = std::move(m);
    return j.run(named(*l, label.c_str()), {id}, [&] { d.put(id, *l); });
}

bool add_live(DocJournal& j, Document& d, const Id& id, const Filter& f) {
    auto l = stack_layer(d, id);
    if (!l || f.kind == FilterKind::None) return false;
    l->filters.push_back({filter::normalised(f), true});
    return j.run(std::string(filter::label(f.kind)) + " (live) on “" + l->name + "”", {id}, [&] { d.put(id, *l); });
}

bool set_live(DocJournal& j, Document& d, const Id& id, std::size_t i, const Filter& f) {
    auto l = stack_layer(d, id);
    if (!l || i >= l->filters.size() || f.kind != l->filters[i].f.kind) return false;
    const auto n = filter::normalised(f);
    if (n == l->filters[i].f) return false;
    l->filters[i].f = n;
    return j.run("Edit " + std::string(filter::label(f.kind)) + " on “" + l->name + "”", {id}, [&] { d.put(id, *l); });
}

bool toggle_live(DocJournal& j, Document& d, const Id& id, std::size_t i) {
    auto l = stack_layer(d, id);
    if (!l || i >= l->filters.size()) return false;
    l->filters[i].on = !l->filters[i].on;
    const std::string what = std::string(l->filters[i].on ? "Show " : "Hide ") + std::string(filter::label(l->filters[i].f.kind));
    return j.run(named(*l, what.c_str()), {id}, [&] { d.put(id, *l); });
}

bool remove_live(DocJournal& j, Document& d, const Id& id, std::size_t i) {
    auto l = stack_layer(d, id);
    if (!l || i >= l->filters.size()) return false;
    const std::string what = "Delete " + std::string(filter::label(l->filters[i].f.kind));
    l->filters.erase(l->filters.begin() + static_cast<std::ptrdiff_t>(i));
    if (l->filters.empty()) l->filter_mask = nullptr;   // the mask goes with the last filter
    return j.run(named(*l, what.c_str()), {id}, [&] { d.put(id, *l); });
}

bool set_live_blend(DocJournal& j, Document& d, const Id& id, std::size_t i, int opacity, pixel::Blend mode) {
    auto l = stack_layer(d, id);
    if (!l || i >= l->filters.size() || mode == pixel::Blend::PassThrough) return false;
    opacity = std::clamp(opacity, 0, 100);
    if (l->filters[i].opacity == opacity && l->filters[i].mode == mode) return false;
    l->filters[i].opacity = opacity;
    l->filters[i].mode = mode;
    const std::string what = "Blending of " + std::string(filter::label(l->filters[i].f.kind));
    return j.run(named(*l, what.c_str()), {id}, [&] { d.put(id, *l); }, "live-blend:" + id + ":" + std::to_string(i));
}

bool move_live(DocJournal& j, Document& d, const Id& id, std::size_t from, std::size_t to) {
    auto l = stack_layer(d, id);
    if (!l || from >= l->filters.size() || to >= l->filters.size() || from == to) return false;
    const auto f = l->filters[from];
    l->filters.erase(l->filters.begin() + static_cast<std::ptrdiff_t>(from));
    l->filters.insert(l->filters.begin() + static_cast<std::ptrdiff_t>(to), f);
    return j.run(named(*l, "Reorder Filters"), {id}, [&] { d.put(id, *l); });
}

bool bake_live(DocJournal& j, Document& d, const Id& id) {
    auto l = stack_layer(d, id);
    if (!l || (l->filters.empty() && !l->styles.any())) return false;
    const bool styled = l->styles.any();
    l->pixels = live::look(*l);
    l->filters.clear();
    l->filter_mask = nullptr;
    if (styled) {   // s019: the effects into the pixels -- the fill is in them already
        l->styles = Styles{};
        l->fill = 100;
    }
    return j.run(named(*l, styled ? "Rasterize Layer Style" : "Rasterize Filters"), {id}, [&] { d.put(id, *l); });
}

}  // namespace klimt::core::layers
