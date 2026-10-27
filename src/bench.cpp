// klimt_bench -- how fast the compositor is (s010 m3). Headless, core only.
//
//   ./build/klimt_bench            a 4000 x 3000 RGB 8-bit document, 8 layers
//   ./build/klimt_bench 16         the same at 16-bit
//
// Composites every tile once, on ONE thread (the app spreads tiles over the
// workers), and prints milliseconds per tile. A Debug build is several times
// slower than Release; compare like with like.
#include "core/Composite.hpp"
#include "core/Document.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace core = klimt::core;
namespace P = klimt::pixel;

int main(int argc, char** argv) {
    const int depth = argc > 1 ? std::atoi(argv[1]) : 8;
    const P::Format f{depth == 16 ? P::Depth::U16 : depth == 32 ? P::Depth::F32 : P::Depth::U8, P::Mode::RGB, true};
    const int W = 4000, H = 3000;
    core::Image im{.width = W, .height = H, .format = f};
    core::Layers layers;
    const auto add = [&](const std::string& id, double pos, P::TileMap m, P::Blend b = P::Blend::Normal, int op = 100) {
        core::Layer l;
        l.name = id;
        l.pos = pos;
        l.blend = b;
        l.opacity = op;
        l.pixels = std::make_shared<const P::TileMap>(std::move(m));
        layers[id] = l;
    };
    const P::TileMap blank(f, W, H);
    // A flat background, a full-frame gradient (no two pixels alike), two
    // soft discs, a half-transparent band, a Multiply wash, a Screen glow and
    // a small sticker -- a mix of uniform, sparse and dense tiles.
    add("bg", 1, blank.fill({0, 0, W, H}, P::rgba(0.9f, 0.88f, 0.85f)));
    add("grad", 2, blank.paint({0, 0, W, H}, [&](int x, int y, const P::Px&) {
        return P::rgba(static_cast<float>(x) / W, static_cast<float>(y) / H, 0.5f, 1);
    }), P::Blend::Normal, 60);
    const auto disc = [&](double cx, double cy, double r, P::Px c) {
        return blank.paint({static_cast<int>(cx - r), static_cast<int>(cy - r), static_cast<int>(2 * r), static_cast<int>(2 * r)},
                           [=](int x, int y, const P::Px& old) {
                               const double d = std::hypot(x - cx, y - cy);
                               if (d > r) return old;
                               P::Px p = c;
                               p.c[3] = static_cast<float>(std::clamp((r - d) / 40.0, 0.0, 1.0));
                               return p;
                           });
    };
    add("disc1", 3, disc(1200, 1100, 700, P::rgba(0.8f, 0.6f, 0.2f)));
    add("disc2", 4, disc(2900, 1900, 900, P::rgba(0.1f, 0.3f, 0.5f)));
    add("band", 5, blank.fill({0, 1300, W, 400}, P::rgba(1, 1, 1, 0.4f)));
    add("wash", 6, blank.fill({0, 0, W, H}, P::rgba(0.7f, 0.8f, 1.f)), P::Blend::Multiply, 50);
    add("glow", 7, disc(2000, 1500, 1200, P::rgba(1, 0.9f, 0.6f)), P::Blend::Screen, 40);
    add("sticker", 8, blank.fill({300, 300, 200, 200}, P::rgba(1, 0, 0)));

    const int cols = (W + P::kTile - 1) / P::kTile, rows = (H + P::kTile - 1) / P::kTile;
    std::vector<std::uint32_t> out;
    int w = 0, h = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) core::composite_tile_argb(im, layers, c, r, out, w, h);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("%d x %d, %d-bit RGBA, 8 layers, %d tiles: %.0f ms -- %.2f ms a tile (one thread)\n", W, H, depth,
                cols * rows, ms, ms / (cols * rows));
    // A checksum so a faster compositor can be seen to give the same picture.
    std::uint64_t sum = 0;
    core::composite_tile_argb(im, layers, 5, 5, out, w, h);
    for (auto v : out) sum = sum * 1000003u + v;
    std::printf("checksum of tile (5, 5): %016llx\n", static_cast<unsigned long long>(sum));
    return 0;
}
