// klimt_bench -- how fast the compositor is (s010 m3). Headless, core only.
//
//   ./build/klimt_bench            a 4000 x 3000 RGB 8-bit document, 8 layers
//   ./build/klimt_bench 16         the same at 16-bit
//   ./build/klimt_bench size       s047: Image Size -- 7 layers of 1200 x 1600
//                                  to half (WEAKNESSES 27), the time it takes
//   ./build/klimt_bench ants       s049: the marching ants of a ragged 6000 x
//                                  4000 selection -- traced, and one frame
//                                  drawn (Cairo, as the canvas does), before
//                                  (every segment, full size) and after (the
//                                  view's level, its cells only) -- WEAKNESSES 25
//
// Composites every tile once, on ONE thread (the app spreads tiles over the
// workers), and prints milliseconds per tile. A Debug build is several times
// slower than Release; compare like with like.
#include "core/Canvas.hpp"
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/Selection.hpp"   // s049

#include <cairo.h>

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

// s049: WEAKNESSES 25 -- the ants, before and after.
namespace S = klimt::core::select;
namespace {
double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
// One frame of ants into a vw x vh surface at `zoom`, centred on (cx, cy):
// the canvas's draw_ants (white under, dashed black over). `segs` visited
// by `each`. Returns the segments stroked.
template <class Each>
std::size_t frame(cairo_t* cr, int vw, int vh, double zoom, double cx, double cy, Each&& each) {
    const auto sx = [&](int x) { return std::round(vw / 2.0 + (x - cx) * zoom) + 0.5; };
    const auto sy = [&](int y) { return std::round(vh / 2.0 + (y - cy) * zoom) + 0.5; };
    cairo_set_line_width(cr, 1);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
    std::size_t n = 0;
    each([&](const S::Seg& s) {
        const double x0 = sx(s.x0), y0 = sy(s.y0), x1 = sx(s.x1), y1 = sy(s.y1);
        if (std::max(x0, x1) < -1 || std::min(x0, x1) > vw + 1 || std::max(y0, y1) < -1 || std::min(y0, y1) > vh + 1) return;
        cairo_move_to(cr, x0, y0);
        cairo_line_to(cr, x1, y1);
        ++n;
    });
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_dash(cr, nullptr, 0, 0);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    const double dash[] = {4, 4};
    cairo_set_dash(cr, dash, 2, 3);
    cairo_stroke(cr);
    return n;
}
int bench_ants() {
    const int W = 6000, H = 4000, VW = 1600, VH = 1000;
    const P::Format mf{P::Depth::U8, P::Mode::Gray, false};
    // A Wand on a noisy photo: a disc of speckle, islands everywhere in it.
    const auto mask = P::TileMap(mf, W, H).paint({0, 0, W, H}, [&](int x, int y, const P::Px&) {
        P::Px p;
        const double dx = x - W / 2.0, dy = y - H / 2.0;
        const unsigned v = (static_cast<unsigned>(x / 3) * 73856093u) ^ (static_cast<unsigned>(y / 3) * 19349663u);
        p.c[0] = (dx * dx + dy * dy < 1800.0 * 1800.0 && ((v >> 9) % 100) < 50) ? 1.f : 0.f;
        return p;
    });
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, VW, VH);
    cairo_t* cr = cairo_create(surf);
    std::printf("ants: a ragged 6000 x 4000 selection, a %d x %d view\n", VW, VH);
    auto t0 = std::chrono::steady_clock::now();
    const auto all = S::outline(mask);
    const double trace0 = ms_since(t0);
    std::printf("  before: traced at full size     %8zu segments  %7.0f ms (once per selection)\n", all.size(), trace0);
    struct View { const char* name; double zoom; };
    for (const View v : {View{"fit (25 %)", 0.25}, View{"100 %", 1.0}, View{"12.5 %", 0.125}}) {
        const int level = S::ants_level(v.zoom);
        t0 = std::chrono::steady_clock::now();
        const auto a = S::ants(mask, level);
        const double trace = ms_since(t0);
        // Before: every segment, each tested against the view.
        double before = 0, after = 0;
        std::size_t nb = 0, na = 0, visited = 0;
        const int reps = 5;
        for (int r = 0; r < reps; ++r) {
            t0 = std::chrono::steady_clock::now();
            nb = frame(cr, VW, VH, v.zoom, W / 2.0, H / 2.0, [&](auto&& f) { for (const auto& s : all) f(s); });
            before += ms_since(t0);
            t0 = std::chrono::steady_clock::now();
            const int ix0 = static_cast<int>(std::floor(W / 2.0 - VW / 2.0 / v.zoom)) - 1, iy0 = static_cast<int>(std::floor(H / 2.0 - VH / 2.0 / v.zoom)) - 1;
            const int ix1 = static_cast<int>(std::ceil(W / 2.0 + VW / 2.0 / v.zoom)) + 1, iy1 = static_cast<int>(std::ceil(H / 2.0 + VH / 2.0 / v.zoom)) + 1;
            visited = 0;
            na = frame(cr, VW, VH, v.zoom, W / 2.0, H / 2.0, [&](auto&& f) {
                a->visit({ix0, iy0, ix1 - ix0, iy1 - iy0}, [&](const S::Seg& s) { ++visited; f(s); });
            });
            after += ms_since(t0);
        }
        std::printf("  %-11s before: %8zu stroked of %zu, %7.1f ms a frame\n", v.name, nb, all.size(), before / reps);
        std::printf("  %-11s after:  %8zu stroked (%zu visited) at 1/%d, %7.1f ms a frame; traced in %.0f ms\n", "", na, visited,
                    1 << level, after / reps, trace);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "size") return bench_size();
    if (argc > 1 && std::string(argv[1]) == "ants") return bench_ants();   // s049
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
        std::uint64_t psum = 0;   // s050: the preview's own checksum -- the threaded composite gives the same picture
        for (auto v : prev) psum = psum * 1000003u + v;
        std::printf("preview at 1/%d (%d x %d): reduce %.0f ms + composite %.0f ms = %.0f ms (%.1fx sooner); cached %.0f ms (%.0fx)  [%016llx]\n",
                    1 << k, r->image.width, r->image.height, msd(a, b), msd(b, c), msd(a, c), ms / msd(a, c), msd(c, d2), ms / msd(c, d2),
                    static_cast<unsigned long long>(psum));
    }
    // A checksum so a faster compositor can be seen to give the same picture.
    std::uint64_t sum = 0;
    core::composite_tile_argb(im, layers, 5, 5, out, w, h);
    for (auto v : out) sum = sum * 1000003u + v;
    std::printf("checksum of tile (5, 5): %016llx\n", static_cast<unsigned long long>(sum));
    return 0;
}
