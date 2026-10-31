// core/PsdStyles -- layer styles in Photoshop's lfx2 descriptor (s053 m2).
// See core/PsdStyles.hpp and docs/file-format.md "Photoshop layer styles".
//
// An action descriptor, as Photoshop writes one: a Unicode name (here ""), a
// class id, a count, then items -- each a key, a 4-character type and its
// value. An id is a length then that many characters, or length 0 then four.
// Types used: long, doub, UntF (a unit -- #Prc, #Ang, #Pxl -- and a double),
// bool, enum (type id, value id), TEXT (UTF-16, a null at the end), Objc (a
// nested descriptor), VlLs (a list of typed values). Every number big-endian.
#include "core/PsdStyles.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace klimt::core::psd::fx {

namespace {

using pixel::Blend;

void be16(Bytes& b, unsigned v) { b.push_back(static_cast<std::uint8_t>(v >> 8)); b.push_back(static_cast<std::uint8_t>(v)); }
void be32(Bytes& b, std::uint32_t v) { be16(b, v >> 16); be16(b, v & 0xFFFF); }
void put(Bytes& b, std::string_view s) { b.insert(b.end(), s.begin(), s.end()); }
void put(Bytes& b, const Bytes& s) { b.insert(b.end(), s.begin(), s.end()); }
void f64(Bytes& b, double v) {
    std::uint64_t u;
    std::memcpy(&u, &v, 8);
    for (int i = 7; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(u >> (8 * i)));
}
void id(Bytes& b, std::string_view k) {
    be32(b, k.size() == 4 ? 0 : static_cast<std::uint32_t>(k.size()));
    put(b, k);
}

// Klimt's blend <-> the descriptor's BlnM enum.
struct ModeCode { Blend b; const char* code; };
constexpr ModeCode kModes[] = {
    {Blend::Normal, "Nrml"},          {Blend::Dissolve, "Dslv"},       {Blend::Darken, "Drkn"},
    {Blend::Multiply, "Mltp"},        {Blend::ColorBurn, "CBrn"},      {Blend::LinearBurn, "linearBurn"},
    {Blend::DarkerColor, "darkerColor"}, {Blend::Lighten, "Lghn"},     {Blend::Screen, "Scrn"},
    {Blend::ColorDodge, "CDdg"},      {Blend::LinearDodge, "linearDodge"}, {Blend::LighterColor, "lighterColor"},
    {Blend::Overlay, "Ovrl"},         {Blend::SoftLight, "SftL"},      {Blend::HardLight, "HrdL"},
    {Blend::VividLight, "vividLight"}, {Blend::LinearLight, "linearLight"}, {Blend::PinLight, "pinLight"},
    {Blend::HardMix, "hardMix"},      {Blend::Difference, "Dfrn"},     {Blend::Exclusion, "Xclu"},
    {Blend::Subtract, "blendSubtraction"}, {Blend::Divide, "blendDivide"}, {Blend::Hue, "H   "},
    {Blend::Saturation, "Strt"},      {Blend::Color, "Clr "},          {Blend::Luminosity, "Lmns"},
};
const char* code_of(Blend b) {
    for (const auto& m : kModes) if (m.b == b) return m.code;
    return "Nrml";
}
Blend blend_of(std::string_view c, Blend fallback) {
    for (const auto& m : kModes) if (c == m.code) return m.b;
    // The spelled-out forms some writers use.
    static const std::map<std::string, Blend, std::less<>> kLong = {
        {"normal", Blend::Normal}, {"dissolve", Blend::Dissolve}, {"darken", Blend::Darken}, {"multiply", Blend::Multiply},
        {"colorBurn", Blend::ColorBurn}, {"lighten", Blend::Lighten}, {"screen", Blend::Screen}, {"colorDodge", Blend::ColorDodge},
        {"overlay", Blend::Overlay}, {"softLight", Blend::SoftLight}, {"hardLight", Blend::HardLight}, {"difference", Blend::Difference},
        {"exclusion", Blend::Exclusion}, {"Sbtr", Blend::Subtract}, {"hue", Blend::Hue}, {"saturation", Blend::Saturation},
        {"color", Blend::Color}, {"luminosity", Blend::Luminosity}};
    const auto it = kLong.find(c);
    return it == kLong.end() ? fallback : it->second;
}

// ── writing ───────────────────────────────────────────────────────────────

// A descriptor being built: its class and its items (key -> type + value).
struct D {
    std::string cls;
    std::vector<std::pair<std::string, Bytes>> it;
    explicit D(std::string c) : cls(std::move(c)) {}
    Bytes& add(std::string_view k, std::string_view type) {
        it.emplace_back(std::string(k), Bytes{});
        put(it.back().second, type);
        return it.back().second;
    }
    D& lng(std::string_view k, int v) { be32(add(k, "long"), static_cast<std::uint32_t>(v)); return *this; }
    D& dbl(std::string_view k, double v) { f64(add(k, "doub"), v); return *this; }
    D& unt(std::string_view k, std::string_view unit, double v) { auto& b = add(k, "UntF"); put(b, unit); f64(b, v); return *this; }
    D& bln(std::string_view k, bool v) { add(k, "bool").push_back(v ? 1 : 0); return *this; }
    D& enm(std::string_view k, std::string_view type, std::string_view v) { auto& b = add(k, "enum"); id(b, type); id(b, v); return *this; }
    D& txt(std::string_view k, std::string_view s) {
        auto& b = add(k, "TEXT");
        be32(b, static_cast<std::uint32_t>(s.size() + 1));
        for (const char c : s) be16(b, static_cast<unsigned char>(c));   // ASCII only here
        be16(b, 0);
        return *this;
    }
    D& obj(std::string_view k, const D& o) { put(add(k, "Objc"), o.body()); return *this; }
    D& lst(std::string_view k, const std::vector<D>& v) {
        auto& b = add(k, "VlLs");
        be32(b, static_cast<std::uint32_t>(v.size()));
        for (const auto& o : v) { put(b, "Objc"); put(b, o.body()); }
        return *this;
    }
    Bytes body() const {
        Bytes b;
        be32(b, 1); be16(b, 0);   // the name: one null character
        id(b, cls);
        be32(b, static_cast<std::uint32_t>(it.size()));
        for (const auto& [k, v] : it) { id(b, k); put(b, v); }
        return b;
    }
};

D colour(const Rgb& c) {
    D o("RGBC");
    o.dbl("Rd  ", std::clamp(c.r, 0.f, 1.f) * 255.0).dbl("Grn ", std::clamp(c.g, 0.f, 1.f) * 255.0).dbl("Bl  ", std::clamp(c.b, 0.f, 1.f) * 255.0);
    return o;
}
// A straight contour (Klimt draws no contours): Photoshop's "Linear".
D linear_contour() {
    D o("ShpC");
    o.txt("Nm  ", "Linear");
    std::vector<D> pts;
    for (const double v : {0.0, 255.0}) { D p("CrPt"); p.dbl("Hrzn", v).dbl("Vrtc", v); pts.push_back(p); }
    o.lst("Crv ", pts);
    return o;
}
// The head every effect shares.
D effect(const char* cls, bool on, Blend mode) {
    D o(cls);
    o.bln("enab", on).bln("present", true).bln("showInDialog", true).enm("Md  ", "BlnM", code_of(mode));
    return o;
}

D shadow(const char* cls, const ShadowFx& f, bool drop) {
    D o = effect(cls, f.on, f.mode);
    o.obj("Clr ", colour(f.colour)).unt("Opct", "#Prc", f.opacity).bln("uglg", false).unt("lagl", "#Ang", f.angle)
        .unt("Dstn", "#Pxl", f.distance).unt("Ckmt", "#Prc", f.spread).unt("blur", "#Pxl", f.size).unt("Nose", "#Prc", 0)
        .bln("AntA", false).obj("TrnS", linear_contour());
    if (drop) o.bln("layerConceals", true);
    return o;
}

D glow(const char* cls, bool on, Blend mode, const Rgb& c, int opacity, int spread_or_choke, int size, const bool* center) {
    D o = effect(cls, on, mode);
    o.obj("Clr ", colour(c)).unt("Opct", "#Prc", opacity).enm("GlwT", "BETE", "SfBL").unt("Ckmt", "#Prc", spread_or_choke)
        .unt("blur", "#Pxl", size).unt("ShdN", "#Prc", 0).unt("Nose", "#Prc", 0).bln("AntA", false);
    if (center) o.enm("glwS", "IGSr", *center ? "SrcC" : "SrcE");
    o.obj("TrnS", linear_contour()).unt("Inpr", "#Prc", 50);
    return o;
}

// Has anyone set this effect (on, or changed from a fresh one)?
template <class T> bool set(const T& f) { return f.on || !(f == T{}); }

}  // namespace

bool crosses(const Styles& s) { return !s.pattern.on; }

Bytes block(const Styles& s) {
    D top("null");
    top.unt("Scl ", "#Prc", 100).bln("masterFXSwitch", true);
    const Styles fresh;
    // Photoshop's order.
    if (set(s.drop)) top.obj("DrSh", shadow("DrSh", s.drop, true));
    if (s.inner.on || !(s.inner == fresh.inner)) top.obj("IrSh", shadow("IrSh", s.inner, false));   // its fresh form is Styles'

    if (set(s.outer)) top.obj("OrGl", glow("OrGl", s.outer.on, s.outer.mode, s.outer.colour, s.outer.opacity, s.outer.spread, s.outer.size, nullptr));
    if (set(s.inner_glow))
        top.obj("IrGl", glow("IrGl", s.inner_glow.on, s.inner_glow.mode, s.inner_glow.colour, s.inner_glow.opacity,
                             s.inner_glow.choke, s.inner_glow.size, &s.inner_glow.center));
    if (set(s.bevel)) {
        const auto& b = s.bevel;
        D o = effect("ebbl", b.on, Blend::Normal);
        o.it.pop_back();   // a bevel has two modes, no "Md  "
        o.enm("hglM", "BlnM", code_of(b.highlight_mode)).obj("hglC", colour(b.highlight)).unt("hglO", "#Prc", b.highlight_opacity)
            .enm("sdwM", "BlnM", code_of(b.shadow_mode)).obj("sdwC", colour(b.shadow)).unt("sdwO", "#Prc", b.shadow_opacity)
            .enm("bvlT", "bvlT", "SfBL").enm("bvlS", "BESl", b.style == 1 ? "Embs" : "InrB").bln("uglg", false)
            .unt("lagl", "#Ang", b.angle).unt("Lald", "#Ang", b.altitude).unt("srgR", "#Prc", b.depth).unt("blur", "#Pxl", b.size)
            .enm("bvlD", "BESs", b.up ? "In  " : "Out ").obj("TrnS", linear_contour()).bln("antialiasGloss", false)
            .unt("Sftn", "#Pxl", b.soften).bln("useShape", false).bln("useTexture", false);
        top.obj("ebbl", o);
    }
    if (set(s.satin)) {
        const auto& t = s.satin;
        D o = effect("ChFX", t.on, t.mode);
        o.obj("Clr ", colour(t.colour)).bln("AntA", false).bln("Invr", t.invert).unt("Opct", "#Prc", t.opacity)
            .unt("lagl", "#Ang", t.angle).unt("Dstn", "#Pxl", t.distance).unt("blur", "#Pxl", t.size).obj("MpgS", linear_contour());
        top.obj("ChFX", o);
    }
    if (set(s.overlay)) {
        D o = effect("SoFi", s.overlay.on, s.overlay.mode);
        o.unt("Opct", "#Prc", s.overlay.opacity).obj("Clr ", colour(s.overlay.colour));
        top.obj("SoFi", o);
    }
    if (set(s.gradient)) {
        const auto& g = s.gradient;
        D grad("Grdn");
        grad.txt("Nm  ", "Custom").enm("GrdF", "GrdF", "CstS").dbl("Intr", 4096);
        std::vector<D> stops, trans;
        for (int k = 0; k < 2; ++k) {
            D c("Clrt");
            c.obj("Clr ", colour(k == 0 ? g.from : g.to)).enm("Type", "Clry", "UsrS").lng("Lctn", k * 4096).lng("Mdpn", 50);
            stops.push_back(c);
            D t("TrnS");
            t.unt("Opct", "#Prc", 100).lng("Lctn", k * 4096).lng("Mdpn", 50);
            trans.push_back(t);
        }
        grad.lst("Clrs", stops).lst("Trns", trans);
        D off("Pnt ");
        off.unt("Hrzn", "#Prc", 0).unt("Vrtc", "#Prc", 0);
        D o = effect("GrFl", g.on, g.mode);
        o.unt("Opct", "#Prc", g.opacity).obj("Grad", grad).unt("Angl", "#Ang", g.angle).enm("Type", "GrdT", g.style == 1 ? "Rdl " : "Lnr ")
            .bln("Rvrs", g.reverse).bln("Dthr", false).bln("Algn", true).unt("Scl ", "#Prc", g.scale).obj("Ofst", off);
        top.obj("GrFl", o);
    }
    if (set(s.stroke)) {
        const auto& t = s.stroke;
        D o = effect("FrFX", t.on, t.mode);
        o.enm("Styl", "FStl", t.position == 1 ? "InsF" : t.position == 2 ? "CtrF" : "OutF").enm("PntT", "FrFl", "SClr")
            .unt("Opct", "#Prc", t.opacity).unt("Sz  ", "#Pxl", t.size).obj("Clr ", colour(t.colour));
        top.obj("FrFX", o);
    }
    Bytes b;
    be32(b, 0);    // object effects version
    be32(b, 16);   // descriptor version
    put(b, top.body());
    return b;
}

// ── reading ───────────────────────────────────────────────────────────────

namespace {

// A read descriptor value.
struct V {
    char t = '?';                 // n number, b bool, e enum, s text, o object, l list, ? other
    double n = 0;
    std::string s;                // enum value; object class
    std::vector<std::pair<std::string, V>> kv;
    std::vector<V> list;
    const V* get(std::string_view k) const {
        for (const auto& [key, v] : kv) if (key == k) return &v;
        return nullptr;
    }
    double num(std::string_view k, double d) const { const V* v = get(k); return v && (v->t == 'n' || v->t == 'b') ? v->n : d; }
    bool flag(std::string_view k, bool d) const { const V* v = get(k); return v && v->t == 'b' ? v->n != 0 : d; }
    std::string enm(std::string_view k) const { const V* v = get(k); return v && v->t == 'e' ? v->s : std::string(); }
};

struct Cur {
    const std::uint8_t* d;
    std::size_t n, at = 0;
    bool bad = false;
    bool need(std::size_t k) { if (bad || k > n - std::min(at, n)) bad = true; return !bad; }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        const std::uint32_t v = (std::uint32_t(d[at]) << 24) | (std::uint32_t(d[at + 1]) << 16) | (std::uint32_t(d[at + 2]) << 8) | d[at + 3];
        at += 4;
        return v;
    }
    unsigned u8() { return need(1) ? d[at++] : 0; }
    double f64() {
        if (!need(8)) return 0;
        std::uint64_t u = 0;
        for (int i = 0; i < 8; ++i) u = (u << 8) | d[at + static_cast<std::size_t>(i)];
        at += 8;
        double v;
        std::memcpy(&v, &u, 8);
        return std::isfinite(v) ? v : 0;
    }
    std::string str(std::size_t k) { if (!need(k)) return {}; std::string s(reinterpret_cast<const char*>(d + at), k); at += k; return s; }
    std::string ident() {
        const std::uint32_t k = u32();
        if (k > 4096) { bad = true; return {}; }
        return str(k == 0 ? 4 : k);
    }
    std::string ustr() {
        const std::uint32_t k = u32();
        if (k > (1u << 20) || !need(std::size_t(k) * 2)) { bad = true; return {}; }
        std::string s;
        for (std::uint32_t i = 0; i < k; ++i) {
            const unsigned c = (unsigned(d[at + 2 * i]) << 8) | d[at + 2 * i + 1];
            if (c == 0) break;
            s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        at += std::size_t(k) * 2;
        return s;
    }
};

V value(Cur& c, const std::string& type, int depth);

V object(Cur& c, int depth) {
    V o;
    o.t = 'o';
    if (depth > 16) { c.bad = true; return o; }
    c.ustr();
    o.s = c.ident();
    const std::uint32_t count = c.u32();
    for (std::uint32_t i = 0; i < count && !c.bad && i < 4096; ++i) {
        std::string k = c.ident();
        const std::string type = c.str(4);
        if (c.bad) break;
        V v = value(c, type, depth + 1);
        if (c.bad) break;
        o.kv.emplace_back(std::move(k), std::move(v));
    }
    return o;
}

V value(Cur& c, const std::string& type, int depth) {
    V v;
    if (type == "long") { v.t = 'n'; v.n = static_cast<std::int32_t>(c.u32()); }
    else if (type == "comp") { v.t = 'n'; const double hi = static_cast<std::int32_t>(c.u32()); v.n = hi * 4294967296.0 + c.u32(); }
    else if (type == "doub") { v.t = 'n'; v.n = c.f64(); }
    else if (type == "UntF") { c.str(4); v.t = 'n'; v.n = c.f64(); }
    else if (type == "bool") { v.t = 'b'; v.n = c.u8() ? 1 : 0; }
    else if (type == "enum") { c.ident(); v.t = 'e'; v.s = c.ident(); }
    else if (type == "TEXT") { v.t = 's'; v.s = c.ustr(); }
    else if (type == "Objc" || type == "GlbO") v = object(c, depth);
    else if (type == "VlLs") {
        v.t = 'l';
        const std::uint32_t k = c.u32();
        for (std::uint32_t i = 0; i < k && !c.bad && i < 4096; ++i) {
            const std::string t = c.str(4);
            v.list.push_back(value(c, t, depth + 1));
        }
    }
    else if (type == "type" || type == "GlbC") { c.ustr(); v.s = c.ident(); }
    else if (type == "tdta" || type == "alis" || type == "Pth ") { const std::uint32_t k = c.u32(); c.str(k); }
    else if (type == "UnFl") { c.str(4); const std::uint32_t k = c.u32(); for (std::uint32_t i = 0; i < k && !c.bad && i < 65536; ++i) c.f64(); }
    else c.bad = true;   // a reference ("obj ") or unknown: can't go past it
    return v;
}

Rgb rgb_of(const V* o, bool* odd) {
    if (!o || o->t != 'o') return {};
    if (o->s == "RGBC")
        return {static_cast<float>(std::clamp(o->num("Rd  ", 0) / 255.0, 0.0, 1.0)),
                static_cast<float>(std::clamp(o->num("Grn ", 0) / 255.0, 0.0, 1.0)),
                static_cast<float>(std::clamp(o->num("Bl  ", 0) / 255.0, 0.0, 1.0))};
    if (o->s == "Grsc") {   // gray, 0..100 % ink
        const float g = static_cast<float>(std::clamp(1.0 - o->num("Gry ", 0) / 100.0, 0.0, 1.0));
        return {g, g, g};
    }
    if (odd) *odd = true;
    return {};
}

int pct(const V& o, std::string_view k, int d) { return std::clamp(static_cast<int>(std::lround(o.num(k, d))), 0, 100); }
int px(const V& o, std::string_view k, int d, int lo, int hi) { return std::clamp(static_cast<int>(std::lround(o.num(k, d))), lo, hi); }
// Into Klimt's -180..180.
int wrap(double deg) {
    const long a = std::lround(deg);
    return static_cast<int>(((a + 180) % 360 + 360) % 360 - 180);
}
// The effect's light: its own, or the document's when it "uses global light".
int angle(const V& o, int global) { return wrap(o.flag("uglg", false) ? global : o.num("lagl", global)); }

// The effects in `top` into a Styles; losses into `lost`.
std::optional<Styles> convert(const V& top, int ga, int galt, std::vector<std::string>& lost) {
    Styles s;
    std::map<std::string, int> seen;
    bool any = false, odd = false;
    const auto mode = [](const V& o, std::string_view k, Blend d) { return blend_of(o.enm(k), d); };
    const auto take = [&](const V& o) {
        if (o.t != 'o' || !o.flag("present", true)) return;
        if (++seen[o.s] > 1) return;   // a second of the same kind
        const bool on = o.flag("enab", false);
        if (o.s == "DrSh" || o.s == "IrSh") {
            ShadowFx& f = o.s == "DrSh" ? s.drop : s.inner;
            f.on = on;
            f.mode = mode(o, "Md  ", Blend::Multiply);
            f.colour = rgb_of(o.get("Clr "), &odd);
            f.opacity = pct(o, "Opct", 75);
            f.angle = angle(o, ga);
            f.distance = px(o, "Dstn", 5, 0, 1000);
            f.spread = pct(o, "Ckmt", 0);
            f.size = px(o, "blur", 5, 0, 250);
        } else if (o.s == "OrGl") {
            auto& g = s.outer;
            g.on = on;
            g.mode = mode(o, "Md  ", Blend::Screen);
            g.colour = rgb_of(o.get("Clr "), &odd);
            g.opacity = pct(o, "Opct", 75);
            g.spread = pct(o, "Ckmt", 0);
            g.size = px(o, "blur", 5, 0, 250);
        } else if (o.s == "IrGl") {
            auto& g = s.inner_glow;
            g.on = on;
            g.mode = mode(o, "Md  ", Blend::Screen);
            g.colour = rgb_of(o.get("Clr "), &odd);
            g.opacity = pct(o, "Opct", 75);
            g.choke = pct(o, "Ckmt", 0);
            g.size = px(o, "blur", 5, 0, 250);
            g.center = o.enm("glwS") == "SrcC";
        } else if (o.s == "ebbl") {
            auto& b = s.bevel;
            b.on = on;
            const std::string st = o.enm("bvlS");
            b.style = (st == "Embs" || st == "PlEb") ? 1 : 0;
            if (!st.empty() && st != "Embs" && st != "InrB") lost.emplace_back("bevel");
            b.depth = px(o, "srgR", 100, 1, 1000);
            b.up = o.enm("bvlD") != "Out ";
            b.size = px(o, "blur", 5, 0, 250);
            b.soften = px(o, "Sftn", 0, 0, 16);
            b.angle = angle(o, ga);
            b.altitude = std::clamp(static_cast<int>(std::lround(o.flag("uglg", false) ? galt : o.num("Lald", galt))), 0, 90);
            b.highlight = o.get("hglC") ? rgb_of(o.get("hglC"), &odd) : Rgb{1, 1, 1};
            b.highlight_opacity = pct(o, "hglO", 75);
            b.highlight_mode = mode(o, "hglM", Blend::Screen);
            b.shadow = rgb_of(o.get("sdwC"), &odd);
            b.shadow_opacity = pct(o, "sdwO", 75);
            b.shadow_mode = mode(o, "sdwM", Blend::Multiply);
        } else if (o.s == "ChFX") {
            auto& t = s.satin;
            t.on = on;
            t.mode = mode(o, "Md  ", Blend::Multiply);
            t.colour = rgb_of(o.get("Clr "), &odd);
            t.opacity = pct(o, "Opct", 50);
            t.angle = angle(o, 19);
            t.distance = px(o, "Dstn", 11, 1, 250);
            t.size = px(o, "blur", 14, 0, 250);
            t.invert = o.flag("Invr", true);
        } else if (o.s == "SoFi") {
            auto& t = s.overlay;
            t.on = on;
            t.mode = mode(o, "Md  ", Blend::Normal);
            t.colour = rgb_of(o.get("Clr "), &odd);
            t.opacity = pct(o, "Opct", 100);
        } else if (o.s == "GrFl") {
            auto& g = s.gradient;
            g.on = on;
            g.mode = mode(o, "Md  ", Blend::Normal);
            g.opacity = pct(o, "Opct", 100);
            g.angle = wrap(o.num("Angl", 90));
            const std::string ty = o.enm("Type");
            g.style = ty == "Rdl " ? 1 : 0;
            if (!ty.empty() && ty != "Lnr " && ty != "Rdl ") lost.emplace_back("gradient");
            g.scale = px(o, "Scl ", 100, 10, 150);
            g.reverse = o.flag("Rvrs", false);
            bool lossy = false;
            if (const V* gr = o.get("Grad"); gr && gr->t == 'o') {
                std::vector<std::pair<double, Rgb>> stops;
                if (const V* cl = gr->get("Clrs"); cl && cl->t == 'l')
                    for (const auto& st : cl->list)
                        if (st.t == 'o') stops.emplace_back(st.num("Lctn", 0), rgb_of(st.get("Clr "), &odd));
                std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                if (!stops.empty()) { g.from = stops.front().second; g.to = stops.back().second; }
                if (stops.size() > 2) lossy = true;
                if (const V* tr = gr->get("Trns"); tr && tr->t == 'l')
                    for (const auto& st : tr->list)
                        if (st.t == 'o' && st.num("Opct", 100) < 99.5) lossy = true;
            }
            if (lossy) lost.emplace_back("gradient");
        } else if (o.s == "FrFX") {
            auto& t = s.stroke;
            t.on = on;
            t.mode = mode(o, "Md  ", Blend::Normal);
            t.colour = rgb_of(o.get("Clr "), &odd);
            t.opacity = pct(o, "Opct", 100);
            t.size = px(o, "Sz  ", 3, 1, 250);
            const std::string p = o.enm("Styl");
            t.position = p == "InsF" ? 1 : p == "CtrF" ? 2 : 0;
            if (o.enm("PntT") == "GrFl" || o.enm("PntT") == "Ptrn") lost.emplace_back("stroke fill");
        } else if (o.s == "patternFill") {
            if (on) lost.emplace_back("pattern");
            return;
        } else {
            return;
        }
        any = true;
    };
    for (const auto& [k, v] : top.kv) {
        if (v.t == 'l') for (const auto& e : v.list) take(e);
        else take(v);
    }
    for (const auto& [cls, n] : seen)
        if (n > 1) { lost.emplace_back("copies"); break; }
    if (odd) lost.emplace_back("colour");
    if (!any) return std::nullopt;
    if (!top.flag("masterFXSwitch", true)) {
        s.drop.on = s.inner.on = s.outer.on = s.inner_glow.on = s.bevel.on = s.satin.on = s.overlay.on = s.gradient.on = s.stroke.on = false;
        lost.emplace_back("off");
    }
    return s;
}

}  // namespace

struct Reading::Parsed { V top; };

bool Reading::feed(std::string_view key, const std::uint8_t* d, std::size_t n) {
    if (key != "lfx2" && key != "lmfx") return false;
    if (m_p && key == "lmfx") return true;   // lfx2 first: the same effects again
    Cur c{d, n};
    c.u32();                   // object effects version
    if (c.u32() != 16) return true;
    auto p = std::make_shared<Parsed>();
    p->top = object(c, 0);
    if (p->top.kv.empty()) return true;   // damaged at once: nothing (a damaged tail keeps what came first)
    m_p = std::move(p);
    return true;
}

std::optional<Styles> Reading::result(int global_angle, int global_altitude) const {
    if (!m_p) return std::nullopt;
    std::vector<std::string> lost;
    return convert(m_p->top, global_angle, global_altitude, lost);
}

std::vector<std::string> Reading::losses() const {
    std::vector<std::string> lost;
    if (m_p) convert(m_p->top, 120, 30, lost);
    std::sort(lost.begin(), lost.end());
    lost.erase(std::unique(lost.begin(), lost.end()), lost.end());
    return lost;
}

}  // namespace klimt::core::psd::fx
