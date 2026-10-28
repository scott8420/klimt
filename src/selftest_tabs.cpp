// klimt_selftest -- DOCUMENT TABS (s020): parked documents, a history per
// tab, saved marks that never cross tabs, closing, reordering.
#include "core/Document.hpp"
#include "core/Tabs.hpp"

#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

void selftest_tabs(const Check& check) {
    std::cout << "\n-- document tabs (s020) --\n";
    core::Document d;
    core::DocJournal j{d};
    d.set_door([&] { return j.open(); });
    L::load_fixture(d);
    core::Tabs tabs(d, j);
    const auto w0 = d.image().width;
    const auto n0 = d.count();
    check("tabs: one to start, the live document", tabs.count() == 1 && tabs.live() == 0 && !tabs.dirty(0), "");

    L::rename(j, d, "L4", "Ruby");
    check("tabs: a step makes tab 0 dirty", tabs.dirty(0) && j.size() == 1, "");

    // A second tab: empty, a fresh history.
    const auto b = tabs.add(0);
    check("add: after the live one, parked, not shown", b == 1 && tabs.live() == 0 && tabs.count() == 2, "");
    check("add: the new tab is clean and empty", !tabs.dirty(1) && tabs.image(1).width == 0 && tabs.layer_count(1) == 0, "");
    check("show: refused nowhere -- switches", tabs.show(1) && tabs.live() == 1, "");
    check("show: the live document is the new tab's (empty)", d.count() == 0 && d.image().width == 0, "");
    check("show: its journal is its own (nothing to undo)", !j.can_undo() && j.size() == 0, "");
    check("show: tab 0 parked, still dirty, its size kept", tabs.dirty(0) && tabs.image(0).width == w0 && tabs.layer_count(0) == n0, "");
    check("show: no stray writes", d.strays() == 0, std::to_string(d.strays()));

    // Fill the second tab and make a step there.
    {
        core::Document::Loading load(d);
        core::Image im;
        im.width = 300;
        im.height = 200;
        d.replace(im, {}, 1);
    }
    const auto id = L::add_layer(j, d, "Paint");
    check("tab 1: a step of its own", j.size() == 1 && tabs.dirty(1) && d.get(id).has_value(), "");
    const auto saved1 = tabs.mark(1);
    tabs.live_info().saved_mark = saved1;   // as a save would
    check("tab 1: saved -> clean", !tabs.dirty(1), "");

    // Back to tab 0: its document, its history.
    check("show 0: back", tabs.show(0) && d.image().width == w0 && d.get("L4")->name == "Ruby", "");
    check("show 0: its own undo (the rename)", j.can_undo() && j.undo_label().find("Rename") != std::string::npos, j.undo_label());
    check("show 0: tab 1's mark never matches tab 0", tabs.mark(0) != saved1 && tabs.dirty(0), "");
    j.undo();
    check("undo in tab 0: back to the load -> clean; tab 1 untouched", !tabs.dirty(0) && d.get("L4")->name != "Ruby" && !tabs.dirty(1), "");
    j.redo();
    check("tabs: parked tab 1 kept its step and size", tabs.image(1).width == 300 && tabs.layer_count(1) == 1, "");

    // Inside a gesture a switch is refused.
    L::opacity_begin(j, d, std::string("L4"));
    check("show: refused inside a gesture", !tabs.show(1) && tabs.live() == 0, "");
    L::opacity_to(j, d, std::string("L4"), 40);
    L::opacity_end(j);
    check("show: after the gesture ends", tabs.show(1) && tabs.live() == 1 && tabs.show(0), "");

    // Keys and paths.
    tabs.info(1).path = "/tmp/x.klimt";
    check("by_path / by_key", tabs.by_path("/tmp/x.klimt") == 1u && tabs.by_key(tabs.info(1).key) == 1u && !tabs.by_path(""), "");

    // Reorder: the live one stays live.
    const auto c = tabs.add(1);
    const auto key0 = tabs.info(0).key;
    tabs.reorder(0, 2);
    check("reorder: tab 0 to the end, still live", tabs.live() == 2 && tabs.info(2).key == key0 && c == 2, "");
    check("reorder: the live document unchanged", d.get("L4").has_value(), "");
    tabs.reorder(2, 0);

    // Close: a parked one, then the live one (its right neighbour shows).
    const auto key2 = tabs.info(2).key;
    check("close parked: gone, live unchanged", tabs.close(2) && tabs.count() == 2 && tabs.live() == 0 && !tabs.by_key(key2), "");
    check("close live: the neighbour shows", tabs.close(0) && tabs.count() == 1 && tabs.live() == 0 && d.image().width == 300, "");
    check("close live: its history came too", j.size() == 1 && !tabs.dirty(0), "");
    // The last tab: emptied, never removed.
    const auto key_last = tabs.info(0).key;
    check("close the last: emptied, not removed", tabs.close(0) && tabs.count() == 1 && d.count() == 0 && d.image().width == 0, "");
    check("close the last: a new key, a fresh clean history", tabs.info(0).key != key_last && !j.can_undo() && !tabs.dirty(0), "");
    check("tabs: the tree's invariants hold throughout", d.check().empty() && d.strays() == 0, d.check());
}
