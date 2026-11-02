// core::smart -- smart layers (s082). See core/Smart.hpp.
#include "core/Smart.hpp"
#include "core/Canvas.hpp"
#include "core/File.hpp"
#include "core/Transfer.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace klimt::core::smart {

namespace fs = std::filesystem;

xform::Projective map_of(const Smart& s) {
    xform::Projective p;
    for (int i = 0; i < 9; ++i) p.h[i] = s.m[static_cast<std::size_t>(i)];
    return p;
}

Smart with_map(const Smart& s, const xform::Projective& m) {
    Smart t = s;
    for (int i = 0; i < 9; ++i) t.m[static_cast<std::size_t>(i)] = m.h[i];
    return t;
}

std::optional<std::shared_ptr<const pixel::TileMap>> rendered(const Image& im, const Smart& s, const std::function<bool()>& cancelled) {
    if (!s.source || im.width <= 0 || im.height <= 0) return std::shared_ptr<const pixel::TileMap>{};
    const xform::Projective m = map_of(s);
    // A map that only permutes pixels (whole moves, flips, quarter turns): exact.
    xform::Quality q = xform::Quality::Best;
    if (m.affine()) {
        const auto a = m.to_affine();
        const auto whole = [](double v) { return std::abs(v - std::round(v)) < 1e-9; };
        const auto unit = [&](double v) { return whole(v) && std::abs(v) <= 1; };
        if (unit(a.a) && unit(a.b) && unit(a.c) && unit(a.d) && whole(a.e) && whole(a.f) && std::abs(std::abs(a.a * a.d - a.b * a.c) - 1) < 1e-9)
            q = xform::Quality::Fast;
    }
    const pixel::TileMap blank(im.format, im.width, im.height);
    auto out = xform::over_transformed(blank, *s.source, nullptr, {0, 0, s.source->width(), s.source->height()}, m, q, cancelled);
    if (!out) return std::nullopt;
    auto inside = out->without_outside();   // the picture itself is kept: nothing beyond the canvas is needed
    if (!inside.tile_count()) return std::shared_ptr<const pixel::TileMap>{};
    return std::make_shared<const pixel::TileMap>(std::move(inside));
}

xform::Quad corners(const Smart& s) {
    const xform::Projective m = map_of(s);
    const double w = s.source ? s.source->width() : 0, h = s.source ? s.source->height() : 0;
    return {{m.apply({0, 0}), m.apply({w, 0}), m.apply({w, h}), m.apply({0, h})}};
}

double scale_of(const Smart& s) {
    if (!s.source) return 1;
    const xform::Projective m = map_of(s);
    const double cx = s.source->width() / 2.0, cy = s.source->height() / 2.0;
    const auto o = m.apply({cx, cy}), ex = m.apply({cx + 1, cy}), ey = m.apply({cx, cy + 1});
    const double sx = std::hypot(ex.x - o.x, ex.y - o.y), sy = std::hypot(ey.x - o.x, ey.y - o.y);
    return std::sqrt(sx * sy);
}

std::string refusal(const pixel::Format& f, const Image& into) {
    if (into.width <= 0 || into.height <= 0) return "there is no image to place it in";
    if (!into.format.alpha) return "the document has no transparency -- a placed picture needs it round its edges";
    const auto fm = f.mode, tm = into.format.mode;
    const bool simple = (fm == pixel::Mode::RGB || fm == pixel::Mode::Gray) && (tm == pixel::Mode::RGB || tm == pixel::Mode::Gray);
    if (fm != tm && !simple) return "a CMYK or Lab picture goes only into a document of the same mode, for now";
    return {};
}

Stamp stamp_of(const std::string& path) {
    Stamp s;
    std::error_code ec;
    if (path.empty() || !fs::is_regular_file(path, ec)) return s;
    const auto t = fs::last_write_time(path, ec);
    if (ec) return s;
    s.exists = true;
    s.stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    s.bytes = static_cast<std::uint64_t>(fs::file_size(path, ec));
    return s;
}

Link status(const Smart& s) {
    if (s.link.empty()) return Link::Embedded;
    const auto now = stamp_of(s.link);
    if (!now.exists) return Link::Missing;
    return now.stamp == s.stamp && now.bytes == s.bytes ? Link::Current : Link::Modified;
}

const char* link_label(Link l) {
    switch (l) {
        case Link::Embedded: return "embedded";
        case Link::Current:  return "linked";
        case Link::Modified: return "linked -- the file has changed";
        case Link::Missing:  return "linked -- the file is missing";
    }
    return "";
}

std::optional<Loaded> load(const std::string& path, std::string& error, const formats::Progress& p) {
    const auto before = stamp_of(path);
    if (!before.exists) { error = "it isn't there"; return std::nullopt; }
    formats::Progress first = p;
    first.progress = [&p](double f) { p.at(f * 0.6); };
    auto c = file::open(path, error, first);
    if (!c) return std::nullopt;
    formats::Progress second = p;
    second.progress = [&p](double f) { p.at(0.6 + f * 0.4); };
    auto flat = file::flatten(c->image, c->layers, second);
    if (!flat) { error = "cancelled"; return std::nullopt; }
    Loaded l;
    l.pixels = std::move(*flat);
    l.dpi = c->image.dpi;
    l.stamp = before.stamp;
    l.bytes = before.bytes;
    l.name = fs::path(path).filename().string();
    return l;
}

}  // namespace klimt::core::smart

namespace klimt::core::layers {

namespace {
std::shared_ptr<const pixel::TileMap> fitted(const Document& d, const pixel::TileMap& picture) {
    return std::make_shared<const pixel::TileMap>(transfer::converted(picture.without_outside(), d.image().format));
}
}  // namespace

Id place_smart(DocJournal& j, Document& d, const pixel::TileMap& picture, const std::string& name, double dpi,
               const std::string& link, std::int64_t stamp, std::uint64_t bytes, const Id& above) {
    const Image& im = d.image();
    if (picture.width() <= 0 || picture.height() <= 0 || !smart::refusal(picture.format(), im).empty()) return {};
    smart::Smart s;
    s.source = fitted(d, picture);
    s.name = name;
    s.link = link;
    s.stamp = stamp;
    s.bytes = bytes;
    s.dpi = dpi;
    // At 100 %, or down to fit; centred (on whole pixels at 100 %, so it lands exactly).
    const double pw = picture.width(), ph = picture.height();
    const double k = std::min({1.0, im.width / pw, im.height / ph});
    double ex = (im.width - pw * k) / 2, ey = (im.height - ph * k) / 2;
    if (k == 1) { ex = std::floor(ex); ey = std::floor(ey); }
    s = smart::with_map(s, xform::Projective(xform::Affine{k, 0, 0, k, ex, ey}));
    const auto px = smart::rendered(im, s);
    if (!px) return {};
    auto sp = std::make_shared<const smart::Smart>(std::move(s));
    return add_layer_of(j, d, Kind::Smart, name.empty() ? std::string("Placed") : name, above, [&](Layer& l) {
        l.smart = sp;
        l.pixels = *px;
    }, "Place");
}

bool to_smart(DocJournal& j, Document& d, const Id& id) {
    auto l = d.get(id);
    if (!l || l->kind != Kind::Pixel || d.locked_all(id) || !l->pixels) return false;
    const pixel::Rect r = canvas::content_bounds(*l->pixels);
    if (r.empty()) return false;
    const pixel::Rect in = r.intersect({0, 0, d.image().width, d.image().height});
    if (in.empty()) return false;
    // The picture: what it holds on the canvas (beyond the canvas is cut, as Photoshop's).
    pixel::TileMap pic(l->pixels->format(), in.w, in.h);
    pic = pic.paint({0, 0, in.w, in.h}, [&](int x, int y, const pixel::Px&) { return l->pixels->get(in.x + x, in.y + y); });
    smart::Smart s;
    s.source = std::make_shared<const pixel::TileMap>(std::move(pic));
    s.name = l->name;
    s.dpi = d.image().dpi;
    s = smart::with_map(s, xform::Projective(xform::Affine::translate(in.x, in.y)));
    const auto px = smart::rendered(d.image(), s);
    if (!px) return false;
    l->kind = Kind::Smart;
    l->smart = std::make_shared<const smart::Smart>(std::move(s));
    l->pixels = *px;
    return j.run("Convert to Smart Layer “" + l->name + "”", {id}, [&] { d.put(id, *l); });
}

bool rasterize_smart(DocJournal& j, Document& d, const Id& id) {
    auto l = d.get(id);
    if (!l || l->kind != Kind::Smart || d.locked_all(id)) return false;
    l->kind = Kind::Pixel;
    l->smart = nullptr;
    return j.run("Rasterize Smart Layer “" + l->name + "”", {id}, [&] { d.put(id, *l); });
}

bool replace_smart(DocJournal& j, Document& d, const Id& id, const pixel::TileMap& picture, const std::string& name,
                   double dpi, const std::string& link, std::int64_t stamp, std::uint64_t bytes, const std::string& label) {
    auto l = d.get(id);
    if (!l || l->kind != Kind::Smart || !l->smart || d.locked_all(id)) return false;
    if (picture.width() <= 0 || picture.height() <= 0 || !smart::refusal(picture.format(), d.image()).empty()) return false;
    const smart::Smart& old = *l->smart;
    smart::Smart s = old;
    s.source = fitted(d, picture);
    s.name = name;
    s.dpi = dpi;
    s.link = link;
    s.stamp = stamp;
    s.bytes = bytes;
    // Centred where the old one was: the old map after a shift by half the difference.
    const double dx = (old.source->width() - s.source->width()) / 2.0, dy = (old.source->height() - s.source->height()) / 2.0;
    s = smart::with_map(s, smart::map_of(old) * xform::Projective(xform::Affine::translate(dx, dy)));
    if (s == old) return false;
    const auto px = smart::rendered(d.image(), s);
    if (!px) return false;
    l->smart = std::make_shared<const smart::Smart>(std::move(s));
    l->pixels = *px;
    return j.run(label + " “" + l->name + "”", {id}, [&] { d.put(id, *l); });
}

bool embed_smart(DocJournal& j, Document& d, const Id& id) {
    auto l = d.get(id);
    if (!l || l->kind != Kind::Smart || !l->smart || l->smart->link.empty() || d.locked_all(id)) return false;
    smart::Smart s = *l->smart;
    s.link.clear();
    s.stamp = 0;
    s.bytes = 0;
    l->smart = std::make_shared<const smart::Smart>(std::move(s));
    return j.run("Embed Linked “" + l->name + "”", {id}, [&] { d.put(id, *l); });
}

bool set_smart_map(DocJournal& j, Document& d, const Id& id, const xform::Projective& m, const std::string& label) {
    auto l = d.get(id);
    if (!l || l->kind != Kind::Smart || !l->smart || d.locked_all(id)) return false;
    const smart::Smart s = smart::with_map(*l->smart, m);
    if (s == *l->smart) return false;
    const auto px = smart::rendered(d.image(), s);
    if (!px) return false;
    l->smart = std::make_shared<const smart::Smart>(s);
    l->pixels = *px;
    return j.run(label, {id}, [&] { d.put(id, *l); });
}

void load_smart_layer(Document& d, const pixel::TileMap& picture, const xform::Projective& m) {
    Document::Loading load(d);
    smart::Smart s;
    s.source = fitted(d, picture);
    s.name = "Placed";
    s = smart::with_map(s, m);
    Layer l;
    l.kind = Kind::Smart;
    l.name = "Placed";
    l.pos = 4;
    l.pixels = *smart::rendered(d.image(), s);
    l.smart = std::make_shared<const smart::Smart>(std::move(s));
    d.put("L6", l);
}

}  // namespace klimt::core::layers
