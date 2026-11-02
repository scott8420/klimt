// klimt_selftest -- s086: local AI (libs/seg) and Select > Subject
// (core/Subject.hpp). The model store's checks always; the model itself only
// when ONNX Runtime is built in AND the model is in the store (Scott's
// machine may have neither yet -- then those checks are skipped, said).
#include "core/Subject.hpp"
#include "core/Document.hpp"
#include "core/Undo.hpp"
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
    {   // s087: the fine model in the catalog; the best one chosen; the edge; set_mask
        const auto* f = seg::model("subject-fine");
        check("seg: the catalog's fine model (ISNet, 1024 square, its own mean / spread)",
              f && f->side == 1024 && f->mean[0] == 0.5f && f->spread[0] == 1.f && std::string(f->sha256).size() == 64, "");
        const auto empty = fs::temp_directory_path() / ("klimt_selftest_seg_none_" + std::to_string(::getpid()));
        fs::create_directories(empty);
        check("seg: no model in the store -- no best one", seg::best_subject(empty.string()) == nullptr, "");
        fs::remove_all(empty);
        check("seg: Remove Background's edge -- 2 px small, 0.4 % of the side, 24 px at most",
              sub::edge_for(200, 100) == 2 && sub::edge_for(3000, 2000) == 12 && sub::edge_for(12000, 8000) == 24, "");
        klimt::core::Document d;
        klimt::core::DocJournal j{d};
        d.set_door([&j] { return j.open(); });
        klimt::core::layers::load_fixture(d);
        const auto mf = d.mask_format();
        P::TileMap hide(mf, d.image().width, d.image().height);
        hide = hide.fill({0, 0, 100, 100}, P::Px{{1, 0, 0, 0}});
        const bool ok = klimt::core::layers::set_mask(j, d, "L1", hide, "Remove Background");
        check("seg: set_mask -- a mask made, ONE step named for the verb", ok && d.get("L1")->mask && d.get("L1")->mask_on &&
              j.undo_label() == "Remove Background" && d.get("L1")->mask->get(50, 50).c[0] == 1.f, j.undo_label());
        P::TileMap again(mf, d.image().width, d.image().height);
        const bool ok2 = klimt::core::layers::set_mask(j, d, "L1", again, "Remove Background");
        check("seg: set_mask again -- the mask replaced, one more step", ok2 && d.get("L1")->mask->get(50, 50).c[0] == 0.f, "");
        check("seg: set_mask of the wrong size -- refused", !klimt::core::layers::set_mask(j, d, "L1", P::TileMap(mf, 10, 10), "x"), "");
        j.undo(); j.undo();
        check("seg: undone -- no mask", !d.get("L1")->mask, "");
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
              !seg::salient(*m, "x.onnx", px, 1, 1, err) && err.find("onnxruntime-devel") != std::string::npos, err);
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
    const auto mk = sub::mask(pic, nullptr, mf, *m, path, err);
    check("seg: Select Subject -- the disc in, the ground out",
          mk && mk->format() == mf && mk->width() == 400 && mk->get(150, 160).c[0] > 0.9f && mk->get(10, 10).c[0] < 0.1f &&
              mk->get(380, 280).c[0] < 0.1f, err);
    // A rough lasso round its left half: only what's inside it.
    P::TileMap lasso(mf, 400, 300);
    lasso = lasso.fill({40, 60, 110, 200}, P::Px{{1, 0, 0, 0}});
    const auto cut = sub::mask(pic, &lasso, mf, *m, path, err);
    check("seg: inside a rough lasso -- the disc's left half, nothing past the lasso",
          cut && cut->get(120, 160).c[0] > 0.9f && cut->get(200, 160).c[0] == 0.f, err);
    const auto stop = sub::mask(pic, nullptr, mf, *m, path, err, [] { return true; });
    check("seg: cancelled -- nothing, said", !stop && err == "cancelled", err);
    // s087: the fine model, and Remove Background's mask.
    const auto* fine = seg::model("subject-fine");
    if (!fine || seg::check(*fine, dir) != seg::State::Ready) {
        check("seg: (skipped -- " + std::string(fine ? fine->file : "the fine model") + " isn't in " + dir + ")", true, "");
        return;
    }
    check("seg: both in the store -- the fine one is the best", seg::best_subject(dir) == fine, "");
    const auto fm = sub::mask(pic, nullptr, mf, *fine, seg::path_of(*fine, dir), err);
    check("seg: the fine model -- the disc in, the ground out", fm && fm->get(150, 160).c[0] > 0.9f && fm->get(10, 10).c[0] < 0.1f, err);
    const auto hide = sub::background_mask(pic, nullptr, mf, *fine, seg::path_of(*fine, dir), sub::edge_for(400, 300), err);
    check("seg: Remove Background's mask -- hides the ground, shows the disc, a soft edge at its rim",
          hide && hide->format() == mf && hide->get(10, 10).c[0] > 0.9f && hide->get(150, 160).c[0] < 0.1f, err);
}
