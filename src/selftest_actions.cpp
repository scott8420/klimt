// klimt_selftest -- s084: actions and batch (core/Actions.hpp). Acts made
// from settings, kept in a library file, played on a private document and
// landed as ONE step; stops in words; Image Size keeping each picture's
// shape; a folder through an action. Rules only.
#include "core/Actions.hpp"
#include "core/Adjust.hpp"
#include "core/File.hpp"
#include "core/Filter.hpp"
#include "core/Selection.hpp"
#include "core/Snapshot.hpp"
#include "core/Undo.hpp"
#include "formats/Formats.hpp"
#include "pixel/Pixel.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

namespace P  = klimt::pixel;
namespace F  = klimt::formats;
namespace core = klimt::core;
namespace ac = klimt::core::actions;
namespace fs = std::filesystem;

using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
fs::path scratch() {
    static const fs::path d = [] {
        auto p = fs::temp_directory_path() / ("klimt_selftest_actions_" + std::to_string(::getpid()));
        fs::remove_all(p);
        fs::create_directories(p);
        return p;
    }();
    return d;
}
struct Doc {
    core::Document d;
    core::DocJournal j{d};
    Doc() { d.set_door([this] { return j.open(); }); core::layers::load_fixture(d); }
};
bool write_png(const std::string& f, int w, int h, P::Px c) {
    F::Raster r;
    r.pixels = P::TileMap(P::Format{P::Depth::U8, P::Mode::RGB, true}, w, h).fill({0, 0, w, h}, c);
    std::string err;
    return F::write_png(f, r, err);
}
}  // namespace

void selftest_actions(const Check& check) {
    const auto red = P::rgba(1, 0, 0);
    // ── acts: made from settings, read back ─────────────────────────────────
    core::Filter blur;
    blur.kind = core::FilterKind::GaussianBlur;
    blur.radius = 3;
    {
        const auto t = core::file::filter_text(blur);
        const auto back = core::file::filter_parse(t);
        check("actions: a filter's settings as text and back", back && back->kind == blur.kind && back->radius == 3, t.substr(0, 60));
        check("actions: every verb the acts use is in the table", ac::known("filter.apply") && ac::known("image.size") &&
              ac::known("select.fill") && !ac::known("nonsense"), "");
        const auto a = ac::apply_filter(blur);
        check("actions: an act names itself for the panel", a.verb == "filter.apply" && a.label == "Gaussian Blur" && a.on, a.label);
    }
    ac::Action act{"Red and soft", {ac::select_all(), ac::fill_selection(red), ac::select_none(), ac::apply_filter(blur),
                                    ac::new_layer("Notes"), ac::set_opacity(50)}};
    // ── the library: kept and read back ────────────────────────────────────
    {
        const std::string f = (scratch() / "conf" / "actions.json").string();
        const auto all = std::vector<ac::Action>{act, ac::starters()[0]};
        const bool ok = ac::save(f, all);
        const auto back = ac::load(f);
        check("actions: the library saved and read back the same", ok && back == all, std::to_string(back.size()));
        check("actions: no library yet -- none (not an error)", ac::load((scratch() / "nope.json").string()).empty(), "");
        check("actions: next_name -- one past the highest", ac::next_name({{"Action 2", {}}, {"Other", {}}}) == "Action 3", "");
        const auto st = ac::starters();
        check("actions: three starters, each with steps", st.size() == 3 &&
              std::all_of(st.begin(), st.end(), [](const ac::Action& a) { return !a.acts.empty(); }), "");
    }
    // ── played on a private document; landed as ONE step ──────────────────
    {
        Doc t;
        const auto start = core::snapshot::take(t.d, "start");
        const auto before_bg = t.d.get("L1")->pixels;
        const auto p = ac::play(start, "L1", act);
        check("actions: played -- every step ran", p.ok && p.ran == 6, p.error);
        check("actions: the live document untouched while it plays (a private copy)", t.d.get("L1")->pixels == before_bg && !t.j.can_undo(), "");
        const bool landed = p.ok && core::snapshot::restore(t.j, t.d, p.result);
        const auto bg = t.d.get("L1");
        check("actions: landed as ONE step, named for it", landed && t.j.undo_label() == "Action “Red and soft”", t.j.undo_label());
        check("actions: filled red then blurred (the middle red, the selection gone)",
              bg && bg->pixels && bg->pixels->get(300, 200) == red && !t.d.selection(), "");
        const auto made = t.d.get(p.picked);
        check("actions: the new layer is picked, at 50 %", made && made->name == "Notes" && made->opacity == 50, p.picked);
        t.j.undo();
        check("actions: undone whole -- the picture as it was", t.d.get("L1")->pixels == before_bg && !t.d.get(p.picked), "");
    }
    // ── stops, in words; switched off skips ────────────────────────────────
    {
        Doc t;
        const auto start = core::snapshot::take(t.d, "start");
        const ac::Action bad{"On a group", {ac::select_all(), ac::fill_selection(red)}};
        const auto p = ac::play(start, "L2", bad);   // the group
        check("actions: a step that can't run stops it -- which step, and why",
              !p.ok && p.error.rfind("step 2, Fill: ", 0) == 0, p.error);
        auto off = bad;
        off.acts[1].on = false;
        const auto q = ac::play(start, "L2", off);
        check("actions: a step switched off is skipped", q.ok && q.ran == 1, q.error);
        const ac::Action feather{"Feather", {ac::select_none(), ac::feather(4)}};
        const auto r = ac::play(start, "L1", feather);
        check("actions: Feather with nothing selected -- \"nothing is selected\"", !r.ok && r.error.find("nothing is selected") != std::string::npos, r.error);
        int calls = 0;
        const auto c = ac::play(start, "L1", act, [&calls] { return ++calls > 2; });
        check("actions: cancelled between steps -- nothing comes back", !c.ok && c.error == "cancelled", c.error);
        ac::Act unknown{"from.the.future", "{}", "Something new", true};
        const auto u = ac::play(start, "L1", {"New", {unknown}});
        check("actions: a step this Klimt doesn't know -- said", !u.ok && u.error.find("doesn't know") != std::string::npos, u.error);
    }
    // ── Image Size: each picture keeps its shape ───────────────────────────
    {
        Doc t;   // 600 x 400
        const auto sized = ac::image_size(300, 200, 72, core::canvas::Filter::Smooth, 600, 400);
        core::Document tall;
        core::DocJournal tj{tall};
        tall.set_door([&tj] { return tj.open(); });
        core::layers::load_fixture(tall);
        core::canvas::image_size(tj, tall, 200, 400, tall.image().dpi);   // 1 : 2
        const auto p = ac::play(core::snapshot::take(tall, "s"), "L1", {"Size", {sized}});
        const auto im = p.ok ? p.result.records.at(core::kImageId).image : nullptr;
        check("actions: Image Size with proportions kept -- a tall picture stays tall (300 wide, 600 high)",
              im && im->width == 300 && im->height == 600, im ? std::to_string(im->width) + " x " + std::to_string(im->height) : p.error);
        auto rel = ac::starters()[0];   // half, by ratio
        const auto h = ac::play(core::snapshot::take(tall, "s"), "L1", rel);
        const auto hm = h.ok ? h.result.records.at(core::kImageId).image : nullptr;
        check("actions: \"Half Size\" by ratio -- 100 x 200, its resolution kept",
              hm && hm->width == 100 && hm->height == 200 && hm->dpi == tall.image().dpi, h.error);
        const auto turned = ac::play(core::snapshot::take(t.d, "s"), "L1", {"Turn", {ac::turn(0)}});
        const auto tm = turned.ok ? turned.result.records.at(core::kImageId).image : nullptr;
        check("actions: Rotate 90° -- 400 x 600", tm && tm->width == 400 && tm->height == 600, turned.error);
    }
    // ── adjustment layers: made, then retuned in Properties ────────────────
    {
        auto lv = core::adjust::fresh(core::AdjustKind::HueSat);
        auto a = ac::adjust_layer(lv);
        lv.hue = 40;
        ac::retune(a, lv);
        Doc t;
        const auto p = ac::play(core::snapshot::take(t.d, "s"), "L5", {"Hue", {a}});
        const auto l = p.ok ? p.result.records.find(p.picked) : p.result.records.end();
        check("actions: an adjustment layer made with the settings it was given after (retune)",
              p.ok && l != p.result.records.end() && l->second.kind == core::Kind::Adjustment &&
                  l->second.adjust.hue == 40, p.error);
    }
    // ── batch: a folder through an action ──────────────────────────────────
    {
        const auto in = scratch() / "in", out = scratch() / "out";
        fs::create_directories(in);
        write_png((in / "a.png").string(), 400, 200, P::rgba(0.2f, 0.4f, 0.8f));
        write_png((in / "b.png").string(), 100, 300, P::rgba(0.9f, 0.4f, 0.1f));
        std::ofstream((in / "notes.txt").string()) << "not a picture";
        write_png((in / ".hidden.png").string(), 10, 10, red);
        ac::Batch b;
        b.source = in.string();
        b.dest = out.string();
        b.kind = F::Kind::PNG;
        const auto web = ac::starters()[0];
        const auto srcs = ac::sources(b);
        check("actions: batch -- the pictures in the folder (not the text, not the hidden)", srcs.size() == 2, std::to_string(srcs.size()));
        check("actions: batch -- the target named for the source, the format's extension",
              ac::target_of(b, (in / "a.png").string()) == (out / "a.png").string(), ac::target_of(b, (in / "a.png").string()));
        check("actions: batch -- runs", ac::refusal(b, web).empty(), ac::refusal(b, web));
        const auto r = ac::run_batch(b, web);
        std::string err;
        const auto a = core::file::open((out / "a.png").string(), err);
        const auto bb = core::file::open((out / "b.png").string(), err);
        check("actions: batch -- each written at half its own size", r.done == 2 && r.failed == 0 && a && bb &&
              a->image.width == 200 && a->image.height == 100 && bb->image.width == 50 && bb->image.height == 150,
              std::to_string(r.done) + " done");
        const auto again = ac::run_batch(b, web);
        check("actions: batch -- run again, the files there are skipped (said)", again.done == 0 && again.skipped == 2 &&
              !again.lines.empty() && again.lines[0].find("skipped") != std::string::npos, "");
        b.overwrite = true;
        check("actions: batch -- Replace ticked, written again", ac::run_batch(b, web).done == 2, "");
        ac::Batch same = b;
        same.dest.clear();
        same.overwrite = false;
        check("actions: batch -- into its own folder with no suffix: refused, saying how", !ac::refusal(same, web).empty(), ac::refusal(same, web));
        same.suffix = "-web";
        check("actions: batch -- a suffix: beside each, \"a-web.png\"", ac::refusal(same, web).empty() &&
              ac::target_of(same, (in / "a.png").string()) == (in / "a-web.png").string(), "");
        ac::Batch k = b;
        k.dest = (scratch() / "klimt").string();
        k.kind = F::Kind::Klimt;
        const ac::Action bad{"Feather nothing", {ac::select_none(), ac::feather(3)}};
        const auto rf = ac::run_batch(k, bad);
        check("actions: batch -- a step that can't run: that file failed, said, nothing written",
              rf.failed == 2 && rf.done == 0 && rf.lines.size() == 2 && rf.lines[0].find("a.png: step 2") == 0 &&
                  !fs::exists(scratch() / "klimt" / "a.klimt"), rf.lines.empty() ? "" : rf.lines[0]);
        const auto rk = ac::run_batch(k, web);
        const auto ko = core::file::open((scratch() / "klimt" / "a.klimt").string(), err);
        check("actions: batch -- as .klimt", rk.done == 2 && ko && ko->image.width == 200, err);
        int n = 0;
        const auto rc = ac::run_batch(b, web, [&n] { return ++n > 1; });
        check("actions: batch -- cancelled, said", rc.cancelled, "");
    }
}
