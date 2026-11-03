#include "Shell.hpp"

#include <adwaita.h>
#include "Log.hpp"
#include "core/Selection.hpp"
#include "core/Tools.hpp"
#include "Registry.hpp"

#include <glibmm/main.h>
#include <gtkmm/adjustment.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/window.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <sstream>

// Shell_sweep_canvas.cpp -- THE CANVAS SWEEP (s091; docs/regression.md).
// `scripts/sweep_canvas.sh` runs `KLIMT_SWEEP=canvas build/klimt` on a
// private Xvfb: the demo, then a scene for each tool -- a stroke with each
// paint tool, each Select variant, Move, Crop, Free Transform (and its
// kinds), Warp, Liquify, Text, the Pen. The app can't press its own mouse in
// GTK 4, so it writes what to press to $KLIMT_FINGERS ("N drag x y x y ...
// mods", window coordinates) and the script's xdotool does it -- the real
// path, gesture to step -- then acks N in $KLIMT_FINGERS.ack. Each scene says
// whether a step should land; the log names the step (or says none came),
// takes a screenshot, and the scene is undone so the next finds the demo as
// it was. A crash, a GLib warning or a hang names its scene.

namespace klimt {

namespace {
using B = Shell::SweepBeat;
using K = Shell::SweepBeat::K;
B tool(const std::string& id, int variant = 0) { return {K::Tool, id, variant, {}, {}}; }
B action(const std::string& a) { return {K::Action, a, 0, {}, {}}; }
B widget(const std::string& name) { return {K::Widget, name, 0, {}, {}}; }
B drag(std::vector<std::array<double, 2>> pts, const std::string& mods = {}) { return {K::Drag, {}, 0, std::move(pts), mods}; }
B click(double x, double y, int n = 1, const std::string& mods = {}) { return {K::Click, {}, n, {{x, y}}, mods}; }
B key(const std::string& k) { return {K::Key, k, 0, {}, {}}; }
B type(const std::string& t) { return {K::Type, t, 0, {}, {}}; }
// s092: the panels. A row of a named list (x: a fraction of its width -- the
// eye at the left, the name further in), clicked n times; a row dragged onto
// another; a named slider dragged from one fraction of its width to another.
B row(const std::string& list, int i, double x = 0.6, int n = 1, const std::string& mods = {}) { return {K::Row, list, i, {{x, static_cast<double>(n)}}, mods}; }
// s093: the k-th button (or check box) inside row i of a named list -- a
// colour channel's Load / Duplicate, an action's arrow, a step's tick.
B row_button(const std::string& list, int i, int k) { return {K::RowButton, list, i, {{static_cast<double>(k), 0}}, {}}; }
// s093: the dock's tabs -- 0 Layers, 1 Channels, 2 History, 3 Actions.
B tab(int i) { return {K::Row, "dock.tabs", i, {{0.5, 1}}, {}}; }
B row_drag(const std::string& list, int from, int to) { return {K::RowDrag, list, from, {{0.6, static_cast<double>(to)}}, {}}; }
// s092: the setup is done -- the scene's expected step is counted from here.
B mark() { return {K::Mark, {}, 0, {}, {}}; }
B slide(const std::string& name, double x0, double x1) { return {K::Slide, name, 0, {{x0, 0.5}, {x1, 0.5}}, {}}; }

std::vector<Shell::SweepScene> scenes() {
    const std::vector<std::array<double, 2>> stroke = {{0.25, 0.35}, {0.35, 0.42}, {0.45, 0.45}, {0.55, 0.42}, {0.65, 0.35}};
    const std::vector<std::array<double, 2>> box = {{0.3, 0.3}, {0.5, 0.5}, {0.7, 0.7}};
    const std::vector<std::array<double, 2>> loop = {{0.3, 0.3}, {0.7, 0.32}, {0.65, 0.7}, {0.35, 0.65}, {0.31, 0.33}};
    const std::vector<std::array<double, 2>> nudge = {{0.5, 0.5}, {0.52, 0.53}, {0.55, 0.56}};
    return {
        // ── paint ──
        {"brush", true, {tool("brush"), drag(stroke)}},
        {"brush, Shift a straight line", true, {tool("brush"), click(0.2, 0.2), click(0.7, 0.6, 1, "shift")}},
        {"eraser", true, {tool("eraser"), drag(stroke)}},
        {"smudge", true, {tool("smudge"), drag(stroke)}},
        {"clone stamp (Alt-click a source)", true, {tool("clone"), click(0.2, 0.2, 1, "alt"), drag(stroke)}},
        {"heal: spot", true, {tool("heal", 0), drag(stroke)}},
        {"heal: healing (Alt-click a source)", true, {tool("heal", 1), click(0.2, 0.2, 1, "alt"), drag(stroke)}},
        {"heal: patch (a selection dragged)", true, {tool("select", 0), drag(box), mark(), tool("heal", 2), drag({{0.5, 0.5}, {0.45, 0.4}, {0.4, 0.3}})}},
        {"gradient: linear", true, {tool("gradient", 0), drag({{0.2, 0.5}, {0.5, 0.5}, {0.8, 0.5}})}},
        {"gradient: radial", true, {tool("gradient", 1), drag({{0.5, 0.5}, {0.6, 0.55}, {0.7, 0.6}})}},
        // ── select ──
        {"select: rectangle", true, {tool("select", 0), drag(box)}},
        {"select: rectangle, Shift adds", true, {tool("select", 0), drag(box), mark(), drag({{0.1, 0.1}, {0.2, 0.15}, {0.25, 0.2}}, "shift")}},
        {"select: ellipse", true, {tool("select", 1), drag(box)}},
        {"select: lasso", true, {tool("select", 2), drag(loop)}},
        {"select: wand", true, {tool("select", 3), click(0.5, 0.5)}},
        {"select: colour range", false, {tool("select", 4), click(0.5, 0.5)}},
        {"select: magnetic", true, {tool("select", 5), click(0.3, 0.3), click(0.7, 0.3), click(0.6, 0.7), key("Return")}},
        {"select: object (needs its model)", false, {tool("select", 6), drag(box)}},
        {"refine brush (on a selection)", true, {tool("select", 0), drag(box), mark(), tool("refine"), drag({{0.3, 0.5}, {0.5, 0.5}, {0.7, 0.5}})}},
        // ── move / crop ──
        {"move", true, {tool("move"), drag(nudge)}},
        {"crop: a box, Enter", true, {tool("crop", 0), drag({{0.1, 0.1}, {0.5, 0.5}, {0.85, 0.85}}), key("Return")}},
        {"crop: a box, double-click inside", true, {tool("crop", 0), drag({{0.1, 0.1}, {0.5, 0.5}, {0.85, 0.85}}), click(0.5, 0.5, 2)}},
        {"crop: straighten", true, {tool("crop", 1), drag({{0.2, 0.5}, {0.5, 0.52}, {0.8, 0.55}})}},
        // ── transform ──
        {"free transform: moved, Enter", true, {tool("move"), action("free-transform"), drag(nudge), key("Return")}},
        {"free transform: a corner, Apply", true, {tool("move"), action("free-transform"), widget("strip.transform.free"), drag({{0.999, 0.999}, {0.95, 0.95}, {0.9, 0.9}}), widget("strip.transform.apply")}},
        {"free transform: skew", true, {tool("move"), action("free-transform"), widget("strip.transform.skew"), drag({{0.999, 0.5}, {0.999, 0.55}, {0.999, 0.6}}), key("Return")}},
        {"free transform: distort", true, {tool("move"), action("free-transform"), widget("strip.transform.distort"), drag({{0.999, 0.999}, {0.97, 0.95}, {0.93, 0.9}}), key("Return")}},
        {"free transform: perspective", true, {tool("move"), action("free-transform"), widget("strip.transform.perspective"), drag({{0.999, 0.001}, {0.97, 0.03}, {0.93, 0.05}}), key("Return")}},
        {"free transform: Esc leaves nothing", false, {tool("move"), action("free-transform"), drag(nudge), key("Escape")}},
        {"warp: inside bent, Enter", true, {tool("move"), action("free-transform"), widget("strip.transform.warp"), drag({{0.5, 0.5}, {0.55, 0.45}, {0.6, 0.4}}), key("Return")}},
        {"liquify: forward, Enter", true, {action("liquify"), drag(stroke), key("Return")}},
        {"liquify: Esc leaves nothing", false, {action("liquify"), drag(stroke), key("Escape")}},
        // ── type / pen ──
        {"text: a click, typed, Esc", true, {tool("text"), click(0.3, 0.3), type("Sweep"), key("Escape")}},
        {"pen: three anchors, closed", true, {tool("pen", 0), click(0.3, 0.3), click(0.7, 0.3), click(0.5, 0.7), click(0.3, 0.3)}},
        {"pen: anchors, Enter", true, {tool("pen", 0), click(0.3, 0.6), drag({{0.6, 0.6}, {0.65, 0.5}, {0.7, 0.4}}), key("Return")}},
        // ── the panels (s092) ──
        {"layers: a click picks the row below", false, {action("layer-duplicate"), action("layer-duplicate"), mark(), row("layers.list", 1)}},
        {"layers: the eye hides a layer", true, {action("layer-duplicate"), action("layer-duplicate"), mark(), row("layers.list", 1, 0.06)}},
        {"layers: a row dragged two down", true, {action("layer-duplicate"), action("layer-duplicate"), mark(), row_drag("layers.list", 0, 2)}},
        {"layers: double-click renames, typed, Enter", true, {action("layer-duplicate"), action("layer-duplicate"), mark(), row("layers.list", 1, 0.6, 2), key("ctrl+a"), type("Swept"), key("Return")}},
        {"properties: opacity dragged", true, {slide("properties.opacity", 0.9, 0.35)}},
        {"properties: fill dragged", true, {slide("properties.fill", 0.9, 0.5)}},
        {"properties: lock alpha", true, {widget("properties.lock.alpha")}},
        {"properties: blend picked from the list", true, {widget("properties.blend"), key("End"), key("Return")}},
        {"history: a stroke, then the row before it", false, {tool("brush"), drag(stroke), tab(2), row("history.list", 0)}},
        // ── channels (s093) ──
        {"channels: the selection saved", true, {tool("select", 0), drag(box), mark(), tab(1), widget("dock.channels.save")}, "channels=1"},
        {"channels: a saved one loaded", true, {tool("select", 0), drag(box), tab(1), widget("dock.channels.save"), action("deselect"), mark(),
                                                row("dock.channels.list", 0)}},
        {"channels: double-click renames, typed, Enter", true, {tool("select", 0), drag(box), tab(1), widget("dock.channels.save"), mark(),
                                                                row("dock.channels.list", 0, 0.5, 2), key("ctrl+a"), type("Swept"), key("Return")}, "channel0=Swept"},
        {"channels: a saved one deleted", true, {tool("select", 0), drag(box), tab(1), widget("dock.channels.save"), mark(),
                                                 row("dock.channels.list", 0), widget("dock.channels.remove")}, "channels=0"},
        {"channels: the red one viewed", false, {tab(1), row("dock.channels.colour", 1, 0.3)}, "viewing=0"},
        {"channels: Ctrl-click red loads it", true, {tab(1), row("dock.channels.colour", 1, 0.3, 1, "ctrl")}},
        {"channels: red duplicated", true, {tab(1), row_button("dock.channels.colour", 1, 1)}, "channels=1"},
        // ── paths: a path layer's row and its Properties (s093; the Paths tab went at m4) ──
        {"paths: the outline hidden", false, {tool("pen", 0), click(0.3, 0.3), click(0.7, 0.3), click(0.5, 0.7), click(0.3, 0.3), mark(),
                                              widget("layers.path_outline")}, "outline-off"},
        {"paths: mask below switched on", true, {tool("pen", 0), click(0.3, 0.3), click(0.7, 0.3), click(0.5, 0.7), click(0.3, 0.3), mark(),
                                                 widget("properties.path.mask")}},
        {"paths: be the selection", true, {tool("pen", 0), click(0.3, 0.3), click(0.7, 0.3), click(0.5, 0.7), click(0.3, 0.3), mark(),
                                           widget("properties.path.select")}},
        {"paths: loaded as a selection", true, {tool("pen", 0), click(0.3, 0.3), click(0.7, 0.3), click(0.5, 0.7), click(0.3, 0.3), mark(),
                                                widget("properties.path.load")}},
        {"paths: Ctrl+Enter loads it", true, {tool("pen", 0), click(0.3, 0.3), click(0.7, 0.3), click(0.5, 0.7), click(0.3, 0.3), mark(),
                                              key("ctrl+Return")}},
        {"paths: isolate inside it", true, {tool("pen", 0), click(0.2, 0.2), click(0.8, 0.2), click(0.8, 0.8), click(0.2, 0.8), click(0.2, 0.2), mark(),
                                            widget("properties.path.isolate")}},
        {"paths: a work path from the selection", true, {tool("select", 0), drag(box), mark(), action("path-make-work")}},
        // ── actions (s093; the library is put back after each) ──
        {"actions: new, Select All recorded, stop", true, {tab(3), widget("actions.new"), action("select-all"), widget("actions.stop")}, "actions+1"},
        {"actions: a drawn box recorded (s094)", true, {tab(3), widget("actions.new"), tool("select", 0), drag(box), widget("actions.stop")}, "recorded=select.shape"},
        {"actions: a drawn lasso recorded (s094)", true, {tab(3), widget("actions.new"), tool("select", 2), drag(loop), widget("actions.stop")}, "recorded=select.shape"},
        {"actions: a Wand click recorded (s095)", true, {tab(3), widget("actions.new"), tool("select", 3), click(0.5, 0.5), widget("actions.stop")}, "recorded=select.wand"},
        {"actions: a starter played", true, {tab(3), row("actions.list", 0, 0.95), widget("actions.play")}},
        {"actions: a step switched off", false, {tab(3), row_button("actions.list", 0, 0), row_button("actions.list", 1, 0)}, "step0off"},
        {"actions: double-click renames, typed, Enter", false, {tab(3), row("actions.list", 0, 0.4, 2), key("ctrl+a"), type("Swept"), key("Return")}, "action0=Swept"},
        {"actions: one deleted", false, {tab(3), row("actions.list", 2, 0.95), widget("actions.delete")}, "actions-1"},
        {"actions: Batch opens its dialog", false, {tab(3), row("actions.list", 0, 0.95), widget("actions.batch")}, "window"},
        // ── sample / view (no step) ──
        {"eyedropper", false, {tool("eyedropper"), click(0.5, 0.5)}},
        {"hand", false, {tool("hand"), drag(nudge)}},
        {"zoom: a click in", false, {tool("zoom", 0), click(0.5, 0.5)}},
        {"zoom: a box", false, {tool("zoom", 0), drag(box)}},
    };
}
}  // namespace

bool Shell::finger(const std::string& line) {
    if (m_fingers.empty()) return false;
    std::ofstream f(m_fingers, std::ios::app);
    f << ++m_finger_n << ' ' << line << '\n';
    return static_cast<bool>(f);
}

// A point of a widget (its own coordinates) in the window's surface -- what
// xdotool's --window means (the CSD shadow included).
bool Shell::widget_point(Gtk::Widget& w, double x, double y, double& wx, double& wy) {
    graphene_point_t in = GRAPHENE_POINT_INIT(static_cast<float>(x), static_cast<float>(y)), out;
    if (!gtk_widget_compute_point(w.gobj(), GTK_WIDGET(gobj()), &in, &out)) return false;
    double sx = 0, sy = 0;
    gtk_native_get_surface_transform(GTK_NATIVE(gobj()), &sx, &sy);
    wx = out.x + sx;
    wy = out.y + sy;
    return true;
}

// s093: a widget by name -- the registry first, else the window's tree; on show only.
Gtk::Widget* Shell::sweep_named(const std::string& name) {
    if (auto* r = registry::find(name); r && r->get_mapped()) return r;
    Gtk::Widget* w = nullptr;
    std::function<void(Gtk::Widget&)> find = [&](Gtk::Widget& p) {
        if (w) return;
        if (std::string(p.get_name()) == name && p.get_mapped()) { w = &p; return; }
        for (auto* c = p.get_first_child(); c; c = c->get_next_sibling()) find(*c);
    };
    find(*this);
    return w;
}

// s093: a widget below its panel's fold (Properties is tall for a path layer)
// is scrolled into view -- xdotool would press whatever lies over it.
bool Shell::sweep_reveal(Gtk::Widget& w) {
    for (auto* p = w.get_parent(); p; p = p->get_parent()) {
        auto* sw = dynamic_cast<Gtk::ScrolledWindow*>(p);
        if (!sw) continue;
        graphene_point_t in = GRAPHENE_POINT_INIT(0.f, 0.f), out;
        if (!gtk_widget_compute_point(w.gobj(), GTK_WIDGET(sw->gobj()), &in, &out)) return false;
        const double top = out.y, bottom = out.y + w.get_height(), h = sw->get_height();
        if (top >= 0 && bottom <= h) return false;
        auto adj = sw->get_vadjustment();
        adj->set_value(adj->get_value() + top - h / 3);
        return true;
    }
    return false;
}

// s093: what a scene asks to hold beyond its step.
std::string Shell::canvas_check(const std::string& what) {
    const auto n = m_actions.size(), was = m_sweep_actions_n;
    if (what == "actions+1") {
        if (n != was + 1) return "the library has " + std::to_string(n) + " actions (was " + std::to_string(was) + ")";
        if (m_actions.back().acts.empty()) return "the new action recorded nothing";
        if (m_recording >= 0) return "still recording";
        return {};
    }
    if (what.rfind("recorded=", 0) == 0) {   // s094: the new action's last act is this verb
        if (n != was + 1 || m_actions.back().acts.empty()) return "nothing recorded (" + std::to_string(n) + " actions, was " + std::to_string(was) + ")";
        const auto& v = m_actions.back().acts.back().verb;
        return v == what.substr(9) ? std::string{} : "the last act recorded is \"" + v + "\"";
    }
    if (what == "actions-1") return n + 1 == was ? std::string{} : "the library has " + std::to_string(n) + " actions (was " + std::to_string(was) + ")";
    if (what == "action0=Swept") return !m_actions.empty() && m_actions[0].name == "Swept" ? std::string{} : "the first action is \"" + (m_actions.empty() ? std::string{} : m_actions[0].name) + "\"";
    if (what == "step0off") return !m_actions.empty() && !m_actions[0].acts.empty() && !m_actions[0].acts[0].on ? std::string{} : "the first action's first step is still on";
    if (what.rfind("channels=", 0) == 0) {
        const auto want = std::stoul(what.substr(9)), have = m_doc.channel_order().size();
        return have == want ? std::string{} : std::to_string(have) + " saved channels, not " + std::to_string(want);
    }
    if (what == "channel0=Swept") {
        const auto& o = m_doc.channel_order();
        const auto l = o.empty() ? decltype(m_doc.get(o.front())){} : m_doc.get(o.front());
        return l && l->name == "Swept" ? std::string{} : "the saved channel is \"" + (l ? l->name : std::string{}) + "\"";
    }
    if (what == "viewing=0") return m_channel_view == 0 ? std::string{} : "viewing channel " + std::to_string(m_channel_view);
    if (what == "outline-off") {
        const auto id = path_layer();
        return !id.empty() && m_outlines_off.count(id) > 0 ? std::string{} : "the outline still shows";
    }
    if (what == "window") {
        for (auto* w : Gtk::Window::list_toplevels()) if (w != this && w->get_visible()) return {};
        return "no dialog on show";
    }
    return "unknown check \"" + what + "\"";
}

void Shell::start_canvas_sweep() {
    sweep_open([this] { start_canvas_run(); });
}

void Shell::start_canvas_run() {
    const char* f = std::getenv("KLIMT_FINGERS");
    m_fingers = f ? f : "";
    auto lg = log::get(log::Area::Shell);
    if (m_fingers.empty()) {
        if (lg) lg->warn("sweep: canvas -- no KLIMT_FINGERS (run scripts/sweep_canvas.sh)");
        Glib::signal_timeout().connect_once([this] { m_closing = true; close(); }, 500);
        return;
    }
    m_scenes = scenes();
    if (const char* only = std::getenv("KLIMT_SWEEP_ONLY")) {
        std::vector<SweepScene> keep;
        for (auto& s : m_scenes) if (s.name.find(only) != std::string::npos) keep.push_back(std::move(s));
        m_scenes = std::move(keep);
    }
    m_scene_at = m_beat_at = 0;
    m_scene_bad = 0;
    m_finger_n = 0;
    m_sweep_toasts = true;   // s093: a toast left up covers the dock's foot (Channels' Save) -- dismissed between scenes
    m_sweep_actions = m_actions;   // s093: put back after every scene (and the file with it)
    m_sweep_actions_n = m_actions.size();
    if (lg) lg->info("sweep: canvas -- {} scenes", m_scenes.size());
    // The window first, so the script knows where to press.
    Glib::signal_timeout().connect_once([this] {
        unsigned long xid = 0;
#ifdef GDK_WINDOWING_X11
        // s091 (Scott's GTK: the X11 backend API is deprecated -- a warning):
        // the canvas sweep is X11 by nature (Xvfb + xdotool), so the one call
        // is fenced, not replaced.
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        if (auto* s = gtk_native_get_surface(GTK_NATIVE(gobj())); s && GDK_IS_X11_SURFACE(s)) xid = gdk_x11_surface_get_xid(s);
        G_GNUC_END_IGNORE_DEPRECATIONS
#endif
        if (!xid) {
            if (auto lg = log::get(log::Area::Shell)) lg->warn("sweep: canvas -- not an X11 window (GDK_BACKEND=x11)");
            m_closing = true;
            close();
            return;
        }
        finger("window " + std::to_string(xid));
        m_sweep_mark = m_journal.mark();
        canvas_step();
    }, 1200);
}

// Wait for the script's ack (and any job), then the next beat.
void Shell::canvas_step() {
    auto lg = log::get(log::Area::Shell);
    int acked = 0;
    { std::ifstream a(m_fingers + ".ack"); a >> acked; }
    if (acked < m_finger_n || m_jobs.active() > 0) {
        if (++m_canvas_waits < 300) { Glib::signal_timeout().connect_once([this] { canvas_step(); }, 100); return; }
        if (lg) lg->warn("sweep:   STILL WAITING after 30 s ({}) -- cancelled", acked < m_finger_n ? "no ack from the script" : "a job");
        ++m_scene_bad;
        m_jobs.cancel_all();
        m_finger_n = acked;
    }
    m_canvas_waits = 0;
    Glib::signal_timeout().connect_once([this] { canvas_beat(); }, 250);   // the step lands, the canvas redraws
}

void Shell::canvas_beat() {
    auto lg = log::get(log::Area::Shell);
    if (m_scene_at >= m_scenes.size()) {
        if (lg) lg->info("sweep: canvas done -- {} scenes, {} not as expected", m_scenes.size(), m_scene_bad);
        finger("done");
        Glib::signal_timeout().connect_once([this] { m_closing = true; close(); }, 500);
        return;
    }
    const auto& sc = m_scenes[m_scene_at];
    if (m_beat_at == 0) {
        if (!has_image()) { sweep_open([this] { canvas_beat(); }); return; }   // a scene closed the picture
        m_sweep_mark = m_expect_mark = m_journal.mark();
        if (lg) lg->info("sweep: canvas [{}/{}] {}", m_scene_at + 1, m_scenes.size(), sc.name);
    }
    if (m_beat_at >= sc.beats.size()) {   // the scene's end: what landed, a picture, then undone
        const bool landed = m_journal.mark() != m_expect_mark;
        if (landed && lg) lg->info("sweep:   a step: \"{}\"", m_journal.undo_label());
        if (!landed && lg) lg->info("sweep:   no step");
        if (landed != sc.step) {
            ++m_scene_bad;
            if (lg) lg->warn("sweep:   NOT AS EXPECTED -- {}", sc.step ? "a step should have landed" : "a step landed that shouldn't");
        }
        if (!sc.check.empty()) {   // s093
            if (const auto bad = canvas_check(sc.check); !bad.empty()) {
                ++m_scene_bad;
                if (lg) lg->warn("sweep:   NOT AS EXPECTED -- {} ({})", bad, sc.check);
            } else if (lg) lg->info("sweep:   holds: {}", sc.check);
        }
        std::string shot = sc.name;
        for (auto& ch : shot) if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
        char num[8];
        std::snprintf(num, sizeof num, "%02zu_", m_scene_at + 1);
        finger("shot " + std::string(num) + shot);
        canvas_cleanup();
        ++m_scene_at;
        m_beat_at = 0;
        canvas_step();
        return;
    }
    // s093: a toast raised by this scene ("Free Transform: drag the box") can
    // lie over the next press -- dismissed, and its fade waited out, first.
    if (!m_toasts_up.empty()) {
        for (auto* t : m_toasts_up) { adw_toast_dismiss(t); g_object_unref(t); }
        m_toasts_up.clear();
        Glib::signal_timeout().connect_once([this] { canvas_beat(); }, 400);
        return;
    }
    const auto& b = sc.beats[m_beat_at++];
    // Image fractions -> window points (the view now; a zoom moves it).
    auto at = [this](const std::array<double, 2>& p, double& x, double& y) {
        const auto& im = m_doc.image();
        const double sx = m_canvas.screen_x_of(p[0] * im.width), sy = m_canvas.screen_y_of(p[1] * im.height);
        return widget_point(m_canvas, sx, sy, x, y);
    };
    std::ostringstream o;
    switch (b.k) {
    case K::Tool:
        m_rail.set_tool(b.s);
        on_tool(b.s);
        if (b.v) m_strip.set_variant(b.s, b.v);
        else if (const auto* t = core::find_tool(b.s); t && !t->variants.empty()) m_strip.set_variant(b.s, 0);
        canvas_step();
        return;
    case K::Variant:
        m_strip.set_variant(b.s, b.v);
        canvas_step();
        return;
    case K::Action:
        activate_action("win." + b.s);
        canvas_step();
        return;
    case K::Mark:
        m_expect_mark = m_journal.mark();
        canvas_step();
        return;
    case K::Widget: {
        Gtk::Widget* w = sweep_named(b.s);   // s092: the registry first (a name it knows); s093: one helper
        if (w && !m_revealing && sweep_reveal(*w)) {   // s093: scrolled into view first, then this beat again
            m_revealing = true;
            --m_beat_at;
            Glib::signal_timeout().connect_once([this] { canvas_beat(); }, 250);
            return;
        }
        m_revealing = false;
        double x = 0, y = 0;
        if (!w || !widget_point(*w, w->get_width() / 2.0, w->get_height() / 2.0, x, y)) {
            if (lg) lg->warn("sweep:   NO WIDGET \"{}\" on show", b.s);
            ++m_scene_bad;
            canvas_step();
            return;
        }
        o << "click 1 " << static_cast<int>(x) << ' ' << static_cast<int>(y) << " -";
        break;
    }
    case K::Row: case K::RowDrag: {   // s092
        auto* lb = dynamic_cast<Gtk::ListBox*>(sweep_named(b.s));   // s093: Channels' lists by their names too
        Gtk::Widget* r = lb && lb->get_mapped() ? lb->get_row_at_index(b.v) : nullptr;
        if (!lb && b.k == K::Row) {   // not a list: the i-th child of a named box (the dock's tabs)
            Gtk::Widget* box = nullptr;
            std::function<void(Gtk::Widget&)> find = [&](Gtk::Widget& p) {
                if (box) return;
                if (std::string(p.get_name()) == b.s && p.get_mapped()) { box = &p; return; }
                for (auto* c = p.get_first_child(); c; c = c->get_next_sibling()) find(*c);
            };
            find(*this);
            r = box ? box->get_first_child() : nullptr;
            for (int i = 0; r && i < b.v; ++i) r = r->get_next_sibling();
        }
        auto* r2 = lb && b.k == K::RowDrag ? lb->get_row_at_index(static_cast<int>(b.pts[0][1])) : nullptr;
        double x = 0, y = 0, x2 = 0, y2 = 0;
        if (!r || !r->get_mapped() || (b.k == K::RowDrag && (!r2 || !r2->get_mapped())) ||
            !widget_point(*r, r->get_width() * b.pts[0][0], r->get_height() / 2.0, x, y)) {
            if (lg) lg->warn("sweep:   NO ROW {} of \"{}\" on show", b.v, b.s);
            ++m_scene_bad;
            canvas_step();
            return;
        }
        if (b.k == K::Row) {
            o << "click " << static_cast<int>(b.pts[0][1]) << ' ' << static_cast<int>(x) << ' ' << static_cast<int>(y) << ' ' << (b.mods.empty() ? "-" : b.mods);
            break;
        }
        widget_point(*r2, r2->get_width() * 0.6, r2->get_height() * 0.75, x2, y2);   // its lower part: below it
        o << "drag - " << static_cast<int>(x) << ' ' << static_cast<int>(y) << ' ' << static_cast<int>(x) << ' ' << static_cast<int>(y + 12)
          << ' ' << static_cast<int>(x2) << ' ' << static_cast<int>(y2);
        break;
    }
    case K::RowButton: {   // s093
        auto* lb = dynamic_cast<Gtk::ListBox*>(sweep_named(b.s));
        Gtk::Widget* r = lb ? lb->get_row_at_index(b.v) : nullptr;
        Gtk::Widget* hit = nullptr;
        int k = static_cast<int>(b.pts[0][0]);
        std::function<void(Gtk::Widget&)> find = [&](Gtk::Widget& p) {
            if (hit) return;
            if ((dynamic_cast<Gtk::Button*>(&p) || dynamic_cast<Gtk::CheckButton*>(&p)) && p.get_mapped() && k-- == 0) { hit = &p; return; }
            for (auto* c = p.get_first_child(); c; c = c->get_next_sibling()) find(*c);
        };
        if (r && r->get_mapped()) find(*r);
        double x = 0, y = 0;
        if (!hit || !widget_point(*hit, hit->get_width() / 2.0, hit->get_height() / 2.0, x, y)) {
            if (lg) lg->warn("sweep:   NO BUTTON {} in row {} of \"{}\"", static_cast<int>(b.pts[0][0]), b.v, b.s);
            ++m_scene_bad;
            canvas_step();
            return;
        }
        o << "click 1 " << static_cast<int>(x) << ' ' << static_cast<int>(y) << " -";
        break;
    }
    case K::Slide: {   // s092
        auto* w = registry::find(b.s);
        double x = 0, y = 0, x2 = 0, y2 = 0;
        if (!w || !w->get_mapped() || !widget_point(*w, w->get_width() * b.pts[0][0], w->get_height() / 2.0, x, y)) {
            if (lg) lg->warn("sweep:   NO WIDGET \"{}\" on show", b.s);
            ++m_scene_bad;
            canvas_step();
            return;
        }
        widget_point(*w, w->get_width() * b.pts[1][0], w->get_height() / 2.0, x2, y2);
        o << "drag - " << static_cast<int>(x) << ' ' << static_cast<int>(y) << ' ' << static_cast<int>((x + x2) / 2) << ' ' << static_cast<int>(y)
          << ' ' << static_cast<int>(x2) << ' ' << static_cast<int>(y2);
        break;
    }
    case K::Drag: {
        o << "drag " << (b.mods.empty() ? "-" : b.mods);
        for (const auto& p : b.pts) { double x, y; at(p, x, y); o << ' ' << static_cast<int>(x) << ' ' << static_cast<int>(y); }
        break;
    }
    case K::Click: {
        double x, y;
        at(b.pts[0], x, y);
        o << "click " << b.v << ' ' << static_cast<int>(x) << ' ' << static_cast<int>(y) << ' ' << (b.mods.empty() ? "-" : b.mods);
        break;
    }
    case K::Key: o << "key " << b.s; break;
    case K::Type: o << "type " << b.s; break;
    case K::Shot: o << "shot " << b.s; break;
    }
    if (lg) lg->info("sweep:   {}", o.str());
    finger(o.str());
    canvas_step();
}

// Whatever a scene left open is shut without a step, then its steps undone.
void Shell::canvas_cleanup() {
    text_close();
    if (m_xform.active) xform_cancel();
    if (m_liq.active) liquify_cancel();
    if (m_canvas.lasso_open()) m_canvas.lasso_cancel();
    if (m_canvas.pen_drawing()) m_canvas.pen_finish();
    m_canvas.clear_crop();
    for (auto* w : Gtk::Window::list_toplevels())
        if (w != this && w->get_visible()) {
            if (auto lg = log::get(log::Area::Shell)) lg->info("sweep:   closed \"{}\"", std::string(w->get_title()));
            w->close();
        }
    while (m_journal.can_undo() && m_journal.mark() != m_sweep_mark) m_journal.undo();
    if (!m_sweep_layer.empty() && m_doc.get(m_sweep_layer)) m_layers.select({m_sweep_layer}, false);
    activate_action("win.zoom-fit");
    m_dock_stack.set_visible_child("layers");   // s092: a scene may have shown History
    // s093: the picture's own view again; the actions library as it was found.
    if (m_channel_view >= 0) on_channel_view(-1);
    for (auto* t : m_toasts_up) { adw_toast_dismiss(t); g_object_unref(t); }
    m_toasts_up.clear();
    m_recording = -1;
    m_rec_made.clear();
    m_actions = m_sweep_actions;
    m_actions_panel.pick(-1);
    m_actions_panel.close_all();
    save_actions();
    sync_actions();
    update_layer_actions();
}

}  // namespace klimt
