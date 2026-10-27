#include "pixel/Pixel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace klimt::pixel {

// ── Format ──────────────────────────────────────────────────────────────────
int Format::colour_channels() const {
    switch (mode) {
        case Mode::Gray: return 1;
        case Mode::RGB:  return 3;
        case Mode::CMYK: return 4;
        case Mode::Lab:  return 3;
    }
    return 3;
}

int Format::bytes_per_sample() const {
    switch (depth) {
        case Depth::U8:  return 1;
        case Depth::U16: return 2;
        case Depth::F32: return 4;
    }
    return 1;
}

Px rgba(float r, float g, float b, float a) {
    Px p;
    p.c = {r, g, b, a, 0.f};
    return p;
}

Px transparent() { return Px{}; }

// ── encoding one pixel at a format ──────────────────────────────────────────
namespace {
void encode(const Format& f, const Px& p, std::uint8_t* out) {
    const int n = f.channels();
    for (int i = 0; i < n; ++i) {
        const float v = p.c[static_cast<std::size_t>(i)];
        switch (f.depth) {
            case Depth::U8: {
                out[i] = static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
                break;
            }
            case Depth::U16: {
                const auto s = static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.f, 1.f) * 65535.f));
                std::memcpy(out + 2 * i, &s, 2);
                break;
            }
            case Depth::F32:
                std::memcpy(out + 4 * i, &v, 4);
                break;
        }
    }
}

Px decode(const Format& f, const std::uint8_t* in) {
    Px p;
    const int n = f.channels();
    for (int i = 0; i < n; ++i) {
        float v = 0;
        switch (f.depth) {
            case Depth::U8:  v = in[i] / 255.f; break;
            case Depth::U16: { std::uint16_t s; std::memcpy(&s, in + 2 * i, 2); v = s / 65535.f; break; }
            case Depth::F32: std::memcpy(&v, in + 4 * i, 4); break;
        }
        p.c[static_cast<std::size_t>(i)] = v;
    }
    return p;
}

std::size_t full_bytes(const Format& f) {
    return static_cast<std::size_t>(kTile) * kTile * static_cast<std::size_t>(f.bytes_per_pixel());
}
}  // namespace

// ── Tile ────────────────────────────────────────────────────────────────────
std::shared_ptr<const Tile> Tile::uniform(const Format& f, const Px& p) {
    auto t = std::shared_ptr<Tile>(new Tile());
    t->m_format = f;
    t->m_uniform = true;
    t->m_data.resize(static_cast<std::size_t>(f.bytes_per_pixel()));
    encode(f, p, t->m_data.data());
    return t;
}

Px Tile::get(int x, int y) const {
    if (m_uniform) return decode(m_format, m_data.data());
    const auto bpp = static_cast<std::size_t>(m_format.bytes_per_pixel());
    return decode(m_format, m_data.data() + (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * bpp);
}

void Tile::premultiplied(std::vector<float>& out) const {
    const int n = m_format.channels();
    out.resize(static_cast<std::size_t>(kTile) * kTile * static_cast<std::size_t>(n));
    const auto put = [&](std::size_t i, const Px& p) {
        const float a = m_format.alpha ? p.c[static_cast<std::size_t>(n - 1)] : 1.f;
        const int colours = m_format.colour_channels();
        for (int k = 0; k < colours; ++k) out[i * static_cast<std::size_t>(n) + static_cast<std::size_t>(k)] = p.c[static_cast<std::size_t>(k)] * a;
        if (m_format.alpha) out[i * static_cast<std::size_t>(n) + static_cast<std::size_t>(n - 1)] = a;
    };
    if (m_uniform) {
        const Px p = get(0, 0);
        for (std::size_t i = 0; i < static_cast<std::size_t>(kTile) * kTile; ++i) put(i, p);
        return;
    }
    for (int y = 0; y < kTile; ++y)
        for (int x = 0; x < kTile; ++x)
            put(static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x), get(x, y));
}

// ── TileWriter ──────────────────────────────────────────────────────────────
TileWriter::TileWriter(const Format& f, const Px& fill) : m_format(f), m_fill(fill) {}

TileWriter::TileWriter(std::shared_ptr<const Tile> from) : m_format(from->format()), m_from(from) {
    if (from->is_uniform()) m_fill = from->get(0, 0);
    else { m_data = from->m_data; m_expanded = true; }
}

void TileWriter::expand() {
    if (m_expanded) return;
    const auto bpp = static_cast<std::size_t>(m_format.bytes_per_pixel());
    m_data.resize(full_bytes(m_format));
    std::vector<std::uint8_t> one(bpp);
    encode(m_format, m_fill, one.data());
    for (std::size_t i = 0; i < static_cast<std::size_t>(kTile) * kTile; ++i)
        std::memcpy(m_data.data() + i * bpp, one.data(), bpp);
    m_expanded = true;
}

void TileWriter::set(int x, int y, const Px& p) {
    // Only a real difference (as STORED) counts: writing a pixel its own
    // value is no change, so a fill that changes nothing keeps the tile.
    const auto bpp = static_cast<std::size_t>(m_format.bytes_per_pixel());
    std::array<std::uint8_t, kMaxChannels * 4> enc{};
    encode(m_format, p, enc.data());
    if (!m_expanded) {
        std::array<std::uint8_t, kMaxChannels * 4> cur{};
        encode(m_format, m_fill, cur.data());
        if (std::memcmp(enc.data(), cur.data(), bpp) == 0) return;
    }
    expand();
    std::uint8_t* at = m_data.data() + (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * bpp;
    if (std::memcmp(at, enc.data(), bpp) == 0) return;
    std::memcpy(at, enc.data(), bpp);
    m_changed = true;
}

Px TileWriter::get(int x, int y) const {
    if (!m_expanded) return decode(m_format, [&] {
        static thread_local std::vector<std::uint8_t> one;
        one.resize(static_cast<std::size_t>(m_format.bytes_per_pixel()));
        encode(m_format, m_fill, one.data());
        return one.data();
    }());
    const auto bpp = static_cast<std::size_t>(m_format.bytes_per_pixel());
    return decode(m_format, m_data.data() + (static_cast<std::size_t>(y) * kTile + static_cast<std::size_t>(x)) * bpp);
}

void TileWriter::set_premultiplied(const std::vector<float>& in) {
    expand();
    const int n = m_format.channels();
    const int colours = m_format.colour_channels();
    const auto bpp = static_cast<std::size_t>(m_format.bytes_per_pixel());
    for (std::size_t i = 0; i < static_cast<std::size_t>(kTile) * kTile; ++i) {
        Px p;
        const float* s = in.data() + i * static_cast<std::size_t>(n);
        const float a = m_format.alpha ? s[n - 1] : 1.f;
        for (int k = 0; k < colours; ++k) p.c[static_cast<std::size_t>(k)] = a > 0.f ? s[k] / a : 0.f;
        if (m_format.alpha) p.c[static_cast<std::size_t>(n - 1)] = a;
        encode(m_format, p, m_data.data() + i * bpp);
    }
    m_changed = true;
}

std::shared_ptr<const Tile> TileWriter::freeze() {
    if (!m_changed && m_from) return m_from;
    if (!m_expanded) return Tile::uniform(m_format, m_fill);
    auto t = std::shared_ptr<Tile>(new Tile());
    t->m_format = m_format;
    t->m_data = std::move(m_data);
    m_data.clear();
    m_expanded = false;
    return t;
}

// ── Rect ────────────────────────────────────────────────────────────────────
Rect Rect::intersect(const Rect& o) const {
    const int x0 = std::max(x, o.x), y0 = std::max(y, o.y);
    const int x1 = std::min(x + w, o.x + o.w), y1 = std::min(y + h, o.y + o.h);
    return {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
}

// ── TileMap ─────────────────────────────────────────────────────────────────
TileMap::TileMap(const Format& f, int width, int height)
    : m_format(f), m_w(std::max(0, width)), m_h(std::max(0, height)),
      m_grid(std::make_shared<const std::vector<TileRef>>(
          static_cast<std::size_t>(cols()) * static_cast<std::size_t>(rows()))) {}

TileMap::TileRef TileMap::tile(int col, int row) const {
    if (!m_grid || col < 0 || row < 0 || col >= cols() || row >= rows()) return nullptr;
    return (*m_grid)[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols()) + static_cast<std::size_t>(col)];
}

Px TileMap::get(int x, int y) const {
    if (x < 0 || y < 0 || x >= m_w || y >= m_h) return transparent();
    const auto t = tile(x / kTile, y / kTile);
    return t ? t->get(x % kTile, y % kTile) : transparent();
}

std::size_t TileMap::tile_count() const {
    if (!m_grid) return 0;
    return static_cast<std::size_t>(std::count_if(m_grid->begin(), m_grid->end(), [](const TileRef& t) { return t != nullptr; }));
}

std::size_t TileMap::bytes() const {
    std::size_t n = 0;
    if (m_grid) for (const auto& t : *m_grid) if (t) n += t->bytes();
    return n;
}

TileMap TileMap::with_tile(int col, int row, TileRef t) const {
    if (!m_grid || col < 0 || row < 0 || col >= cols() || row >= rows()) return *this;
    TileMap out = *this;
    auto grid = std::make_shared<std::vector<TileRef>>(*m_grid);   // pointers only
    (*grid)[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols()) + static_cast<std::size_t>(col)] = std::move(t);
    out.m_grid = std::move(grid);
    return out;
}

TileMap TileMap::fill(const Rect& r0, const Px& p) const {
    const Rect r = r0.intersect({0, 0, m_w, m_h});
    if (r.empty() || !m_grid) return *this;
    TileMap out = *this;
    auto grid = std::make_shared<std::vector<TileRef>>(*m_grid);
    const bool clear = m_format.alpha && p.c[static_cast<std::size_t>(m_format.channels() - 1)] <= 0.f;
    for (int row = r.y / kTile; row <= (r.y + r.h - 1) / kTile; ++row)
        for (int col = r.x / kTile; col <= (r.x + r.w - 1) / kTile; ++col) {
            const Rect cell{col * kTile, row * kTile, kTile, kTile};
            // A tile's part beyond the image doesn't exist: a fill covering
            // the part that does (an edge tile) makes it uniform too.
            const Rect live = cell.intersect({0, 0, m_w, m_h});
            const Rect hit = r.intersect(cell);
            auto& slot = (*grid)[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols()) + static_cast<std::size_t>(col)];
            if (hit == live) {                         // wholly covered
                if (clear) { slot = nullptr; continue; }
                auto u = Tile::uniform(m_format, p);
                if (!(slot && slot->is_uniform() && slot->get(0, 0) == u->get(0, 0))) slot = u;
                continue;
            }
            TileWriter w = slot ? TileWriter(slot) : TileWriter(m_format, transparent());
            for (int y = hit.y; y < hit.y + hit.h; ++y)
                for (int x = hit.x; x < hit.x + hit.w; ++x) w.set(x - cell.x, y - cell.y, p);
            slot = w.freeze();
        }
    out.m_grid = std::move(grid);
    return out;
}

TileMap TileMap::paint(const Rect& r0, const std::function<Px(int, int, const Px&)>& f) const {
    const Rect r = r0.intersect({0, 0, m_w, m_h});
    if (r.empty() || !m_grid) return *this;
    TileMap out = *this;
    auto grid = std::make_shared<std::vector<TileRef>>(*m_grid);
    bool any = false;
    for (int row = r.y / kTile; row <= (r.y + r.h - 1) / kTile; ++row)
        for (int col = r.x / kTile; col <= (r.x + r.w - 1) / kTile; ++col) {
            const Rect cell{col * kTile, row * kTile, kTile, kTile};
            const Rect hit = r.intersect(cell);
            auto& slot = (*grid)[static_cast<std::size_t>(row) * static_cast<std::size_t>(cols()) + static_cast<std::size_t>(col)];
            TileWriter w = slot ? TileWriter(slot) : TileWriter(m_format, transparent());
            bool changed = false;
            for (int y = hit.y; y < hit.y + hit.h; ++y)
                for (int x = hit.x; x < hit.x + hit.w; ++x) {
                    const Px old = w.get(x - cell.x, y - cell.y);
                    const Px now = f(x, y, old);
                    if (!(now == old)) { w.set(x - cell.x, y - cell.y, now); changed = true; }
                }
            if (!changed) continue;            // shared, not copied
            slot = w.freeze();
            any = true;
        }
    if (!any) return *this;
    out.m_grid = std::move(grid);
    return out;
}

bool TileMap::same_tile(const TileMap& other, int col, int row) const {
    return tile(col, row) == other.tile(col, row);
}

}  // namespace klimt::pixel
