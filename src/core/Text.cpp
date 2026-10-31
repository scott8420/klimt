// core::Text -- text layers (s046). See core/Text.hpp, docs/text.md.
#include "core/Text.hpp"

#include "core/Composite.hpp"   // from_srgb

#include <algorithm>

namespace klimt::core {

using pixel::kTile;

std::shared_ptr<const pixel::TileMap> text_pixels(const Image& im, const text::Spec& s) {
    if (im.width <= 0 || im.height <= 0) return nullptr;
    const auto cov = text::render(s, im.width, im.height);
    if (cov.at.empty()) return nullptr;
    const pixel::Format& f = im.format;
    const pixel::Px ink = from_srgb(f, s.colour[0], s.colour[1], s.colour[2]);
    const auto ai = static_cast<std::size_t>(f.channels() - 1);
    pixel::TileMap map(f, im.width, im.height);
    const int c0 = cov.at.x / kTile, c1 = (cov.at.x + cov.at.w - 1) / kTile;
    const int r0 = cov.at.y / kTile, r1 = (cov.at.y + cov.at.h - 1) / kTile;
    for (int row = r0; row <= r1; ++row)
        for (int col = c0; col <= c1; ++col) {
            pixel::TileWriter w(f, pixel::transparent());
            bool any = false;
            const int y0 = std::max(row * kTile, cov.at.y), y1 = std::min((row + 1) * kTile, cov.at.y + cov.at.h);
            const int x0 = std::max(col * kTile, cov.at.x), x1 = std::min((col + 1) * kTile, cov.at.x + cov.at.w);
            for (int y = y0; y < y1; ++y) {
                const std::uint8_t* src = cov.a.data() + static_cast<std::size_t>(y - cov.at.y) * static_cast<std::size_t>(cov.at.w);
                for (int x = x0; x < x1; ++x) {
                    const std::uint8_t a = src[x - cov.at.x];
                    if (!a) continue;
                    pixel::Px p = ink;
                    p.c[ai] = static_cast<float>(a) / 255.f * s.colour[3];
                    w.set(x - col * kTile, y - row * kTile, p);
                    any = true;
                }
            }
            if (any) map = map.with_tile(col, row, w.freeze());
        }
    if (map.tile_count() == 0) return nullptr;
    return std::make_shared<const pixel::TileMap>(std::move(map));
}

std::string text_name(const std::string& t) {
    std::string line = t.substr(0, t.find('\n'));
    const auto a = line.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "Text";
    line = line.substr(a, line.find_last_not_of(" \t\r") - a + 1);
    // At most 30 characters (not bytes): cut on a UTF-8 boundary.
    int chars = 0;
    std::size_t i = 0;
    while (i < line.size() && chars < 30) { i = static_cast<std::size_t>(text::next_char(line, static_cast<int>(i))); ++chars; }
    return i < line.size() ? line.substr(0, i) + "…" : line;
}

namespace layers {

namespace {
// The layer `id` with Spec `s` -- pixels set, the name following the text when
// it still carried the old text's name. nullopt: refused / no change.
std::optional<Layer> retyped(const Document& d, const Id& id, const text::Spec& s) {
    auto l = d.get(id);
    if (!l || l->kind != Kind::Text || !l->text || d.locked_all(id)) return std::nullopt;
    if (!text::refusal(s).empty() || *l->text == s) return std::nullopt;
    if (l->name == text_name(l->text->text)) l->name = text_name(s.text);
    l->text = std::make_shared<const text::Spec>(s);
    l->pixels = text_pixels(d.image(), s);
    return l;
}
}  // namespace

bool set_text(DocJournal& j, Document& d, const Id& id, const text::Spec& s, const std::string& label, const std::string& merge_key) {
    const auto l = retyped(d, id, s);
    if (!l) return false;
    return j.run(label + " “" + l->name + "”", {id}, [&] { d.put(id, *l); }, merge_key);
}

bool set_text_on(DocJournal& j, Document& d, const std::vector<Id>& ids, const std::function<void(text::Spec&)>& f,
                 const std::string& label, const std::string& merge_key) {
    std::vector<std::pair<Id, Layer>> to;
    for (const auto& id : ids) {
        const auto l = d.get(id);
        if (!l || !l->text) continue;
        text::Spec s = *l->text;
        f(s);
        if (const auto v = retyped(d, id, s)) to.emplace_back(id, *v);
    }
    if (to.empty()) return false;
    std::vector<Id> touched;
    for (const auto& [id, v] : to) touched.push_back(id);
    return j.run(to.size() == 1 ? label + " “" + to.front().second.name + "”" : label, touched, [&] {
        for (const auto& [id, v] : to) d.put(id, v);
    }, merge_key);
}

bool rasterize_text(DocJournal& j, Document& d, const Id& id) {
    auto l = d.get(id);
    if (!l || l->kind != Kind::Text || d.locked_all(id)) return false;
    l->kind = Kind::Pixel;
    l->text = nullptr;
    return j.run("Rasterize Type “" + l->name + "”", {id}, [&] { d.put(id, *l); });
}

void text_begin(DocJournal& j, const Document& d, const Id& id) {
    const auto l = d.get(id);
    j.begin(l ? "Type “" + l->name + "”" : std::string("Type"), id.empty() ? std::vector<Id>{} : std::vector<Id>{id});
}

bool text_end(DocJournal& j, Document& d, const Id& id, bool made_here) {
    if (!j.in_gesture()) return false;
    const auto l = d.get(id);
    if (made_here && (!l || !l->text || l->text->text.empty())) { j.cancel(); return false; }
    if (l) j.relabel((made_here ? "New Text Layer “" : "Type “") + l->name + "”");
    return j.commit();
}

}  // namespace layers
}  // namespace klimt::core
