// klimt_bench -- how fast the compositor is (s010 m3). Headless, core only.
//
//   ./build/klimt_bench            a 4000 x 3000 RGB 8-bit document, 8 layers
//   ./build/klimt_bench 16         the same at 16-bit
//   ./build/klimt_bench size       s047: Image Size -- 7 layers of 1200 x 1600
//                                  to half (WEAKNESSES 27), the time it takes
//
// Composites every tile once, on ONE thread (the app spreads tiles over the
// workers), and prints milliseconds per tile. A Debug build is several times
// slower than Release; compare like with like.
#include "core/Canvas.hpp"
#include "core/Composite.hpp"
#include "core/Document.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace core = klimt::core;
namespace P = klimt::pixel;

namespace {
// s047: Image Size over several layers -- WEAKNESSES 27's case.
int bench_size() {
    const P::Format f{P::Depth::U8, P::Mode::RGB, true};
    const int W = 1200, H = 1600;
    core::canvas::Before b;
    b.image = std::make_shared<const core::Image>(core::Image{.width = W, .height = H, .format = f});
    const P::TileMap blank(f, W, H);
    for (int k = 0; k < 7; ++k)   // dense: no two pixels alike
        b.pixels["l" + std::to_string(k)] = std::make_shared<const P::TileMap>(blank.paint({0, 0, W, H}, [k](int x, int y, const P::Px&) {
            return P::rgba(static_cast<float>((x * (k + 3)) % 255) / 255.f, static_cast<float>((y * (k + 5)) % 255) / 255.f, 0.5f, 1);
        }));
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = core::canvas::size_plan(b, W / 2, H / 2, 72, core::canvas::Filter::Smooth);
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("Image Size: 7 layers %d x %d -> %d x %d: %.0f ms (%u cores)\n", W, H, W / 2, H / 2,
                std::chrono::duration<double, std::milli>(t1 - t0).count(), std::thread::hardware_concurrency());
    std::uint64_t sum = 0;   // the same picture, however it was spread
    if (r) for (const auto& [id, m] : r->pixels) for (int y = 0; y < m->height(); y += 37) for (int x = 0; x < m->width(); x += 41) {
        const auto px = m->get(x, y);
        for (float c : px.c) sum = sum * 1000003u + static_cast<std::uint64_t>(c * 65535.f);
    }
    std::printf("checksum: %016llx\n", static_cast<unsigned long long>(sum));
    return r ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "size") return bench_size();
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
    // s046: THE VIEW PYRAMID -- what a zoomed-out view waits for before the
    // first whole picture: every tile at full size (above), or a preview at
    // 1/2^k -- the layers reduced (once: a second run hits the cache) and the
    // small document composited.
    for (int k = 1; k <= 3; ++k) {
        core::ReduceCache cache;
        std::vector<std::uint32_t> prev;
        const auto a = std::chrono::steady_clock::now();
        auto r = core::reduced(im, layers, k, &cache);
        const auto b = std::chrono::steady_clock::now();
        core::composite_reduced_argb(*r, prev);
        const auto c = std::chrono::steady_clock::now();
        auto r2 = core::reduced(im, layers, k, &cache);   // cached
        core::composite_reduced_argb(*r2, prev);
        const auto d2 = std::chrono::steady_clock::now();
        const auto msd = [](auto x, auto y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
        std::printf("preview at 1/%d (%d x %d): reduce %.0f ms + composite %.0f ms = %.0f ms (%.1fx sooner); cached %.0f ms (%.0fx)\n",
                    1 << k, r->image.width, r->image.height, msd(a, b), msd(b, c), msd(a, c), ms / msd(a, c), msd(c, d2), ms / msd(c, d2));
    }
    // A checksum so a faster compositor can be seen to give the same picture.
    std::uint64_t sum = 0;
    core::composite_tile_argb(im, layers, 5, 5, out, w, h);
    for (auto v : out) sum = sum * 1000003u + v;
    std::printf("checksum of tile (5, 5): %016llx\n", static_cast<unsigned long long>(sum));
    return 0;
}
