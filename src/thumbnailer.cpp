// klimt-thumbnailer (s049) -- the file manager's preview of a .klimt.
//
//   klimt-thumbnailer -s SIZE INPUT OUTPUT
//
// GNOME's thumbnail factory runs it (data/klimt.thumbnailer) for files of
// type image/x-klimt (data/io.github.scott8420.Klimt.mime.xml). It reads the
// preview a .klimt carries since s049 -- Thumbnails/thumbnail.png (256 px),
// or mergedimage.png (up to 1024) when a bigger thumbnail is asked for --
// fits it to SIZE (never larger than it is) and writes a PNG. A file saved
// before s049 has no preview: exit 1, and the file manager shows its icon.
//
// No GTK, no compositor: libzip and libs/formats only, so it starts fast
// and runs inside the thumbnailer's sandbox.
#include "formats/Formats.hpp"
#include "pixel/Pixel.hpp"

#include <zip.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace P  = klimt::pixel;
namespace Fm = klimt::formats;

namespace {
bool read_entry(zip_t* z, const char* name, std::vector<std::uint8_t>& out) {
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat(z, name, 0, &st) != 0 || !(st.valid & ZIP_STAT_SIZE) || st.size > (64u << 20)) return false;
    out.resize(static_cast<std::size_t>(st.size));
    zip_file_t* f = zip_fopen(z, name, 0);
    if (!f) return false;
    const zip_int64_t got = st.size ? zip_fread(f, out.data(), st.size) : 0;
    zip_fclose(f);
    return got == static_cast<zip_int64_t>(st.size);
}

// Fit within side x side: each output pixel the premultiplied mean of the
// box it covers. 8-bit RGBA out.
P::TileMap fit(const P::TileMap& src, int side) {
    const int W = src.width(), H = src.height();
    const double s = std::min(1.0, static_cast<double>(side) / std::max(W, H));
    const int tw = std::max(1, static_cast<int>(std::lround(W * s))), th = std::max(1, static_cast<int>(std::lround(H * s)));
    const P::Format out{P::Depth::U8, P::Mode::RGB, true};
    const bool alpha = src.format().alpha, gray = src.format().mode == P::Mode::Gray;
    const auto ach = static_cast<std::size_t>(src.format().channels() - 1);
    const double fx = static_cast<double>(W) / tw, fy = static_cast<double>(H) / th;
    return P::TileMap(out, tw, th).paint({0, 0, tw, th}, [&](int x, int y, const P::Px&) {
        const int x0 = static_cast<int>(x * fx), x1 = std::max(x0 + 1, std::min(W, static_cast<int>(std::ceil((x + 1) * fx))));
        const int y0 = static_cast<int>(y * fy), y1 = std::max(y0 + 1, std::min(H, static_cast<int>(std::ceil((y + 1) * fy))));
        double r = 0, g = 0, b = 0, a = 0;
        int n = 0;
        for (int yy = y0; yy < y1; ++yy)
            for (int xx = x0; xx < x1; ++xx) {
                const P::Px p = src.get(xx, yy);
                const double pa = alpha ? p.c[ach] : 1.0;
                r += p.c[0] * pa;
                g += (gray ? p.c[0] : p.c[1]) * pa;
                b += (gray ? p.c[0] : p.c[2]) * pa;
                a += pa;
                ++n;
            }
        if (a <= 0) return P::transparent();
        return P::rgba(static_cast<float>(r / a), static_cast<float>(g / a), static_cast<float>(b / a), static_cast<float>(a / n));
    });
}

int usage() {
    std::fprintf(stderr, "usage: klimt-thumbnailer -s SIZE INPUT.klimt OUTPUT.png\n");
    return 2;
}
}  // namespace

int main(int argc, char** argv) {
    int size = 256;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-s" && i + 1 < argc) size = std::atoi(argv[++i]);
        else args.push_back(a);
    }
    if (args.size() != 2 || size < 1 || size > 4096) return usage();
    const std::string& in = args[0];
    const std::string& out = args[1];

    int code = 0;
    zip_t* z = zip_open(in.c_str(), ZIP_RDONLY, &code);
    if (!z) { std::fprintf(stderr, "klimt-thumbnailer: %s: not a readable .klimt\n", in.c_str()); return 1; }
    std::vector<std::uint8_t> png;
    // The small one when it is enough; the bigger one for large thumbnails.
    const char* first = size > 256 ? "mergedimage.png" : "Thumbnails/thumbnail.png";
    const char* second = size > 256 ? "Thumbnails/thumbnail.png" : "mergedimage.png";
    const bool got = read_entry(z, first, png) || read_entry(z, second, png);
    zip_discard(z);
    if (!got) { std::fprintf(stderr, "klimt-thumbnailer: %s: no preview (saved before s049)\n", in.c_str()); return 1; }

    std::string err;
    auto r = Fm::read_png_bytes(png.data(), png.size(), err);
    if (!r) { std::fprintf(stderr, "klimt-thumbnailer: %s: %s\n", in.c_str(), err.c_str()); return 1; }
    Fm::Raster t;
    t.pixels = fit(r->pixels, size);
    t.dpi = 0;
    if (!Fm::write_png(out, t, err)) { std::fprintf(stderr, "klimt-thumbnailer: %s: %s\n", out.c_str(), err.c_str()); return 1; }
    return 0;
}
