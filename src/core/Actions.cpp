// core/Actions -- actions and batch (s084). See core/Actions.hpp and
// docs/actions.md.
#include "core/Actions.hpp"
#include "core/Adjust.hpp"
#include "core/File.hpp"
#include "core/Filter.hpp"
#include "core/Live.hpp"
#include "core/RoundTrip.hpp"
#include "core/Raw.hpp"
#include "core/Selection.hpp"
#include "core/Transform.hpp"
#include "core/Undo.hpp"
#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace klimt::core::actions {

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace P = pixel;

namespace {

const char* kTurnKeys[] = {"clockwise", "counter-clockwise", "half", "flip-horizontal", "flip-vertical"};
const char* kTurnNames[] = {"Rotate 90° Clockwise", "Rotate 90° Counter Clockwise", "Rotate 180°", "Flip Canvas Horizontal",
                            "Flip Canvas Vertical"};
const char* kAutoKeys[] = {"tone", "contrast", "colour", "white-balance"};
const char* kMaskKeys[] = {"reveal-all", "hide-all", "reveal-selection", "hide-selection"};
const char* kMaskNames[] = {"Add Mask: Reveal All", "Add Mask: Hide All", "Add Mask: Reveal Selection", "Add Mask: Hide Selection"};

Act make(const char* verb, const json& args, std::string label) { return {verb, args.dump(), std::move(label), true}; }

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4g", v);
    return b;
}

json colour_json(const P::Px& c) {
    json a = json::array();
    for (const float v : c.c) a.push_back(v);
    return a;
}
P::Px colour_from(const json& j) {
    P::Px p{};
    if (!j.is_array()) return p;
    for (std::size_t i = 0; i < j.size() && i < p.c.size(); ++i)
        if (j[i].is_number()) p.c[i] = j[i].get<float>();
    return p;
}

// The picked layer, a pixel layer that can take pixels -- else why not.
std::string pixels_refusal(const Document& d, const Document::Id& id) {
    if (id.empty()) return "no layer is picked";
    const auto l = d.get(id);
    if (!l) return "the picked layer is gone";
    if (l->kind != Kind::Pixel) return layers::not_pixels(d, id);
    if (d.locked_all(id) || (l->locks & lock::Pixels)) return "“" + l->name + "” is locked";
    return {};
}

// A document's top layer (panel order) -- what a freshly opened file has picked.
Document::Id top_of(const Document& d) {
    const auto top = d.panel("");
    return top.empty() ? Document::Id{} : top.front();
}

void load_into(Document& d, const snapshot::Snapshot& s) {
    Document::Loading load(d);
    if (const auto it = s.records.find(kImageId); it != s.records.end()) d.put(kImageId, it->second);
    for (const auto& [id, l] : s.records)
        if (id != kImageId) d.put(id, l);
}

}  // namespace

// ── the table ─────────────────────────────────────────────────────────────
const std::vector<Verb>& verbs() {
    static const std::vector<Verb> v = {
        {"image.size", "Image Size"},          {"canvas.size", "Canvas Size"},
        {"canvas.turn", "Rotate / Flip Canvas"}, {"canvas.rotate", "Rotate Canvas"},
        {"adjust.apply", "Adjustment"},        {"adjust.layer", "New Adjustment Layer"},
        {"adjust.auto", "Auto"},               {"filter.apply", "Filter"},
        {"filter.live", "Live Filter"},        {"filter.layer", "New Filter Layer"},
        {"select.all", "Select All"},          {"select.none", "Deselect"},
        {"select.invert", "Inverse"},          {"select.feather", "Feather"},
        {"select.fill", "Fill"},               {"layer.new", "New Layer"},
        {"layer.duplicate", "Duplicate Layer"}, {"layer.delete", "Delete Layer"},
        {"layer.mask", "Add Mask"},            {"layer.opacity", "Opacity"},
        {"layer.blend", "Blend Mode"},
    };
    return v;
}
bool known(const std::string& verb) {
    return std::any_of(verbs().begin(), verbs().end(), [&](const Verb& v) { return verb == v.key; });
}

// ── making acts ───────────────────────────────────────────────────────────
Act image_size(int w, int h, double dpi, canvas::Filter f, int from_w, int from_h) {
    // Kept proportions: within a pixel of the old shape.
    const bool keep = from_w > 0 && from_h > 0 && std::abs(static_cast<double>(w) * from_h / from_w - h) <= 1.0;
    json a{{"w", w}, {"h", h}, {"dpi", dpi}, {"nearest", f == canvas::Filter::Nearest}, {"keep", keep}};
    if (from_w > 0 && from_h > 0) { a["sx"] = static_cast<double>(w) / from_w; a["sy"] = static_cast<double>(h) / from_h; }
    std::string label = "Image Size  " + std::to_string(w) + " × " + std::to_string(h) + " px";
    if (from_w > 0 && w * 2 == from_w && (h * 2 == from_h || h * 2 == from_h + 1 || h * 2 == from_h - 1)) label += " (50 %)";
    return make("image.size", a, label);
}
Act canvas_size(int w, int h, int ax, int ay, int from_w, int from_h) {
    return make("canvas.size", {{"w", w}, {"h", h}, {"ax", ax}, {"ay", ay}, {"dw", w - from_w}, {"dh", h - from_h}},
                "Canvas Size  " + std::to_string(w) + " × " + std::to_string(h) + " px");
}
Act turn(int t) {
    t = std::clamp(t, 0, 4);
    return make("canvas.turn", {{"turn", kTurnKeys[t]}}, kTurnNames[t]);
}
Act rotate_canvas(double degrees) { return make("canvas.rotate", {{"degrees", degrees}}, "Rotate Canvas  " + num(degrees) + "°"); }
Act apply_adjust(const Adjust& a) {
    return make("adjust.apply", {{"adjust", json::parse(file::adjust_text(a))}}, std::string(adjust::label(a.kind)));
}
Act adjust_layer(const Adjust& a) {
    return make("adjust.layer", {{"adjust", json::parse(file::adjust_text(a))}}, "New Adjustment Layer: " + std::string(adjust::label(a.kind)));
}
Act apply_auto(adjust::AutoMethod m) {
    const int i = std::clamp(static_cast<int>(m), 0, 3);
    return make("adjust.auto", {{"method", kAutoKeys[i]}}, adjust::auto_label(m));
}
Act apply_filter(const Filter& f) {
    return make("filter.apply", {{"filter", json::parse(file::filter_text(f))}}, std::string(filter::label(f.kind)));
}
Act live_filter(const Filter& f) {
    return make("filter.live", {{"filter", json::parse(file::filter_text(f))}}, std::string(filter::label(f.kind)) + " (live)");
}
Act filter_layer(const Filter& f) {
    return make("filter.layer", {{"filter", json::parse(file::filter_text(f))}}, "New Filter Layer: " + std::string(filter::label(f.kind)));
}
Act select_all() { return make("select.all", json::object(), "Select All"); }
Act select_none() { return make("select.none", json::object(), "Deselect"); }
Act select_invert() { return make("select.invert", json::object(), "Inverse"); }
Act feather(double r) { return make("select.feather", {{"radius", r}}, "Feather  " + num(r) + " px"); }
Act fill_selection(const P::Px& c) { return make("select.fill", {{"colour", colour_json(c)}}, "Fill"); }
Act new_layer(const std::string& name) { return make("layer.new", {{"name", name}}, "New Layer “" + name + "”"); }
Act duplicate_layer() { return make("layer.duplicate", json::object(), "Duplicate Layer"); }
Act delete_layer() { return make("layer.delete", json::object(), "Delete Layer"); }
Act add_mask(layers::MaskFrom from) {
    const int i = std::clamp(static_cast<int>(from), 0, 3);
    return make("layer.mask", {{"from", kMaskKeys[i]}}, kMaskNames[i]);
}
Act set_opacity(int p) { return make("layer.opacity", {{"percent", p}}, "Opacity  " + std::to_string(p) + " %"); }
Act set_blend(Blend b) {
    return make("layer.blend", {{"mode", std::string(blend_key(b))}}, "Blend Mode: " + std::string(blend_key(b)));
}
void retune(Act& a, const Adjust& now) {
    if (a.verb != "adjust.layer" && a.verb != "adjust.apply") return;
    json j = json::parse(a.args, nullptr, false);
    if (!j.is_object()) return;
    j["adjust"] = json::parse(file::adjust_text(now));
    a.args = j.dump();
}
void retune(Act& a, const Filter& now) {
    if (a.verb.rfind("filter.", 0) != 0) return;
    json j = json::parse(a.args, nullptr, false);
    if (!j.is_object()) return;
    j["filter"] = json::parse(file::filter_text(now));
    a.args = j.dump();
}

// ── playing ───────────────────────────────────────────────────────────────
bool run(DocJournal& j, Document& d, Document::Id& picked, const Act& act, std::string& error,
         const std::function<bool()>& cancelled) {
    (void)cancelled;
    const json a = json::parse(act.args.empty() ? std::string("{}") : act.args, nullptr, false);
    if (!a.is_object()) { error = "its settings can't be read"; return false; }
    const auto& v = act.verb;
    const Image& im = d.image();
    const auto pixels_ok = [&] {
        error = pixels_refusal(d, picked);
        return error.empty();
    };
    const auto layer_ok = [&] {
        if (picked.empty() || !d.get(picked)) { error = "no layer is picked"; return false; }
        return true;
    };
    try {
        if (v == "image.size") {
            int w = a.at("w").get<int>(), h = a.at("h").get<int>();
            if (a.value("relative", false) && a.contains("sx")) {   // by the recorded ratio
                w = std::max(1, static_cast<int>(std::lround(im.width * a["sx"].get<double>())));
                h = std::max(1, static_cast<int>(std::lround(im.height * a["sy"].get<double>())));
            } else if (a.value("keep", false) && im.width > 0) {   // this picture's own shape
                h = std::max(1, static_cast<int>(std::lround(static_cast<double>(w) * im.height / im.width)));
            }
            if (const auto why = canvas::size_refusal(w, h); !why.empty()) { error = why; return false; }
            if (w == im.width && h == im.height && (a.value("relative", false) || a.value("dpi", im.dpi) == im.dpi)) return true;   // nothing to do
            const auto f = a.value("nearest", false) ? canvas::Filter::Nearest : canvas::Filter::Smooth;
            const double dpi = a.value("relative", false) ? im.dpi : a.value("dpi", im.dpi);   // by ratio: the picture's own resolution
            if (!canvas::image_size(j, d, w, h, dpi, f)) { error = "refused"; return false; }
            return true;
        }
        if (v == "canvas.size") {
            int w = a.at("w").get<int>(), h = a.at("h").get<int>();
            if (a.value("relative", false)) { w = im.width + a.value("dw", 0); h = im.height + a.value("dh", 0); }
            if (const auto why = canvas::size_refusal(w, h); !why.empty()) { error = why; return false; }
            if (w == im.width && h == im.height) return true;
            P::Px bg = a.contains("bg") ? colour_from(a["bg"]) : P::rgba(1, 1, 1);
            if (!canvas::canvas_size(j, d, w, h, a.value("ax", 0), a.value("ay", 0), bg)) { error = "refused"; return false; }
            return true;
        }
        if (v == "canvas.turn") {
            const std::string k = a.value("turn", std::string());
            for (int t = 0; t < 5; ++t)
                if (k == kTurnKeys[t]) {
                    if (!xform::turn(j, d, static_cast<xform::Turn>(t))) { error = "refused"; return false; }
                    return true;
                }
            error = "an unknown turn “" + k + "”";
            return false;
        }
        if (v == "canvas.rotate") {
            const double deg = a.value("degrees", 0.0);
            if (std::fmod(deg, 360.0) == 0) return true;
            P::Px bg = a.contains("bg") ? colour_from(a["bg"]) : P::rgba(1, 1, 1);
            if (!xform::rotate_canvas(j, d, deg, bg)) { error = "refused"; return false; }
            return true;
        }
        if (v == "adjust.apply" || v == "adjust.layer") {
            const auto adj = file::adjust_parse(a.at("adjust").dump());
            if (!adj) { error = "its settings can't be read"; return false; }
            if (v == "adjust.layer") {
                const auto id = layers::add_adjustment(j, d, adj->kind, picked);
                if (id.empty()) { error = "refused"; return false; }
                layers::set_adjust(j, d, id, *adj);
                picked = id;
                return true;
            }
            if (!pixels_ok()) return false;
            if (!d.get(picked)->pixels) { error = "“" + d.get(picked)->name + "” is empty"; return false; }
            layers::apply_adjust(j, d, picked, *adj);   // false: nothing changed -- fine
            return true;
        }
        if (v == "adjust.auto") {
            const std::string k = a.value("method", std::string());
            for (int m = 0; m < 4; ++m)
                if (k == kAutoKeys[m]) {
                    if (!pixels_ok()) return false;
                    if (!d.get(picked)->pixels) { error = "“" + d.get(picked)->name + "” is empty"; return false; }
                    layers::apply_auto(j, d, picked, static_cast<adjust::AutoMethod>(m));
                    return true;
                }
            error = "an unknown method “" + k + "”";
            return false;
        }
        if (v == "filter.apply" || v == "filter.live" || v == "filter.layer") {
            const auto f = file::filter_parse(a.at("filter").dump());
            if (!f) { error = "its settings can't be read"; return false; }
            if (v == "filter.layer") {
                const auto id = layers::add_filter_layer(j, d, *f, picked);
                if (id.empty()) { error = "refused"; return false; }
                picked = id;
                return true;
            }
            if (v == "filter.live") {
                if (!layer_ok()) return false;
                const auto l = d.get(picked);
                if (l->kind != Kind::Pixel && l->kind != Kind::Smart) { error = layers::not_pixels(d, picked); return false; }
                if (!filter::livable(f->kind)) { error = "it can't run live"; return false; }
                if (!layers::add_live(j, d, picked, *f)) { error = "refused"; return false; }
                return true;
            }
            if (!pixels_ok()) return false;
            if (!d.get(picked)->pixels && !filter::fills(f->kind)) { error = "“" + d.get(picked)->name + "” is empty"; return false; }
            layers::apply_filter(j, d, picked, *f);
            return true;
        }
        if (v == "select.all") { select::all(j, d); return true; }
        if (v == "select.none") { select::none(j, d); return true; }
        if (v == "select.invert") { select::invert(j, d); return true; }
        if (v == "select.feather") {
            if (!d.selection()) { error = "nothing is selected"; return false; }
            select::feather(j, d, a.value("radius", 0.0));
            return true;
        }
        if (v == "select.fill") {
            if (!pixels_ok()) return false;
            layers::fill_selection(j, d, picked, colour_from(a.value("colour", json::array())));
            return true;
        }
        if (v == "layer.new") {
            const auto id = layers::add_layer(j, d, a.value("name", std::string("Layer")), picked);
            if (id.empty()) { error = "refused"; return false; }
            picked = id;
            return true;
        }
        if (v == "layer.duplicate") {
            if (!layer_ok()) return false;
            const auto made = layers::duplicate(j, d, {picked});
            if (made.empty()) { error = "refused"; return false; }
            picked = made.front();
            return true;
        }
        if (v == "layer.delete") {
            if (!layer_ok()) return false;
            const auto l = d.get(picked);
            const auto parent = l->parent;
            if (!layers::remove(j, d, {picked})) { error = "it would leave the picture empty"; return false; }
            picked = top_of(d);
            (void)parent;
            return true;
        }
        if (v == "layer.mask") {
            if (!layer_ok()) return false;
            const std::string k = a.value("from", std::string());
            for (int m = 0; m < 4; ++m)
                if (k == kMaskKeys[m]) {
                    const auto from = static_cast<layers::MaskFrom>(m);
                    if ((from == layers::MaskFrom::RevealSelection || from == layers::MaskFrom::HideSelection) && !d.selection()) {
                        error = "nothing is selected";
                        return false;
                    }
                    if (d.get(picked)->mask) return true;   // has one already
                    if (!layers::add_mask(j, d, picked, from)) { error = "refused"; return false; }
                    return true;
                }
            error = "an unknown mask “" + k + "”";
            return false;
        }
        if (v == "layer.opacity") {
            if (!layer_ok()) return false;
            layers::set_opacity(j, d, picked, std::clamp(a.value("percent", 100), 0, 100));
            return true;
        }
        if (v == "layer.blend") {
            if (!layer_ok()) return false;
            const auto b = blend_from_key(a.value("mode", std::string("normal")));
            if (!b) { error = "an unknown blend mode"; return false; }
            layers::set_blend(j, d, picked, *b);
            return true;
        }
    } catch (const std::exception&) {
        error = "its settings can't be read";
        return false;
    }
    error = "a step this Klimt doesn't know (“" + v + "”)";
    return false;
}

Played play(const snapshot::Snapshot& start, const Document::Id& picked, const Action& a,
            const std::function<bool()>& cancelled, const std::function<void(double)>& progress) {
    Played out;
    Document d;
    DocJournal j{d};
    d.set_door([&j] { return j.open(); });
    load_into(d, start);
    out.picked = picked.empty() || !d.get(picked) ? top_of(d) : picked;
    const std::size_t n = a.acts.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (cancelled && cancelled()) { out.error = "cancelled"; return out; }
        const Act& act = a.acts[i];
        if (!act.on) continue;
        std::string err;
        if (!run(j, d, out.picked, act, err, cancelled)) {
            out.error = "step " + std::to_string(i + 1) + ", " + act.label + ": " + err;
            return out;
        }
        ++out.ran;
        if (progress) progress(static_cast<double>(i + 1) / static_cast<double>(n));
    }
    out.result = snapshot::take(d, "Action “" + a.name + "”");
    out.ok = true;
    return out;
}

// ── the library ───────────────────────────────────────────────────────────
std::vector<Action> load(const std::string& file) {
    std::vector<Action> out;
    std::ifstream in(file);
    if (!in) return out;
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("actions") || !j["actions"].is_array()) return out;
    for (const auto& ja : j["actions"]) {
        if (!ja.is_object()) continue;
        Action a;
        a.name = ja.value("name", std::string("Action"));
        if (ja.contains("acts") && ja["acts"].is_array())
            for (const auto& jc : ja["acts"]) {
                if (!jc.is_object()) continue;
                Act c;
                c.verb = jc.value("verb", std::string());
                c.args = jc.contains("args") ? jc["args"].dump() : std::string("{}");
                c.label = jc.value("label", c.verb);
                c.on = jc.value("on", true);
                if (!c.verb.empty()) a.acts.push_back(std::move(c));   // an unknown verb is kept (and refused when played)
            }
        out.push_back(std::move(a));
    }
    return out;
}

bool save(const std::string& file, const std::vector<Action>& all) {
    json j{{"klimt_actions", 1}, {"actions", json::array()}};
    for (const auto& a : all) {
        json ja{{"name", a.name}, {"acts", json::array()}};
        for (const auto& c : a.acts) {
            json args = json::parse(c.args.empty() ? std::string("{}") : c.args, nullptr, false);
            if (args.is_discarded()) args = json::object();
            ja["acts"].push_back({{"verb", c.verb}, {"args", args}, {"label", c.label}, {"on", c.on}});
        }
        j["actions"].push_back(std::move(ja));
    }
    std::error_code ec;
    fs::create_directories(fs::path(file).parent_path(), ec);
    const std::string tmp = file + ".partial";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << j.dump(2) << '\n';
        if (!out) return false;
    }
    fs::rename(tmp, file, ec);
    return !ec;
}

std::vector<Action> starters() {
    Action web{"Half Size for the Web", {}};
    web.acts.push_back(image_size(50, 50, 72, canvas::Filter::Smooth, 100, 100));
    {   // by ratio: half of whatever comes in
        json a = json::parse(web.acts[0].args);
        a["relative"] = true;
        web.acts[0].args = a.dump();
        web.acts[0].label = "Image Size  50 %";
    }
    Filter sharpen;
    sharpen.kind = FilterKind::UnsharpMask;
    sharpen.amount = 50;
    sharpen.radius = 0.6;
    sharpen.threshold = 0;
    web.acts.push_back(apply_filter(sharpen));
    Action tone{"Auto Tone and Contrast", {apply_auto(adjust::AutoMethod::Tone), apply_auto(adjust::AutoMethod::Contrast)}};
    Action turned{"Turn Clockwise", {turn(0)}};
    return {web, tone, turned};
}

std::string next_name(const std::vector<Action>& have) {
    int n = 0;
    for (const auto& a : have)
        if (a.name.rfind("Action ", 0) == 0) {
            try { n = std::max(n, std::stoi(a.name.substr(7))); } catch (...) {}
        }
    return "Action " + std::to_string(n + 1);
}

// ── batch ─────────────────────────────────────────────────────────────────
std::vector<std::string> sources(const Batch& b) {
    std::vector<std::string> out;
    std::error_code ec;
    if (b.source.empty() || !fs::is_directory(b.source, ec)) return out;
    const auto take = [&](const fs::directory_entry& e) {
        if (!e.is_regular_file(ec)) return;
        const auto name = e.path().filename().string();
        if (name.empty() || name[0] == '.') return;
        if (name.size() > 8 && name.compare(name.size() - 8, 8, ".partial") == 0) return;
        if (raw::is_raw_name(name)) { out.push_back(e.path().string()); return; }   // s085: camera RAW (developed with the last settings)
        const auto k = roundtrip::kind_of(e.path().string());
        if (k == formats::Kind::Unknown || k == formats::Kind::EPS) return;
        if (k == formats::Kind::PDF && !roundtrip::embedded(e.path().string())) return;   // only Klimt's
        out.push_back(e.path().string());
    };
    const auto dest = b.dest.empty() ? fs::path{} : fs::weakly_canonical(b.dest, ec);
    if (b.subfolders) {
        for (auto it = fs::recursive_directory_iterator(b.source, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec)) {
                const auto n = it->path().filename().string();
                if ((!n.empty() && n[0] == '.') || (!dest.empty() && fs::weakly_canonical(it->path(), ec) == dest)) it.disable_recursion_pending();
                continue;
            }
            take(*it);
        }
    } else {
        for (const auto& e : fs::directory_iterator(b.source, ec)) take(e);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string target_of(const Batch& b, const std::string& source) {
    const fs::path src(source);
    const auto* ef = formats::export_format(b.kind);
    const std::string ext = b.kind == formats::Kind::Klimt ? ".klimt" : ef ? ef->ext : src.extension().string();
    fs::path dir = b.dest.empty() ? src.parent_path() : fs::path(b.dest);
    if (!b.dest.empty() && b.subfolders) {   // keep the folders under the source
        std::error_code ec;
        const auto rel = fs::relative(src.parent_path(), b.source, ec);
        if (!ec && !rel.empty() && rel != ".") dir /= rel;
    }
    return (dir / (src.stem().string() + b.suffix + ext)).string();
}

std::string refusal(const Batch& b, const Action& a) {
    std::error_code ec;
    if (a.acts.empty()) return "the action has no steps";
    if (b.source.empty() || !fs::is_directory(b.source, ec)) return "pick the folder of pictures";
    if (!b.dest.empty() && fs::exists(b.dest, ec) && !fs::is_directory(b.dest, ec)) return "the destination isn't a folder";
    if (b.kind != formats::Kind::Klimt && !formats::export_format(b.kind)) return "pick what to save them as";
    const bool same = b.dest.empty() || fs::weakly_canonical(b.dest, ec) == fs::weakly_canonical(b.source, ec);
    if (same && b.suffix.empty() && !b.overwrite) return "into the same folder: give the names a suffix (\"-web\"), or tick Replace";
    return {};
}

BatchResult run_batch(const Batch& b, const Action& a, const std::function<bool()>& cancelled,
                      const std::function<void(double)>& progress, const std::function<void(const std::string&)>& at) {
    BatchResult r;
    const auto files = sources(b);
    const std::size_t n = files.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (cancelled && cancelled()) { r.cancelled = true; break; }
        const std::string& src = files[i];
        const std::string name = fs::path(src).filename().string();
        if (at) at(name);
        const std::string dst = target_of(b, src);
        std::error_code ec;
        if (fs::exists(dst, ec) && !b.overwrite) {
            ++r.skipped;
            r.lines.push_back(name + ": skipped -- " + fs::path(dst).filename().string() + " is there already");
            continue;
        }
        std::string err;
        auto c = file::open(src, err);
        if (!c) { ++r.failed; r.lines.push_back(name + ": couldn't open -- " + err); continue; }
        Document d;
        {
            Document::Loading load(d);
            d.replace(c->image, std::move(c->layers), c->next_id);
            d.replace_channels(std::move(c->channels));
        }
        const auto start = snapshot::take(d, "start");
        const auto inner = [&](double f) { if (progress) progress((static_cast<double>(i) + 0.8 * f) / static_cast<double>(n)); };
        const auto played = play(start, top_of(d), a, cancelled, inner);
        if (!played.ok) {
            if (cancelled && cancelled()) { r.cancelled = true; break; }
            ++r.failed;
            r.lines.push_back(name + ": " + played.error);
            continue;
        }
        Document out;
        load_into(out, played.result);
        file::Contents w;
        w.image = out.image();
        w.layers = out.snapshot();
        w.channels = out.channels();
        w.selection = out.selection_ptr();
        w.next_id = out.next_id();
        fs::create_directories(fs::path(dst).parent_path(), ec);
        std::vector<std::string> notes;
        formats::Progress fp;
        fp.cancelled = cancelled;
        if (!roundtrip::save_back(dst, b.kind, w, err, notes, fp, b.options)) {
            if (cancelled && cancelled()) { r.cancelled = true; break; }
            ++r.failed;
            r.lines.push_back(name + ": couldn't write " + fs::path(dst).filename().string() + " -- " + err);
            continue;
        }
        ++r.done;
        if (progress) progress(static_cast<double>(i + 1) / static_cast<double>(n));
    }
    return r;
}

}  // namespace klimt::core::actions
