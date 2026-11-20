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
#include "seg/Seg.hpp"

#include <cmath>
#include <cstdio>
#include <thread>
#include <atomic>
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
        std::atomic<int> n{0};   // s096: asked from several lanes at once
        const auto rc = ac::run_batch(b, web, [&n] { return ++n > 1; });
        check("actions: batch -- cancelled, said", rc.cancelled, "");
    }
    // ── s096: many files at once -- each right, the report in the files' order ──
    {
        const auto in = scratch() / "many", out = scratch() / "many_out";
        fs::create_directories(in);
        for (int i = 0; i < 24; ++i) {
            char nm[32];
            std::snprintf(nm, sizeof nm, "p%02d.png", i);
            write_png((in / nm).string(), 40 + 2 * i, 60, P::rgba(0.1f * static_cast<float>(i % 10), 0.5f, 0.5f));
        }
        write_png((in / "p05x.png").string(), 10, 10, P::rgba(1, 0, 0));
        ac::Batch b;
        b.source = in.string();
        b.dest = out.string();
        b.kind = F::Kind::PNG;
        const auto web = ac::starters()[0];
        check("actions: batch -- lanes: more than one on a machine with cores, never more than 8",
              ac::batch_lanes(web, 25) >= std::min(2u, std::max(1u, std::thread::hardware_concurrency())) && ac::batch_lanes(web, 25) <= 8,
              std::to_string(ac::batch_lanes(web, 25)));
        check("actions: batch -- lanes: one for one file", ac::batch_lanes(web, 1) == 1, "");
        const ac::Action ai{"Subject", {ac::select_subject()}};
        check("actions: batch -- lanes: one when a local model runs", ac::batch_lanes(ai, 25) == 1, "");
        const auto r = ac::run_batch(b, web);
        bool all = r.done == 25 && r.failed == 0;
        std::string err, bad;
        for (int i = 0; i < 24 && all; ++i) {
            char nm[32];
            std::snprintf(nm, sizeof nm, "p%02d.png", i);
            const auto o = core::file::open((out / nm).string(), err);
            if (!o || o->image.width != (40 + 2 * i) / 2 || o->image.height != 30) { all = false; bad = nm; }
        }
        check("actions: batch -- 25 files at once, each at half its own size", all, std::to_string(r.done) + " done " + bad);
        const ac::Action fail{"Feather nothing", {ac::select_none(), ac::feather(3)}};
        b.overwrite = true;
        const auto rf = ac::run_batch(b, fail);
        bool ordered = rf.lines.size() == 25;
        for (std::size_t i = 1; ordered && i < rf.lines.size(); ++i) if (rf.lines[i - 1] > rf.lines[i]) ordered = false;
        check("actions: batch -- the failures listed in the files' order", rf.failed == 25 && ordered, rf.lines.empty() ? "" : rf.lines[0]);
    }
    // ── s096: editing an action's steps ────────────────────────────────────
    {
        core::Filter blur;
        blur.kind = core::FilterKind::GaussianBlur;
        blur.radius = 2;
        core::Adjust mix = core::adjust::fresh(core::AdjustKind::Levels);
        ac::Action a{"Edit me", {ac::select_all(), ac::apply_filter(blur), ac::apply_adjust(mix), ac::feather(3)}};
        check("actions: move a step down -- it lands there, the rest close up",
              ac::move_act(a, 0, 2) && a.acts[0].verb == "filter.apply" && a.acts[1].verb == "adjust.apply" && a.acts[2].verb == "select.all" && a.acts[3].verb == "select.feather", "");
        check("actions: move a step up", ac::move_act(a, 3, 0) && a.acts[0].verb == "select.feather" && a.acts[1].verb == "filter.apply", "");
        check("actions: a move out of range -- refused", !ac::move_act(a, 0, 4) && !ac::move_act(a, -1, 0) && !ac::move_act(a, 1, 1), "");
        check("actions: duplicate a step -- the copy just after it",
              ac::duplicate_act(a, 1) && a.acts.size() == 5 && a.acts[1] == a.acts[2] && a.acts[3].verb == "adjust.apply", "");
        const auto f = ac::filter_of(a.acts[1]);
        check("actions: a filter step's settings read back", f && f->kind == core::FilterKind::GaussianBlur && std::fabs(f->radius - 2) < 1e-9, "");
        check("actions: an adjustment step's settings read back", ac::adjust_of(a.acts[3]) && ac::adjust_of(a.acts[3])->kind == core::AdjustKind::Levels, "");
        check("actions: a step without settings -- none", !ac::adjust_of(a.acts[0]) && !ac::filter_of(a.acts[0]), "");
        core::Filter wider = *f;
        wider.radius = 7;
        a.acts[1].on = false;
        const auto edited = ac::with_filter(a.acts[1], wider);
        check("actions: a filter step edited -- its kind, its tick kept, the new radius",
              edited.verb == "filter.apply" && !edited.on && ac::filter_of(edited) && std::fabs(ac::filter_of(edited)->radius - 7) < 1e-9, edited.label);
        const auto live = ac::with_filter(ac::live_filter(blur), wider);
        check("actions: a live filter step stays live", live.verb == "filter.live", live.verb);
        const auto lay = ac::with_adjust(ac::adjust_layer(mix), mix);
        check("actions: an adjustment-layer step stays one", lay.verb == "adjust.layer", lay.verb);
    }
    // ── s096: Mode > Grayscale recorded; a Gray picture written Gray ──────
    {
        const auto in = scratch() / "gray_in", out = scratch() / "gray_out";
        fs::create_directories(in);
        write_png((in / "g.png").string(), 64, 48, P::rgba(0.8f, 0.2f, 0.1f));
        ac::Batch b;
        b.source = in.string();
        b.dest = out.string();
        b.kind = F::Kind::PNG;
        const ac::Action gray{"Red to Gray", {ac::set_mode(P::Mode::Gray)}};
        check("actions: Mode: Grayscale -- an act, named", gray.acts[0].verb == "image.mode" && gray.acts[0].label == "Mode: Grayscale", gray.acts[0].label);
        const auto r = ac::run_batch(b, gray);
        std::string err;
        const auto o = core::file::open((out / "g.png").string(), err);
        // The PNG's own header: byte 25 is its colour type (0 Gray, 2 RGB, 4 / 6 with alpha).
        const auto colour_type = [](const fs::path& f) {
            std::ifstream in(f, std::ios::binary);
            char h[26] = {};
            in.read(h, 26);
            return in ? static_cast<int>(static_cast<unsigned char>(h[25])) : -1;
        };
        check("actions: batch -- Mode: Grayscale, the PNG written Gray, one channel (no alpha: nothing clear)",
              r.done == 1 && o && o->image.format.mode == P::Mode::Gray && colour_type(out / "g.png") == 0,
              std::to_string(colour_type(out / "g.png")));
        const auto rgb_in = core::file::open((in / "g.png").string(), err);
        if (rgb_in) {
            core::file::export_image((scratch() / "opaque.png").string(), F::Kind::PNG, rgb_in->image, rgb_in->layers, err);
            const auto back = core::file::open((scratch() / "opaque.png").string(), err);
            check("actions: an opaque RGB picture exported -- RGB, no alpha channel",
                  back && back->image.format.mode == P::Mode::RGB && colour_type(scratch() / "opaque.png") == 2,
                  std::to_string(colour_type(scratch() / "opaque.png")));
        }
    }
    // ── s094: a selection drawn on the canvas, as fractions of it ─────────
    {
        using SP = core::select::Pt;
        const auto box = ac::select_shape(ac::Drawn::Rect, core::select::Op::Replace, {SP{0.25, 0.25}, SP{0.75, 0.5}});
        check("actions: a drawn box names itself -- its share of the canvas", box.verb == "select.shape" && ac::known("select.shape") &&
              box.label == "Rectangle Select  50 × 25 % of the canvas", box.label);
        Doc t;
        const auto W = t.d.image().width, H = t.d.image().height;
        core::Document::Id picked = "L1";
        std::string err;
        const bool ran = ac::run(t.j, t.d, picked, box, err);
        const auto b = t.d.selection() ? core::select::bounds(*t.d.selection()) : P::Rect{};
        check("actions: a drawn box played -- the same place on this canvas",
              ran && b.x == W / 4 && b.y == H / 4 && b.w == W / 2 && b.h == H / 4, std::to_string(b.x) + "," + std::to_string(b.y) + " " + std::to_string(b.w) + "x" + std::to_string(b.h));
        const auto add = ac::select_shape(ac::Drawn::Rect, core::select::Op::Add, {SP{0.0, 0.0}, SP{0.1, 0.1}});
        check("actions: Shift's Add -- said, and the corner joins the box",
              add.label.find("(Add)") != std::string::npos && ac::run(t.j, t.d, picked, add, err) &&
                  core::select::coverage(*t.d.selection(), 2, 2) == 1.f && core::select::coverage(*t.d.selection(), W / 2, H / 3) == 1.f, add.label);
        const auto ell = ac::select_shape(ac::Drawn::Ellipse, core::select::Op::Replace, {SP{0.0, 0.0}, SP{1.0, 1.0}});
        check("actions: a drawn ellipse -- the middle in, the corner out",
              ac::run(t.j, t.d, picked, ell, err) && core::select::coverage(*t.d.selection(), W / 2, H / 2) == 1.f &&
                  core::select::coverage(*t.d.selection(), 1, 1) == 0.f, err);
        const auto tri = ac::select_shape(ac::Drawn::Lasso, core::select::Op::Replace, {SP{0.1, 0.1}, SP{0.9, 0.1}, SP{0.5, 0.9}});
        check("actions: a drawn lasso -- its points counted; inside in, outside out",
              tri.label == "Lasso  3 points" && ac::run(t.j, t.d, picked, tri, err) && core::select::coverage(*t.d.selection(), W / 2, H / 3) == 1.f &&
                  core::select::coverage(*t.d.selection(), W / 20, H * 8 / 10) == 0.f, tri.label);
        const auto two = ac::select_shape(ac::Drawn::Lasso, core::select::Op::Replace, {SP{0.1, 0.1}, SP{0.9, 0.1}});
        check("actions: a lasso of two points refused, in words", !ac::run(t.j, t.d, picked, two, err) && err == "too few points", err);
        // On a picture twice the size (Image Size first): the box goes with it.
        const ac::Action grow{"Box on bigger", {ac::image_size(W * 2, H * 2, t.d.image().dpi, core::canvas::Filter::Nearest, W, H), box}};
        Doc u;
        const auto p = ac::play(core::snapshot::take(u.d, "start"), "L1", grow);
        const bool landed = p.ok && core::snapshot::restore(u.j, u.d, p.result);
        const auto b2 = (landed && u.d.selection()) ? core::select::bounds(*u.d.selection()) : P::Rect{};
        check("actions: on a picture of another size -- the same share of it",
              landed && u.d.image().width == W * 2 && b2.x == W / 2 && b2.w == W && b2.y == H / 2 && b2.h == H / 2, p.error);
        const std::string f = (scratch() / "conf2" / "actions.json").string();
        const std::vector<ac::Action> lib{{"Drawn", {box, add, ell, tri}}};
        check("actions: drawn selections kept in the library and read back", ac::save(f, lib) && ac::load(f) == lib, "");
    }
    // ── s095: the Magic Wand and Object Select recorded ──────────────────
    {
        using SP = core::select::Pt;
        Doc t;
        const auto W = t.d.image().width, H = t.d.image().height;
        core::Document::Id picked = "L1";
        std::string err;
        // A red box on the picked layer, nothing selected after.
        const auto box = ac::select_shape(ac::Drawn::Rect, core::select::Op::Replace, {SP{0.25, 0.25}, SP{0.5, 0.5}});
        const bool made = ac::run(t.j, t.d, picked, box, err) && ac::run(t.j, t.d, picked, ac::fill_selection(red), err) &&
                          ac::run(t.j, t.d, picked, ac::select_none(), err);
        const auto wand = ac::select_wand(0.375, 0.375, 0, true, false, core::select::Op::Replace);
        check("actions: a Wand click names itself", wand.verb == "select.wand" && ac::known("select.wand") && wand.label == "Magic Wand  0", wand.label);
        const bool ran = made && ac::run(t.j, t.d, picked, wand, err);
        const auto* sel = t.d.selection();
        check("actions: a Wand click played -- the red box selected, corner to corner",
              ran && sel && core::select::coverage(*sel, W / 4, H / 4) == 1.f && core::select::coverage(*sel, W / 2 - 1, H / 2 - 1) == 1.f &&
                  core::select::coverage(*sel, W * 3 / 8, H * 3 / 8) == 1.f, err);
        const auto off = ac::select_wand(1.5, 0.5, 32, true, false, core::select::Op::Replace);
        check("actions: a Wand click off this picture refused, in words", !ac::run(t.j, t.d, picked, off, err) && err == "the click is off this picture", err);
        const auto obj = ac::select_object(0.1, 0.1, 0.6, 0.5, core::select::Op::Add);
        check("actions: Object Select names itself -- its box's share, its Add",
              obj.verb == "select.object" && ac::known("select.object") && obj.label == "Object Select  50 × 40 % of the canvas (Add)", obj.label);
        if (!klimt::seg::available())
            check("actions: Object Select without ONNX Runtime -- refused, in words",
                  !ac::run(t.j, t.d, picked, obj, err) && err == "Klimt was built without ONNX Runtime", err);
        const std::string f = (scratch() / "conf3" / "actions.json").string();
        const std::vector<ac::Action> lib{{"Clicked", {wand, obj}}};
        check("actions: Wand and Object acts kept in the library and read back", ac::save(f, lib) && ac::load(f) == lib, "");
    }
    // ── s097: Crop recorded, in pixels ─────────────────────────────────────
    {
        Doc t;   // 600 x 400
        const auto size_of = [](const ac::Played& p) {
            const auto im = p.ok ? p.result.records.at(core::kImageId).image : nullptr;
            return im ? std::to_string(im->width) + " x " + std::to_string(im->height) : "(" + p.error + ")";
        };
        const auto c = ac::crop(100, 50, 300, 200, false, P::rgba(1, 1, 1));
        check("actions: Crop names itself -- its size and where", c.verb == "canvas.crop" && ac::known("canvas.crop") &&
              c.label == "Crop  300 × 200 px at 100, 50", c.label);
        const auto p = ac::play(core::snapshot::take(t.d, "s"), "L1", {"Crop", {c}});
        check("actions: Crop played -- 300 x 200", size_of(p) == "300 x 200", size_of(p));
        const auto past = ac::play(core::snapshot::take(t.d, "s"), "L1", {"Wide", {ac::crop(-50, 0, 700, 400, false, P::rgba(1, 1, 1))}});
        check("actions: Crop past the edge -- canvas added (700 x 400)", size_of(past) == "700 x 400", size_of(past));
        const auto whole = ac::play(core::snapshot::take(t.d, "s"), "L1", {"Whole", {ac::crop(0, 0, 600, 400, false, P::rgba(1, 1, 1))}});
        check("actions: Crop to the whole picture -- runs, nothing to do", whole.ok && size_of(whole) == "600 x 400", size_of(whole));
        const auto zero = ac::play(core::snapshot::take(t.d, "s"), "L1", {"Zero", {ac::crop(0, 0, 0, 10, false, P::rgba(1, 1, 1))}});
        check("actions: Crop with no width -- refused, in words", !zero.ok && !zero.error.empty(), zero.error);
        using SP = core::select::Pt;
        const auto box = ac::select_shape(ac::Drawn::Rect, core::select::Op::Replace, {SP{0.25, 0.25}, SP{0.75, 0.5}});
        const auto cs = ac::crop_selection(false, P::rgba(1, 1, 1));
        check("actions: Crop to Selection names itself", cs.verb == "canvas.crop_selection" && cs.label == "Crop to Selection", cs.label);
        const auto ps = ac::play(core::snapshot::take(t.d, "s"), "L1", {"Box", {box, cs}});
        check("actions: Crop to Selection played -- the box's size (300 x 100)", size_of(ps) == "300 x 100", size_of(ps));
        const auto none = ac::play(core::snapshot::take(t.d, "s"), "L1", {"None", {ac::select_none(), cs}});
        check("actions: Crop to Selection with nothing selected -- said",
              !none.ok && none.error.find("nothing is selected") != std::string::npos, none.error);
        const std::string f = (scratch() / "conf4" / "actions.json").string();
        const std::vector<ac::Action> lib{{"Crops", {c, cs}}};
        check("actions: Crop acts kept in the library and read back", ac::save(f, lib) && ac::load(f) == lib, "");
    }
    // ── s097: odd and even files apart ───────────────────────────────────
    {
        check("actions: parity -- the last number in the name",
              ac::parity_of("/x/scan_0013.tif") == ac::Parity::Odd && ac::parity_of("page 12.png") == ac::Parity::Even &&
                  ac::parity_of("/a1/cover.jpg") == ac::Parity::None && ac::parity_of("v2 page 7.png") == ac::Parity::Odd &&
                  ac::parity_of("photo.jp2") == ac::Parity::None && ac::parity_of("p0.png") == ac::Parity::Even, "");
        const auto in = scratch() / "pages", out = scratch() / "pages_out";
        fs::create_directories(in);
        write_png((in / "page_01.png").string(), 400, 200, P::rgba(0.2f, 0.4f, 0.8f));
        write_png((in / "page_02.png").string(), 400, 200, P::rgba(0.8f, 0.4f, 0.2f));
        write_png((in / "page_03.png").string(), 400, 200, P::rgba(0.2f, 0.8f, 0.4f));
        write_png((in / "cover.png").string(), 400, 200, P::rgba(0.5f, 0.5f, 0.5f));
        ac::Batch b;
        b.source = in.string();
        b.dest = out.string();
        b.kind = F::Kind::PNG;
        const ac::Action left{"Left", {ac::crop(0, 0, 150, 200, true, P::rgba(1, 1, 1))}};
        const ac::Action right{"Right", {ac::crop(250, 0, 150, 100, true, P::rgba(1, 1, 1))}};
        check("actions: odd / even -- every kind skipped: refused, saying how", !ac::refusal(b, ac::Split{}).empty(), ac::refusal(b, ac::Split{}));
        const ac::Action empty{"Empty", {}};
        check("actions: odd / even -- an action with no steps: refused", !ac::refusal(b, ac::Split{&left, &empty, nullptr}).empty(), "");
        const ac::Split split{&left, &right, nullptr};
        check("actions: odd / even -- runs", ac::refusal(b, split).empty(), ac::refusal(b, split));
        const auto r = ac::run_batch(b, split);
        std::string err;
        const auto o1 = core::file::open((out / "page_01.png").string(), err);
        const auto e2 = core::file::open((out / "page_02.png").string(), err);
        const auto o3 = core::file::open((out / "page_03.png").string(), err);
        check("actions: odd / even -- odd pages cropped left, even right, the cover left alone",
              r.done == 3 && r.left == 1 && r.failed == 0 && r.lines.empty() && o1 && e2 && o3 && o1->image.width == 150 &&
                  o1->image.height == 200 && o3->image.width == 150 && e2->image.width == 150 && e2->image.height == 100 &&
                  !fs::exists(out / "cover.png"),
              std::to_string(r.done) + " done, " + std::to_string(r.left) + " left");
        const ac::Split all{&left, &left, &left};
        check("actions: odd / even -- lanes as for one action", ac::batch_lanes(all, 25) == ac::batch_lanes(left, 25), "");
    }
}
