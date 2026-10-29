// klimt_selftest -- FILTER LAYERS (s030): a filter over what's below, non-
// destructive; its opacity, mask, clip; stacked; following what's below;
// the verbs, the file, undo.
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/File.hpp"
#include "core/Live.hpp"

#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
std::string num(double v) { return std::to_string(v); }
// The fixture: white Background; Group (Pass Through) holding A, a red
// square (100, 100) 200 x 150; Top, empty. 600 x 400 RGB.
core::Sampled at(const core::Document& d, int x, int y) { return core::sample_composite(d.image(), d.snapshot(), x, y); }
bool pure(const core::Sampled& s, float r, float g, float b) { return std::abs(s.r - r) < 0.01f && std::abs(s.g - g) < 0.01f && std::abs(s.b - b) < 0.01f; }
struct Doc {
    core::Document d;
    core::DocJournal j{d};
    Doc() { d.set_door([this] { return j.open(); }); L::load_fixture(d); core::live::clear_cache(); }
    void with(const core::Document::Id& id, const std::function<void(core::Layer&)>& f) {
        core::Document::Loading load(d);
        auto l = *d.get(id);
        f(l);
        d.put(id, l);
    }
};
}  // namespace

void selftest_filterlayers(const Check& check) {
    std::cout << "\n-- filter layers (s030) --\n";
    {
        Doc t;
        check("before: the square's edge is sharp", pure(at(t.d, 101, 175), 1, 0, 0) && pure(at(t.d, 98, 175), 1, 1, 1), "");
        const auto pixels_before = t.d.get("L4")->pixels;
        core::Filter f = core::filter::fresh(core::FilterKind::GaussianBlur);
        f.radius = 6;
        const auto id = L::add_filter_layer(t.j, t.d, f);
        check("add: a step, a filter layer at the top", !id.empty() && t.d.get(id)->kind == core::Kind::Filter && t.j.undo_label().find("Gaussian Blur 1") != std::string::npos,
              t.j.undo_label());
        check("add: it has a mask (Reveal All), no pixels", t.d.get(id)->mask && !t.d.get(id)->pixels, "");
        const auto e1 = at(t.d, 98, 175), e2 = at(t.d, 102, 175);
        check("blur layer: the edge is soft -- outside pink, inside lighter", e1.g < 0.95f && e1.r > 0.95f && e2.g > 0.05f,
              num(e1.g) + " / " + num(e2.g));
        check("blur layer: far from the edge, as it was", pure(at(t.d, 30, 30), 1, 1, 1) && pure(at(t.d, 200, 175), 1, 0, 0), "");
        check("non-destructive: the pixels below untouched", t.d.get("L4")->pixels == pixels_before, "");
        check("a filter layer has no pixels to paint", L::not_pixels(t.d, id).find("filter layer") != std::string::npos, L::not_pixels(t.d, id));
        // Opacity 0: as if it weren't there; 50 %: half-way.
        const float full = at(t.d, 98, 175).g;
        L::set_opacity(t.j, t.d, id, 50);
        const float half = at(t.d, 98, 175).g;
        check("opacity 50 %: half the change", std::abs(half - (1 + full) / 2) < 0.03f, num(half) + " vs " + num(full));
        L::set_opacity(t.j, t.d, id, 0);
        check("opacity 0: no change", pure(at(t.d, 98, 175), 1, 1, 1), "");
        L::set_opacity(t.j, t.d, id, 100);
        // Following what's below: move the square's colour -- the blur follows.
        t.with("L4", [](core::Layer& l) { l.pixels = std::make_shared<const P::TileMap>(P::TileMap(l.pixels->format(), 600, 400).fill({100, 100, 200, 150}, P::rgba(0, 0, 1, 1))); });
        const auto b = at(t.d, 98, 175);
        check("it follows the layers below (blue now)", b.b > 0.95f && b.g < 0.95f && b.r < 0.95f, num(b.r));
        // Changed tiles: any change with a filter layer redraws everything.
        const auto before = t.d.snapshot();
        auto after = before;
        after.at("L5").opacity = 50;
        check("changed tiles: everything, with a filter layer", core::changed_tiles(t.d.image(), before, t.d.image(), after).all, "");
        after = before;
        after.at("L5").name = "renamed";
        check("changed tiles: a rename alone, nothing", core::changed_tiles(t.d.image(), before, t.d.image(), after).count() == 0, "");
        // The step.
        t.j.undo(); t.j.undo(); t.j.undo(); t.j.undo();
        check("undo: the layer gone, the edge sharp again", !t.d.get(id) && pure(at(t.d, 98, 175), 1, 1, 1), "");
    }
    {
        // Its mask: hidden on the left -- the left edge sharp, the right soft.
        Doc t;
        L::load_filter_layer(t.d, core::FilterKind::GaussianBlur);
        t.with("L6", [](core::Layer& l) {
            auto f = l.filters[0].f; f.radius = 6; l.filters[0].f = f;
            l.mask = std::make_shared<const P::TileMap>(P::TileMap(l.mask->format(), 600, 400).fill({0, 0, 200, 400}, [&] { P::Px p; p.c[0] = 1; return p; }()));
        });
        check("mask: hidden where it hides", pure(at(t.d, 98, 175), 1, 1, 1), "");
        check("mask: shows where it shows", at(t.d, 302, 175).g < 0.95f, num(at(t.d, 302, 175).g));
    }
    {
        // Clipped to A: it filters A alone, inside A's shape -- outside A, untouched.
        // (Unclipped, High Pass would grey the white Background too.)
        Doc t;
        {
            core::Document::Loading load(t.d);
            core::Layer l;
            l.kind = core::Kind::Filter;
            l.name = "Blur";
            l.parent = "L2";
            l.pos = 3;
            l.clipped = true;
            core::LiveFilter lf;
            lf.f = core::filter::fresh(core::FilterKind::HighPass);   // flat areas go middle grey
            l.filters = {lf};
            t.d.put("L6", l);
        }
        check("clipped: inside its base, the base filtered (High Pass: grey)", std::abs(at(t.d, 200, 175).r - 0.5f) < 0.03f && std::abs(at(t.d, 200, 175).g - 0.5f) < 0.03f,
              num(at(t.d, 200, 175).r));
        check("clipped: outside its base, nothing", pure(at(t.d, 30, 30), 1, 1, 1) && pure(at(t.d, 98, 175), 1, 1, 1), "");
    }
    {
        // Stacked: blur over blur spreads further than one.
        Doc t;
        core::Filter f = core::filter::fresh(core::FilterKind::GaussianBlur);
        f.radius = 4;
        L::add_filter_layer(t.j, t.d, f);
        const float one = at(t.d, 92, 175).g;
        L::add_filter_layer(t.j, t.d, f);
        const float two = at(t.d, 92, 175).g;
        check("stacked: the second reads the first", two < one - 0.005f, num(one) + " -> " + num(two));
        check("stacked: named in turn", t.j.undo_label().find("Gaussian Blur 2") != std::string::npos, t.j.undo_label());
    }
    {
        // High Pass in Overlay -- the classic sharpening: flat areas unchanged.
        Doc t;
        core::Filter f = core::filter::fresh(core::FilterKind::HighPass);
        const auto id = L::add_filter_layer(t.j, t.d, f);
        L::set_blend(t.j, t.d, id, P::Blend::Overlay);
        check("High Pass in Overlay: flat areas as they were", pure(at(t.d, 30, 30), 1, 1, 1) && pure(at(t.d, 200, 175), 1, 0, 0), "");
        // Its settings: a press is a step; a drag is one step.
        f.radius = 3;
        check("set: a step", L::set_filter_layer(t.j, t.d, id, f) && t.d.get(id)->filters[0].f.radius == 3, "");
        check("set: a different kind refused", !L::set_filter_layer(t.j, t.d, id, core::filter::fresh(core::FilterKind::Median)), "");
        check("set: refused on a pixel layer", !L::set_filter_layer(t.j, t.d, "L4", f), "");
        // The file.
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_flayer_" + std::to_string(::getpid()) + ".klimt")).string();
        core::file::Contents c;
        c.image = t.d.image(); c.layers = t.d.snapshot(); c.next_id = t.d.next_id();
        std::string err;
        const bool saved = core::file::save_klimt(path, c, err);
        const auto back = core::file::open(path, err);
        fs::remove(path);
        check("file: a filter layer kept", saved && back && back->layers.at(id).kind == core::Kind::Filter &&
                                           back->layers.at(id).filters == t.d.get(id)->filters && back->layers.at(id).blend == P::Blend::Overlay, err);
    }
    {
        // Scott, s030: the Background INSIDE a Pass Through group, a filter
        // layer above it in the group -- it reads the Background (it saw
        // nothing: below() hid the group itself).
        Doc t;
        {
            core::Document::Loading load(t.d);
            auto bg = *t.d.get("L1");
            bg.parent = "L2";
            bg.pos = 0.5;
            t.d.put("L1", bg);
        }
        check("setup: the group is Pass Through", t.d.get("L2")->blend == P::Blend::PassThrough, "");
        const auto hid = core::below(t.d.snapshot(), "L4");
        check("below(): the enclosing group stays visible", hid.at("L2").visible && hid.at("L1").visible && hid.at("L3").visible && !hid.at("L4").visible && !hid.at("L5").visible, "");
        core::Filter f = core::filter::fresh(core::FilterKind::GaussianBlur);
        f.radius = 6;
        core::Document::Id id;
        {
            core::Document::Loading load(t.d);
            core::Layer l;
            l.kind = core::Kind::Filter;
            l.name = "Blur";
            l.parent = "L2";
            l.pos = 3;
            core::LiveFilter lf;
            lf.f = f;
            l.filters = {lf};
            t.d.put("L6", l);
            id = "L6";
        }
        const auto e = at(t.d, 98, 175);
        check("in a Pass Through group with the Background inside: it blurs", e.g < 0.95f && e.r > 0.95f, num(e.g));
    }
}
