// klimt_selftest -- layers with pixels beyond the image (s010 m1): outside
// tiles in the map, kept by edits, by Move, Crop, Canvas Size and Image
// Size; Reveal All; and the .klimt round trip.
#include "core/Canvas.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Move.hpp"
#include "core/Paint.hpp"
#include "core/Selection.hpp"

#include <filesystem>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace C = klimt::core::canvas;
namespace M = klimt::core::move;
namespace fs = std::filesystem;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
struct Fix {
    core::Document d;
    core::DocJournal j{d};
    Fix() {
        d.set_door([this] { return j.open(); });
        core::layers::load_fixture(d);
    }
    P::Px at(const std::string& id, int x, int y) const {
        const auto l = d.get(id);
        return l && l->pixels ? l->pixels->get(x, y) : P::transparent();
    }
    const P::TileMap& px(const std::string& id) const { return *d.get(id)->pixels; }
};
bool red(const P::Px& p) { return p.c[0] > 0.99f && p.c[1] < 0.01f && p.c[3] > 0.99f; }
bool clear(const P::Px& p) { return p.c[3] <= 0.f; }
const P::Px kWhite = P::rgba(1, 1, 1, 1);
}  // namespace

void selftest_bounds(const Check& check) {
    std::cout << "\n-- bounds: pixels beyond the image (s010) --\n";
    {
        const P::Format f{P::Depth::U8, P::Mode::RGB, true};
        P::TileMap m(f, 300, 200);
        const auto t = P::Tile::uniform(f, P::rgba(0, 1, 0, 1));
        m = m.with_outside({{{-1, 0}, t}, {{1, 0}, t}});   // left of the image; the edge cell's part past x = 300
        check("bounds: an outside pixel reads back", m.get(-10, 5) == P::rgba(0, 1, 0, 1) && m.get(310, 5) == P::rgba(0, 1, 0, 1), "");
        check("bounds: inside the image, the grid (not the edge cell's outside tile)", clear(m.get(299, 5)), "");
        check("bounds: tile_count counts them", m.tile_count() == 2, std::to_string(m.tile_count()));
        const auto filled = m.fill({0, 0, 300, 200}, P::rgba(1, 0, 0, 1));
        check("bounds: a fill keeps them", filled.has_outside() && filled.get(-10, 5) == P::rgba(0, 1, 0, 1), "");
        const auto painted = m.paint({10, 10, 5, 5}, [](int, int, const P::Px&) { return P::rgba(0, 0, 1, 1); });
        check("bounds: paint keeps them", painted.has_outside(), "");
        check("bounds: with_grid keeps them", m.with_grid({}).has_outside(), "");
        check("bounds: content bounds reach outside", C::content_bounds(m) == P::Rect{-256, 0, 256 + 512 - 300 + 300, 256},
              std::to_string(C::content_bounds(m).x) + "," + std::to_string(C::content_bounds(m).w));
    }
    {
        // Moved off in one step, back in another: nothing lost.
        Fix t;
        M::by(t.j, t.d, {"L4"}, 450, 0, kWhite);   // the red square (100..300) -> 550..750: past x = 600
        check("bounds: moved half off the canvas", red(t.at("L4", 590, 150)) && t.px("L4").has_outside(), "");
        check("bounds: the part beyond is kept", red(t.at("L4", 700, 150)) && red(t.at("L4", 749, 249)), "");
        check("bounds: the document holds", t.d.check().empty(), t.d.check());
        M::by(t.j, t.d, {"L4"}, -450, 0, kWhite);
        check("bounds: moved back -- every pixel home", red(t.at("L4", 100, 100)) && red(t.at("L4", 299, 249)) &&
                                                       clear(t.at("L4", 300, 150)) && !t.px("L4").has_outside(), "");
        check("bounds: two steps", t.j.size() == 2, std::to_string(t.j.size()));
    }
    {
        // Wholly off: the layer isn't empty; painting the image keeps what is beyond.
        Fix t;
        M::by(t.j, t.d, {"L4"}, 0, -500, kWhite);
        check("bounds: wholly off, the layer still holds it", t.d.get("L4")->pixels != nullptr && red(t.at("L4", 150, -350)), "");
        core::paint::Brush b;
        b.size = 40;
        b.colour = P::rgba(0, 0, 1, 1);
        core::paint::stroke(t.j, t.d, "L4", {{50, 50}, {200, 60}}, b);
        check("bounds: a brush stroke keeps it", red(t.at("L4", 150, -350)), "");
        core::layers::load_selection(t.d, {0, 0, 600, 400});
        core::layers::fill_selection(t.j, t.d, "L4", P::rgba(0, 1, 0, 1));
        check("bounds: a fill through the selection keeps it", red(t.at("L4", 150, -350)), "");
    }

    std::cout << "\n-- bounds: Crop, Canvas Size, Image Size, Reveal All (s010) --\n";
    {
        Fix t;
        C::crop(t.j, t.d, {200, 0, 400, 400}, kWhite);   // cuts the square (100..300) at x = 200
        check("bounds: a crop keeps what it cuts", t.d.image().width == 400 && red(t.at("L4", 0, 150)) && red(t.at("L4", -100, 150)), "");
        check("bounds: the document holds", t.d.check().empty(), t.d.check());
        check("bounds: Reveal All", C::reveal_all(t.j, t.d), "");
        // Background (white, the whole old canvas) kept its cut part too: all of it comes back.
        check("bounds: the canvas grows to show it again", t.d.image().width == 600 && red(t.at("L4", 100, 150)) &&
                                                          clear(t.at("L4", 99, 150)) && !t.px("L4").has_outside() &&
                                                          !t.px("L1").has_outside(), std::to_string(t.d.image().width));
        check("bounds: Reveal All with nothing beyond keeps no step", !C::reveal_all(t.j, t.d), "");
    }
    {
        Fix t;
        C::crop(t.j, t.d, {200, 0, 400, 400}, kWhite, true);
        check("bounds: Delete Cropped Pixels deletes them", clear(t.at("L4", -100, 150)) && !t.px("L4").has_outside(), "");
    }
    {
        Fix t;
        C::canvas_size(t.j, t.d, 200, 400, 0, 0, kWhite);   // narrower: the square's right part goes beyond
        check("bounds: a smaller canvas keeps what's past it", red(t.at("L4", 250, 150)), "");
        C::canvas_size(t.j, t.d, 600, 400, 0, 0, kWhite);
        check("bounds: ...and gives it back when it grows", red(t.at("L4", 250, 150)) && red(t.at("L4", 299, 249)) &&
                                                             !t.px("L4").has_outside(), "");
    }
    {
        Fix t;
        M::by(t.j, t.d, {"L4"}, -300, 0, kWhite);   // the square -> -200..0
        C::image_size(t.j, t.d, 300, 200, 72);
        check("bounds: Image Size scales the pixels beyond too", red(t.at("L4", -60, 70)) && clear(t.at("L4", 20, 70)),
              std::to_string(t.at("L4", -60, 70).c[3]));
        check("bounds: the document holds", t.d.check().empty(), t.d.check());
    }

    std::cout << "\n-- bounds: the .klimt file (s010) --\n";
    {
        Fix t;
        M::by(t.j, t.d, {"L4"}, 0, -200, kWhite);   // the square -> y -100..50
        core::file::Contents c;
        c.image = t.d.image();
        c.layers = t.d.snapshot();
        c.next_id = t.d.next_id();
        const auto dir = fs::temp_directory_path() / "klimt_selftest_bounds";
        fs::create_directories(dir);
        const auto path = (dir / "beyond.klimt").string();
        std::string err;
        check("bounds: saves", core::file::save_klimt(path, c, err), err);
        const auto back = core::file::open(path, err);
        check("bounds: opens", back.has_value(), err);
        if (back) {
            const auto& m = *back->layers.at("L4").pixels;
            check("bounds: the pixels beyond come back", m.has_outside() && red(m.get(150, -50)) && red(m.get(299, 49)), "");
        }
        fs::remove_all(dir);
    }
}
