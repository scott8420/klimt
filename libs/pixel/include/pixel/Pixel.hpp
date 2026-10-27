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
#include <map>
#include <memory>
#include <utility>
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
    // s007 -- a tile from STORED bytes (kTile * kTile * bytes_per_pixel, the
    // format's depth, native-endian, straight): what a file reader cuts from a
    // band of rows. Pixels all the same collapse to a uniform tile; all-zero
    // bytes on a format WITH alpha are fully transparent and come back
    // nullptr (an absent tile). A wrong size comes back nullptr too.
    static std::shared_ptr<const Tile> from_bytes(const Format& f, std::vector<std::uint8_t> data);
    // The stored bytes: one pixel when uniform, else the whole tile.
    const std::uint8_t* data() const { return m_data.data(); }

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
//
// s010 -- PIXELS BEYOND THE IMAGE. A layer may hold pixels outside the
// width x height rectangle (what a move pushed off the canvas; what a crop
// kept). They live in OUTSIDE tiles, sparse, keyed by the cell (col, row) on
// the same 256 grid anchored at the image's (0, 0) -- col / row may be
// negative, or the image's own edge cells (for the part of an edge cell past
// the image). An outside tile's pixels INSIDE the image mean nothing; a grid
// tile's pixels outside it mean nothing. Everything that works on the image
// (the compositor, painting, selections) reads the grid as before; edits
// made with fill / paint / with_tile / with_grid keep the outside tiles.
class TileMap {
public:
    using TileRef = std::shared_ptr<const Tile>;
    using Cell    = std::pair<int, int>;               // (col, row)
    using Outside = std::map<Cell, TileRef>;
    TileMap() = default;
    TileMap(const Format& f, int width, int height);

    const Format& format() const { return m_format; }
    int width() const { return m_w; }
    int height() const { return m_h; }
    int cols() const { return (m_w + kTile - 1) / kTile; }
    int rows() const { return (m_h + kTile - 1) / kTile; }

    TileRef tile(int col, int row) const;  // nullptr: transparent
    // Any pixel: inside the image from the grid, outside it from the
    // outside tiles (s010). Absent: transparent.
    Px get(int x, int y) const;
    std::size_t tile_count() const;        // tiles actually held (grid + outside)
    std::size_t bytes() const;             // memory the held tiles use

    // s010 -- the pixels beyond the image.
    TileRef outside(int col, int row) const;           // nullptr: none there
    const Outside& outside_tiles() const;               // every outside tile held
    bool has_outside() const { return m_over && !m_over->empty(); }
    TileMap with_outside(Outside o) const;              // the same grid, these outside tiles (nullptrs dropped)
    TileMap without_outside() const { return with_outside({}); }
    // The same map (size, format, outside tiles) with its grid replaced --
    // what an edit that rebuilt the image's tiles lands through.
    TileMap with_grid(std::vector<TileRef> grid) const;

    TileMap with_tile(int col, int row, TileRef t) const;
    // s007 -- a map from its whole grid at once (cols * rows, row-major; a
    // reader's or a loader's result). A grid of the wrong size is padded /
    // cut; a tile of another format is dropped.
    static TileMap from_tiles(const Format& f, int width, int height, std::vector<TileRef> grid);
    // Row y's STORED bytes (width * bytes_per_pixel; absent tiles as zero):
    // what a file writer reads. y outside the map: all zero.
    void row_bytes(int y, std::uint8_t* out) const;
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
    std::shared_ptr<const Outside>              m_over;   // s010: nullptr = none
};

// Floor division for cells: the cell holding pixel coordinate v (v may be negative).
inline int cell_of(int v) { return v >= 0 ? v / kTile : -((-v + kTile - 1) / kTile); }

}  // namespace klimt::pixel
