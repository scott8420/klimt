// klimt_selftest -- LAYERS BETWEEN DOCUMENTS (s022): take, paste (fresh ids,
// centred, converted), one step, refusals.
#include "core/Document.hpp"
#include "core/Transfer.hpp"
#include "core/File.hpp"
#include "core/Composite.hpp"

#include <filesystem>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <string>

namespace core = klimt::core;
namespace P = klimt::pixel;
namespace L = klimt::core::layers;
namespace T = klimt::core::transfer;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

namespace {
void blank(core::Document& d, int w, int h, P::Format f) {
    core::Document::Loading load(d);
    core::Image im;
    im.width = w;
    im.height = h;
    im.format = f;
    core::Layer bg;
    bg.kind = core::Kind::Pixel;
    bg.name = "Background";
    bg.pos = 1;
    bg.pixels = std::make_shared<const P::TileMap>(P::TileMap(f, w, h).fill({0, 0, w, h}, P::rgba(1, 1, 1, 1)));
    d.replace(im, {{"L1", bg}}, 2);
}
}  // namespace

void selftest_transfer(const Check& check) {
    std::cout << "\n-- layers between documents (s022) --\n";
    core::Document a;
    core::DocJournal ja{a};
    a.set_door([&] { return ja.open(); });
    L::load_fixture(a);   // 600 x 400; L4 the red square (100, 100) 200 x 150, in group L2
    const auto b_group = T::take(a, {"L2", "L4"});
    check("take: a group and a layer in it -> one root, its contents with it",
          b_group.roots.size() == 1 && b_group.roots[0] == "L2" && b_group.layers.size() == 3 && b_group.name == a.get("L2")->name,
          std::to_string(b_group.layers.size()));

    // Into an 8-bit RGB image of another size: centred, converted.
    core::Document b;
    core::DocJournal jb{b};
    b.set_door([&] { return jb.open(); });
    P::Format u8;
    blank(b, 300, 200, u8);
    check("refusal: none (RGB -> RGB, another depth)", T::refusal(T::take(a, {"L4"}), b.image()).empty(), "");
    const auto made = L::paste(jb, b, T::take(a, {"L4"}), "L1");
    check("paste: ONE step, one new root above the target", made.size() == 1 && jb.size() == 1 && b.panel("").front() == made[0] &&
              jb.undo_label().find("Paste") == 0, jb.undo_label());
    const auto l = b.get(made[0]);
    check("paste: a fresh id, the name kept", made[0] != "L1" && l && l->name == a.get("L4")->name, made[0]);
    check("paste: converted to the image's format", l && l->pixels && l->pixels->format() == u8 && l->pixels->width() == 300, "");
    // The CONTENT centred: the square (100, 100) 200 x 150 -> (50, 25) on 300 x 200.
    check("paste: the content centred -- the square's middle (200, 175) at (150, 100)", l && l->pixels->get(150, 100).c[0] > 0.99f &&
              l->pixels->get(150, 100).c[3] > 0.99f && l->pixels->get(50, 25).c[3] > 0.99f && l->pixels->get(48, 100).c[3] == 0.f &&
              l->pixels->get(251, 100).c[3] == 0.f, "");
    check("paste: the tree holds, no strays", b.check().empty() && b.strays() == 0, b.check());
    jb.undo();
    check("undo: gone", b.count() == 1, std::to_string(b.count()));
    jb.redo();
    // A group with its contents.
    const auto g = L::paste(jb, b, b_group);
    check("paste a group: its contents inside it", g.size() == 1 && b.is_group(g[0]) && b.panel(g[0]).size() == 2 && b.check().empty(),
          b.check());

    // Into Gray: luminance.
    core::Document c;
    core::DocJournal jc{c};
    c.set_door([&] { return jc.open(); });
    P::Format gray{P::Depth::U16, P::Mode::Gray, true};
    blank(c, 600, 400, gray);
    const auto gm = L::paste(jc, c, T::take(a, {"L4"}));
    check("paste into Gray: red's luminance", gm.size() == 1 && std::abs(c.get(gm[0])->pixels->get(200, 175).c[0] - 0.2126f) < 0.01f,
          gm.empty() ? "" : std::to_string(c.get(gm[0])->pixels->get(200, 175).c[0]));
    // Refusals.
    core::Document k;
    P::Format cmyk{P::Depth::U8, P::Mode::CMYK, true};
    blank(k, 100, 100, cmyk);
    check("refusal: RGB into CMYK", !T::refusal(T::take(a, {"L4"}), k.image()).empty(), "");
    core::Document n;
    P::Format flat{P::Depth::U8, P::Mode::RGB, false};
    blank(n, 100, 100, flat);
    check("refusal: into an image without alpha", !T::refusal(T::take(a, {"L4"}), n.image()).empty(), "");
    check("refusal: into no image", !T::refusal(T::take(a, {"L4"}), core::Image{}).empty(), "");
    // A flat image as a layer.
    const auto img = std::make_shared<const P::TileMap>(P::TileMap(u8, 50, 40).fill({0, 0, 50, 40}, P::rgba(0, 0, 1, 1)));
    core::Image im; im.width = 50; im.height = 40; im.format = u8;
    const auto dropped = L::paste(jb, b, T::of_image(im, img, "photo.png"));
    check("of_image: one pixel layer, named, centred", dropped.size() == 1 && b.get(dropped[0])->name == "photo.png" &&
              b.get(dropped[0])->pixels->get(150, 100).c[2] > 0.99f && b.get(dropped[0])->pixels->get(10, 10).c[3] == 0.f, "");
    // A flat file (a JPEG has no alpha) opens WITH alpha: a new layer painted
    // in part stays transparent elsewhere (it filled its tile black before).
    {
        namespace fs = std::filesystem;
        const auto path = (fs::temp_directory_path() / ("klimt_flat_" + std::to_string(::getpid()) + ".jpg")).string();
        std::string err;
        core::file::export_image(path, klimt::formats::Kind::JPEG, a.image(), a.snapshot(), err);
        auto opened = core::file::open(path, err);
        fs::remove(path);
        check("open a JPEG: the document has alpha, the Background opaque", opened && opened->image.format.alpha &&
                  opened->layers.at("L1").pixels->get(5, 5).c[3] == 1.f, err);
        if (opened) {
            core::Document f;
            core::DocJournal jf{f};
            f.set_door([&] { return jf.open(); });
            { core::Document::Loading load(f); f.replace(opened->image, opened->layers, opened->next_id); }
            const auto before = core::sample_composite(f.image(), f.snapshot(), 100, 100, 0);
            const auto id = L::add_layer(jf, f, "Paint");
            L::fill(jf, f, id, {10, 10, 5, 5}, P::rgba(0, 1, 0, 1));
            const auto after = core::sample_composite(f.image(), f.snapshot(), 100, 100, 0);
            check("a partial fill on a new layer: the picture shows through elsewhere",
                  std::abs(after.r - before.r) < 1e-4 && std::abs(after.g - before.g) < 1e-4, std::to_string(after.r));
        }
    }
    // Alt-drag in the panel: copies where the drop says, one step.
    {
        core::Document e;
        core::DocJournal je{e};
        e.set_door([&] { return je.open(); });
        L::load_fixture(e);   // L5 Top; L2 Group { L4 A, L3 B }; L1 Background
        const auto n0 = e.count();
        const auto c1 = L::copy_to(je, e, {"L5"}, "L4", L::Where::Below);
        check("copy_to: into the group, below A; named \"copy\"; one step",
              c1.size() == 1 && e.get(c1[0])->parent == "L2" && e.panel("L2")[1] == c1[0] && e.get(c1[0])->name == e.get("L5")->name + " copy" &&
                  je.size() == 1 && je.undo_label().rfind("Duplicate", 0) == 0, je.undo_label());
        const auto c2 = L::copy_to(je, e, {"L2"}, "L2", L::Where::Above);
        check("copy_to: a group onto itself -- the copy beside it, contents too", c2.size() == 1 && e.is_group(c2[0]) &&
                  e.panel(c2[0]).size() == 3 && e.panel("")[1] == c2[0] && e.check().empty(), e.check());
        check("copy_to: Inside a layer refused", L::copy_to(je, e, {"L5"}, "L1", L::Where::Inside).empty(), "");
        je.undo(); je.undo();
        check("copy_to: undone, as it was", e.count() == n0 && e.check().empty(), "");
    }
}
