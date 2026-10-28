// core::live -- live filters (s018). See core/Live.hpp.
#include "core/Live.hpp"
#include "core/StylesRender.hpp"   // s019

#include <algorithm>
#include <future>
#include <mutex>

namespace klimt::core::live {

namespace {
struct Entry {
    std::shared_ptr<const pixel::TileMap> src;   // held: the pointer stays unique while cached
    std::shared_ptr<const pixel::TileMap> mask;  // m3: the filter mask (nullptr: none)
    std::vector<LiveFilter> stack;
    Styles styles;   // s019 (and the fill, which styles keep off their effects)
    int fill = 100;
    std::shared_future<std::shared_ptr<const pixel::TileMap>> result;
    std::uint64_t used = 0;
};
std::mutex g_mutex;
std::vector<Entry> g_cache;
std::uint64_t g_clock = 0;
constexpr std::size_t kKeep = 8;

std::vector<LiveFilter> on_only(const std::vector<LiveFilter>& s) {
    std::vector<LiveFilter> v;
    for (const auto& f : s) if (f.on && f.f.kind != FilterKind::None) v.push_back(f);
    return v;
}
}  // namespace

std::optional<pixel::TileMap> run(const pixel::TileMap& src, const std::vector<LiveFilter>& stack, const pixel::Rect* only,
                                  const std::function<bool()>& cancelled) {
    const auto on = on_only(stack);
    pixel::TileMap cur = src;
    for (std::size_t i = 0; i < on.size(); ++i) {
        std::optional<pixel::Rect> r;
        if (only) {   // this stage must be right wherever the later stages read
            int later = 0;
            for (std::size_t k = i + 1; k < on.size(); ++k) later += filter::reach(on[k].f);
            r = pixel::Rect{only->x - later, only->y - later, only->w + 2 * later, only->h + 2 * later};
        }
        auto next = filter::filtered(cur, on[i].f, nullptr, false, r ? &*r : nullptr, cancelled);
        if (!next) return std::nullopt;
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
    for (const auto& [id, l] : ls) if (active(l)) return true;
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
    std::shared_future<std::shared_ptr<const pixel::TileMap>> fut;
    std::promise<std::shared_ptr<const pixel::TileMap>> mine;
    bool compute = false;
    {
        std::lock_guard g(g_mutex);
        for (auto& e : g_cache)
            if (e.src == l.pixels && e.mask == l.filter_mask && e.stack == stack && e.styles == l.styles &&
                (!l.styles.any() || e.fill == l.fill)) { e.used = ++g_clock; fut = e.result; break; }
        if (!fut.valid()) {
            if (g_cache.size() >= kKeep) {   // the least used, finished entry goes
                auto victim = g_cache.end();
                for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
                    if (it->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
                        (victim == g_cache.end() || it->used < victim->used)) victim = it;
                if (victim != g_cache.end()) g_cache.erase(victim);
            }
            fut = mine.get_future().share();
            g_cache.push_back({l.pixels, l.filter_mask, stack, l.styles, l.fill, fut, ++g_clock});
            compute = true;
        }
    }
    if (compute) {
        auto out = run(*l.pixels, stack);
        if (out && l.filter_mask) out = masked(*l.pixels, *out, *l.filter_mask);
        if (out && l.styles.any()) out = styles::drawn(*out, l.styles, l.fill);   // s019: the effects, the fill on the content
        mine.set_value(out ? std::make_shared<const pixel::TileMap>(std::move(*out)) : l.pixels);
    }
    return fut.get();
}

Layers effective(const Layers& ls) {
    Layers out = ls;
    for (auto& [id, l] : out)
        if (active(l)) {
            l.pixels = look(l);
            l.filters.clear();
            if (l.styles.any()) l.fill = 100;   // s019: the fill is in the look (the content's alone)
            l.styles = Styles{};
        }
    return out;
}

void clear_cache() {
    std::lock_guard g(g_mutex);
    g_cache.clear();
}

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
    if (!l || l->kind != Kind::Pixel || d.locked_all(id) || l->styles == s) return std::nullopt;
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
    if (l && l->kind == Kind::Pixel && !d.locked_all(id)) j.begin("Layer Style on “" + l->name + "”", {id});
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
