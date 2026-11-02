// klimt_selftest -- s086: local AI (libs/seg) and Select > Subject
// (core/Subject.hpp). The model store's checks always; the model itself only
// when ONNX Runtime is built in AND the model is in the store (Scott's
// machine may have neither yet -- then those checks are skipped, said).
#include "core/Subject.hpp"
#include "pixel/Pixel.hpp"
#include "seg/Seg.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

namespace P  = klimt::pixel;
namespace seg = klimt::seg;
namespace sub = klimt::core::subject;
namespace fs = std::filesystem;

using Check = std::function<void(const std::string&, bool, const std::string&)>;

void selftest_seg(const Check& check) {
    {
        const std::string abc = "abc";
        check("seg: SHA-256 of \"abc\" (FIPS 180-4's example)",
              seg::sha256(reinterpret_cast<const std::uint8_t*>(abc.data()), 3) ==
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "");
        check("seg: SHA-256 of nothing", seg::sha256(nullptr, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "");
        std::string million(1000000, 'a');   // the long example: a million 'a's, across many blocks
        check("seg: SHA-256 of a million 'a's",
              seg::sha256(reinterpret_cast<const std::uint8_t*>(million.data()), million.size()) ==
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "");
    }
    const auto* m = seg::model("subject");
    check("seg: the catalog names Select Subject's model, its size and hash", m && m->bytes > 0 && std::string(m->sha256).size() == 64, "");
    if (!m) return;
    {
        const auto dir = fs::temp_directory_path() / ("klimt_selftest_seg_" + std::to_string(::getpid()));
        fs::create_directories(dir);
        check("seg: an empty store -- Missing, said with the folder", seg::check(*m, dir.string()) == seg::State::Missing &&
              seg::state_words(*m, seg::State::Missing, dir.string()).find(dir.string()) != std::string::npos, "");
        std::ofstream(seg::path_of(*m, dir.string())) << "half a download";
        check("seg: a short file -- WrongSize", seg::check(*m, dir.string()) == seg::State::WrongSize, "");
        {
            std::ofstream f(seg::path_of(*m, dir.string()), std::ios::binary | std::ios::trunc);
            std::vector<char> z(m->bytes, 0);
            f.write(z.data(), static_cast<std::streamsize>(z.size()));
        }
        check("seg: the right size, the wrong bytes -- WrongHash", seg::check(*m, dir.string()) == seg::State::WrongHash, "");
        fs::remove_all(dir);
    }
    {   // where the model looks
        P::TileMap sel(P::Format{P::Depth::U8, P::Mode::Gray, false}, 400, 300);
        sel = sel.fill({200, 100, 100, 100}, P::Px{{1, 0, 0, 0}});
        const auto b = sub::look_box(&sel, 400, 300);
        check("seg: inside a lasso -- its box padded (16 px at least), kept on the picture",
              b.x == 184 && b.y == 84 && b.w == 132 && b.h == 132, std::to_string(b.x) + "," + std::to_string(b.y) + " " + std::to_string(b.w));
        const auto all = sub::look_box(nullptr, 400, 300);
        check("seg: no selection -- the whole picture", all.x == 0 && all.w == 400 && all.h == 300, "");
    }
    if (!seg::available()) {
        std::string err;
        const std::uint8_t px[3] = {0, 0, 0};
        check("seg: built without ONNX Runtime -- refused, saying to install onnxruntime-devel",
              !seg::salient("x.onnx", 320, px, 1, 1, err) && err.find("onnxruntime-devel") != std::string::npos, err);
        return;
    }
    check("seg: ONNX Runtime is here, its version said", !seg::version().empty(), seg::version());
    const std::string dir = seg::default_dir();
    if (seg::check(*m, dir) != seg::State::Ready) {
        check("seg: (skipped -- " + std::string(m->file) + " isn't in " + dir + ")", true, "");
        return;
    }
    // A red disc on a grey ground: the subject is the disc.
    const P::Format f8{P::Depth::U8, P::Mode::RGB, true};
    const P::TileMap pic = P::TileMap(f8, 400, 300).paint({0, 0, 400, 300}, [](int x, int y, const P::Px&) {
        const bool disc = (x - 150) * (x - 150) + (y - 160) * (y - 160) < 80 * 80;
        return disc ? P::rgba(0.85f, 0.15f, 0.1f) : P::rgba(0.55f, 0.6f, 0.62f);
    });
    const P::Format mf{P::Depth::U8, P::Mode::Gray, false};
    const std::string path = seg::path_of(*m, dir);
    std::string err;
    const auto mk = sub::mask(pic, nullptr, mf, path, m->side, err);
    check("seg: Select Subject -- the disc in, the ground out",
          mk && mk->format() == mf && mk->width() == 400 && mk->get(150, 160).c[0] > 0.9f && mk->get(10, 10).c[0] < 0.1f &&
              mk->get(380, 280).c[0] < 0.1f, err);
    // A rough lasso round its left half: only what's inside it.
    P::TileMap lasso(mf, 400, 300);
    lasso = lasso.fill({40, 60, 110, 200}, P::Px{{1, 0, 0, 0}});
    const auto cut = sub::mask(pic, &lasso, mf, path, m->side, err);
    check("seg: inside a rough lasso -- the disc's left half, nothing past the lasso",
          cut && cut->get(120, 160).c[0] > 0.9f && cut->get(200, 160).c[0] == 0.f, err);
    const auto stop = sub::mask(pic, nullptr, mf, path, m->side, err, [] { return true; });
    check("seg: cancelled -- nothing, said", !stop && err == "cancelled", err);
}
