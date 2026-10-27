#pragma once
// libs/pixel -- Klimt's pixels (s006). GTK-free, no dependencies. Built to
// docs/pixels.md as Scott answered it (s006: yes to all seven):
//   1  depth is the DOCUMENT's: 8-bit, 16-bit or 32-bit float;
//   2  stored at that depth, all math in float;
//   3  stored STRAIGHT (colour and alpha apart), premultiplied only to composite;
//   4  modes Gray, RGB, CMYK, Lab -- every channel stored normalised 0..1
//      (what Lab's numbers mean is the colour library's business, not this);
//   5  tiles of 256 x 256, interleaved; a map of them is SPARSE (absent =
//      fully transparent) and a tile may be UNIFORM (one pixel for 65,536);
//   6  a tile is IMMUTABLE once made; an edit makes new tiles and a new map
//      that shares every tile it didn't touch -- so the undo journal's two
//      pictures of a layer cost only the tiles a stroke changed;
//   7  (the memory budget, LZ4 and swap come later, over the same tiles).
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace klimt::pixel {

constexpr int kTile = 256;                 // a tile's side, in pixels
constexpr int kMaxChannels = 5;            // CMYK + alpha

enum class Depth : std::uint8_t { U8, U16, F32 };
enum class Mode  : std::uint8_t { Gray, RGB, CMYK, Lab };

struct Format {
    Depth depth = Depth::U8;
    Mode  mode  = Mode::RGB;
    bool  alpha = true;                    // a locked Background may have none
    int colour_channels() const;           // 1, 3, 4, 3
    int channels() const { return colour_channels() + (alpha ? 1 : 0); }
    int bytes_per_sample() const;          // 1, 2, 4
    int bytes_per_pixel() const { return channels() * bytes_per_sample(); }
    bool operator==(const Format&) const = default;
};

// One pixel in float, STRAIGHT, channels in order (alpha last). Values
// beyond channels() are ignored. 0..1 for 8 / 16-bit; float may exceed 1.
struct Px {
    std::array<float, kMaxChannels> c{};
    bool operator==(const Px&) const = default;
};
Px rgba(float r, float g, float b, float a = 1.f);   // convenience for RGB
Px transparent();                                     // all zero

class TileWriter;

// 256 x 256 pixels of one Format. Immutable: made by TileWriter::freeze or
// Tile::uniform, then only read. Shared by pointer between maps and steps.
class Tile {
public:
    static std::shared_ptr<const Tile> uniform(const Format& f, const Px& p);

    const Format& format() const { return m_format; }
    bool is_uniform() const { return m_uniform; }
    Px get(int x, int y) const;            // 0 <= x, y < kTile
    std::size_t bytes() const { return m_data.size(); }   // memory it holds
    // For compositing: the whole tile as PREMULTIPLIED float, channels()
    // floats per pixel, row-major. `out` gets kTile * kTile * channels().
    void premultiplied(std::vector<float>& out) const;

private:
    friend class TileWriter;
    Tile() = default;
    Format                    m_format;
    bool                      m_uniform = false;
    std::vector<std::uint8_t> m_data;      // uniform: one pixel; else kTile*kTile
};

// The only way to make a non-uniform tile: start from a tile (or a fill),
// set pixels, freeze. A writer that never changed anything freezes back to
// the tile it came from (no copy).
class TileWriter {
public:
    TileWriter(const Format& f, const Px& fill);
    explicit TileWriter(std::shared_ptr<const Tile> from);
    void set(int x, int y, const Px& p);
    Px get(int x, int y) const;
    // From PREMULTIPLIED float (what a composite or a filter produces).
    void set_premultiplied(const std::vector<float>& in);
    std::shared_ptr<const Tile> freeze();

private:
    void expand();                         // a uniform source becomes full data
    Format                      m_format;
    std::shared_ptr<const Tile> m_from;    // unchanged? hand it back
    std::vector<std::uint8_t>   m_data;    // full kTile*kTile once expanded
    Px                          m_fill;    // while still uniform
    bool                        m_expanded = false;
    bool                        m_changed = false;
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool empty() const { return w <= 0 || h <= 0; }
    Rect intersect(const Rect& o) const;
    bool operator==(const Rect&) const = default;
};

// A layer's pixels: width x height of one Format, as a grid of tiles.
// IMMUTABLE and cheap to copy (the grid is shared until an edit). Absent
// tiles are fully transparent. Every edit returns a NEW map that shares
// each tile it didn't touch.
class TileMap {
public:
    using TileRef = std::shared_ptr<const Tile>;
    TileMap() = default;
    TileMap(const Format& f, int width, int height);

    const Format& format() const { return m_format; }
    int width() const { return m_w; }
    int height() const { return m_h; }
    int cols() const { return (m_w + kTile - 1) / kTile; }
    int rows() const { return (m_h + kTile - 1) / kTile; }

    TileRef tile(int col, int row) const;  // nullptr: transparent
    Px get(int x, int y) const;            // outside or absent: transparent
    std::size_t tile_count() const;        // tiles actually held
    std::size_t bytes() const;             // memory the held tiles use

    TileMap with_tile(int col, int row, TileRef t) const;
    // Fill a rectangle (clipped to the map). A tile wholly covered becomes a
    // uniform tile; a partly covered one is copied and written; the rest are
    // shared. Filling with transparent over a whole tile drops it.
    TileMap fill(const Rect& r, const Px& p) const;

    // The general edit: `f(x, y, old)` gives each pixel of the rectangle its
    // new value (clipped to the map). Copy-on-write per tile; a tile the
    // function leaves unchanged is shared, not copied. Circles, alpha-locked
    // fills and (later) brush dabs are this.
    TileMap paint(const Rect& r, const std::function<Px(int, int, const Px&)>& f) const;

    // Same grid object? (Two maps from one edit chain share untouched tiles;
    // this asks whether a given tile is the very same tile.)
    bool same_tile(const TileMap& other, int col, int row) const;

private:
    Format m_format;
    int    m_w = 0, m_h = 0;
    std::shared_ptr<const std::vector<TileRef>> m_grid;   // cols * rows
};

}  // namespace klimt::pixel
