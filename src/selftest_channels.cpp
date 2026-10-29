// klimt_selftest -- THE COLOUR CHANNELS (s026).
#include "core/ColourChannels.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"

#include <filesystem>
#include <unistd.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace S = klimt::core::select;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

void selftest_channels(const Check& check) {
    std::cout << "\n-- the colour channels (s026) --\n";
    check("names: RGB / Gray / CMYK", S::colour_channels({P::Depth::U8, P::Mode::RGB, true}).size() == 3 &&
              S::colour_channels({P::Depth::U8, P::Mode::Gray, true}).size() == 1 &&
              S::colour_channels({P::Depth::U8, P::Mode::CMYK, true})[3] == "Black", "");
    check("grey: RGB channels are their values", S::channel_grey(P::Mode::RGB, 0, 0.8f, 0.2f, 0.1f) == 0.8f &&
              S::channel_grey(P::Mode::RGB, 2, 0.8f, 0.2f, 0.1f) == 0.1f, "");
    check("grey: a CMYK plate shows ink dark -- red has no cyan, full magenta", std::abs(S::channel_grey(P::Mode::CMYK, 0, 1, 0, 0) - 1.f) < 1e-5f &&
              S::channel_grey(P::Mode::CMYK, 1, 1, 0, 0) < 1e-5f && std::abs(S::channel_grey(P::Mode::CMYK, 3, 1, 0, 0) - 1.f) < 1e-5f, "");
    check("grey: black is all Black plate", S::channel_grey(P::Mode::CMYK, 3, 0, 0, 0) == 0.f && S::channel_grey(P::Mode::CMYK, 0, 0, 0, 0) == 1.f, "");

    core::Document d;
    core::DocJournal j{d};
    d.set_door([&] { return j.open(); });
    L::load_fixture(d);   // the red square on white
    check("load Red: ONE step, \"Load Red Channel\"", S::load_colour_channel(j, d, 0) && j.size() == 1 && j.undo_label() == "Load Red Channel", j.undo_label());
    // Red channel: white (1) everywhere -- the square and the white both have full red.
    check("Red channel: all selected", S::coverage(*d.selection(), 200, 175) == 1.f && S::coverage(*d.selection(), 50, 50) == 1.f, "");
    S::load_colour_channel(j, d, 1);
    check("Green channel: the white selected, the red square not", S::coverage(*d.selection(), 50, 50) == 1.f && S::coverage(*d.selection(), 200, 175) == 0.f, "");
    S::load_colour_channel(j, d, 2, S::Op::Intersect);
    check("Blue intersect: the same (blue = green here)", S::coverage(*d.selection(), 50, 50) == 1.f && S::coverage(*d.selection(), 200, 175) == 0.f, "");
    const auto n0 = d.channels().size();
    const auto id = S::duplicate_colour_channel(j, d, 2);
    check("duplicate Blue: a saved channel \"Blue copy\", one step", !id.empty() && d.channels().size() == n0 + 1 &&
              d.channels().at(id).name == "Blue copy" && j.undo_label() == "Duplicate Blue Channel", j.undo_label());
    check("refused: no such channel", !S::load_colour_channel(j, d, 3) && S::duplicate_colour_channel(j, d, -1).empty(), "");
    check("channels: the tree holds, no strays", d.check().empty() && d.strays() == 0, d.check());
    // s026 m3: the selection is kept in the file.
    {
        namespace fs = std::filesystem;
        S::rect(j, d, {50, 60, 120, 90});
        const auto path = (fs::temp_directory_path() / ("klimt_sel_" + std::to_string(::getpid()) + ".klimt")).string();
        klimt::core::file::Contents c;
        c.image = d.image(); c.layers = d.snapshot(); c.channels = d.channels(); c.next_id = d.next_id();
        c.selection = d.selection_ptr();
        std::string err;
        klimt::core::file::save_klimt(path, c, err);
        const auto back = klimt::core::file::open(path, err);
        check("the selection: in the file", back && back->selection && S::coverage(*back->selection, 100, 100) == 1.f &&
                  S::coverage(*back->selection, 10, 10) == 0.f, err);
        c.selection = nullptr;
        klimt::core::file::save_klimt(path, c, err);
        const auto none = klimt::core::file::open(path, err);
        check("no selection: none in the file", none && !none->selection, err);
        fs::remove(path);
    }
}
