// klimt_selftest -- History's snapshots (s077).
#include "core/Document.hpp"
#include "core/Selection.hpp"
#include "core/Snapshot.hpp"
#include "core/Transform.hpp"

#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace S = klimt::core::snapshot;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

void selftest_snapshot(const Check& check) {
    std::cout << "\n-- history: snapshots (s077) --\n";
    core::Document d;
    core::DocJournal j{d};
    d.set_door([&] { return j.open(); });
    core::layers::load_fixture(d);
    const auto before = d.get("L4")->pixels;
    const auto layers0 = d.count();
    std::vector<S::Snapshot> have;
    have.push_back(S::take(d, S::next_name(have)));
    check("snapshot: named in turn", have[0].name == "Snapshot 1" && S::next_name(have) == "Snapshot 2", have[0].name);
    check("snapshot: it holds the image's record too", have[0].records.count(core::kImageId) && have[0].records.count("L4"), "");
    check("snapshot: taking one is no step", j.size() == 0, "");
    check("snapshot: back to how it is -- nothing to do, no step", !S::restore(j, d, have[0]) && j.size() == 0, "");

    // Change things: a flip, a new layer, a selection, a delete.
    core::xform::flip(j, d, {"L4"}, true, P::rgba(1, 1, 1, 1));
    core::layers::load_selection(d, {10, 10, 50, 50});
    const auto added = core::layers::add_layer(j, d, "Extra");
    core::layers::remove(j, d, {"L2"});
    const auto steps = j.size();
    check("snapshot: (the document changed)", d.get("L4")->pixels != before && !d.get("L2") && d.get(added), "");

    check("snapshot: back to it -- one step, named", S::restore(j, d, have[0]) && j.size() == steps + 1 && j.undo_label() == "Snapshot 1",
          j.undo_label());
    check("snapshot: the layers as they were", d.get("L4")->pixels == before && d.get("L2").has_value() && !d.get(added) && d.count() == layers0, "");
    check("snapshot: the selection as it was (none)", d.selection() == nullptr, "");
    check("snapshot: the document holds", d.check().empty(), d.check());
    j.undo();
    check("snapshot: undo -- the flip back", d.get("L4")->pixels != before, "");
    check("snapshot: undo -- the delete and the new layer back", !d.get("L2") && d.get(added).has_value(), "");
    check("snapshot: undo -- the selection back", d.selection() != nullptr, "");
    j.redo();
    check("snapshot: redo", d.get("L4")->pixels == before && d.get("L2").has_value(), "");
    check("snapshot: the document holds after", d.check().empty(), d.check());
}
