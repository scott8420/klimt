// klimt_selftest -- MASKS ON GROUPS (s024): Pass Through and isolated groups
// seen through their mask; the verbs; Move carries the mask; the file.
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Move.hpp"

#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

void selftest_groups(const Check& check) {
    std::cout << "\n-- masks on groups (s024) --\n";
    core::Document d;
    core::DocJournal j{d};
    d.set_door([&] { return j.open(); });
    L::load_fixture(d);   // L2 "Group" (Pass Through) holds L4 "A": the red square (100, 100) 200 x 150 on white
    const auto seen = [&](int x, int y) { return core::sample_composite(d.image(), d.snapshot(), x, y, 0); };
    check("group: the square shows (no mask)", seen(150, 150).g < 0.05f, "");
    check("add_mask on a group: one step", L::add_mask(j, d, "L2", L::MaskFrom::HideAll) && j.size() == 1 && d.get("L2")->mask, "");
    check("Pass Through group, Hide All: its contents hidden", seen(150, 150).g > 0.99f, std::to_string(seen(150, 150).g));
    j.undo();
    // Reveal only the left half of the square.
    L::load_mask(d, "L2");   // shows (120, 110) 60 x 60 only
    check("group mask: shows inside its window", seen(150, 140).g < 0.05f, "");
    check("group mask: hides outside it", seen(250, 200).g > 0.99f, std::to_string(seen(250, 200).g));
    // Isolated (Normal) group: the same.
    { core::Document::Loading load(d); auto g = *d.get("L2"); g.blend = core::Blend::Normal; d.put("L2", g); }
    check("isolated group mask: shows inside / hides outside", seen(150, 140).g < 0.05f && seen(250, 200).g > 0.99f, "");
    // Off: the mask stays but shows everything.
    L::set_mask_on(j, d, "L2", false);
    check("group mask off: all shows", seen(250, 200).g < 0.05f, "");
    L::set_mask_on(j, d, "L2", true);
    check("invert a group's mask", L::invert_mask(j, d, "L2") && seen(150, 140).g > 0.99f && seen(250, 200).g < 0.05f, "");
    check("apply mask: refused on a group (no pixels to bake into)", !L::apply_mask(j, d, "L2"), "");
    // Move the group: its mask travels with it.
    const auto m0 = d.get("L2")->mask;
    check("move a group: one step, the mask moves too",
          core::move::by(j, d, {"L2"}, 20, 0, P::rgba(1, 1, 1, 1)) && d.get("L2")->mask != m0 &&
              d.get("L2")->mask->get(140 + 20, 140).c[0] == m0->get(140, 140).c[0] && d.get("L2")->mask->get(130, 140).c[0] != m0->get(130, 140).c[0],
          "");
    check("move a group: undo puts the mask back", (j.undo(), d.get("L2")->mask == m0), "");
    // The file keeps it.
    namespace fs = std::filesystem;
    const auto path = (fs::temp_directory_path() / ("klimt_gmask_" + std::to_string(::getpid()) + ".klimt")).string();
    core::file::Contents c;
    c.image = d.image(); c.layers = d.snapshot(); c.next_id = d.next_id();
    std::string err;
    core::file::save_klimt(path, c, err);
    const auto back = core::file::open(path, err);
    fs::remove(path);
    check("group mask: in the file", back && back->layers.at("L2").mask && back->layers.at("L2").mask->get(150, 140) == d.get("L2")->mask->get(150, 140), err);
    check("groups: the tree holds, no strays", d.check().empty() && d.strays() == 0, d.check());
}
