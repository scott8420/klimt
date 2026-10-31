// libs/text -- setting a Spec with Pango + Cairo (s046). See text/Text.hpp.
#include "text/Text.hpp"

#include <pango/pangocairo.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace klimt::text {
namespace {

constexpr double kS = PANGO_SCALE;

// ── the matrix ──────────────────────────────────────────────────────────────
using M = std::array<double, 6>;
struct P { double x = 0, y = 0; };
P apply(const M& m, P p) { return {m[0] * p.x + m[2] * p.y + m[4], m[1] * p.x + m[3] * p.y + m[5]}; }
double det(const M& m) { return m[0] * m[3] - m[1] * m[2]; }
M inverse(const M& m) {
    const double d = det(m);
    if (std::abs(d) < 1e-12) return {1, 0, 0, 1, 0, 0};
    const double a = m[3] / d, b = -m[1] / d, c = -m[2] / d, e = m[0] / d;
    return {a, b, c, e, -(a * m[4] + c * m[5]), -(b * m[4] + e * m[5])};
}
M compose(const M& a, const M& b) {   // a after b
    return {a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],
            a[0] * b[2] + a[2] * b[3], a[1] * b[2] + a[3] * b[3],
            a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5]};
}
bool finite(const M& m) { return std::all_of(m.begin(), m.end(), [](double v) { return std::isfinite(v); }); }

// ── a laid-out Spec ─────────────────────────────────────────────────────────
// Owns its Pango objects. `ox, oy`: where the layout's top-left sits in text
// space. Layout units are px (Pango units / kS).
struct Laid {
    PangoContext* ctx = nullptr;
    PangoLayout*  lay = nullptr;
    double        ox = 0, oy = 0;
    M             m{1, 0, 0, 1, 0, 0};
    M             inv{1, 0, 0, 1, 0, 0};
    int           len = 0;
    Laid() = default;
    Laid(const Laid&) = delete;
    Laid& operator=(const Laid&) = delete;
    ~Laid() {
        if (lay) g_object_unref(lay);
        if (ctx) g_object_unref(ctx);
    }
    P to_image(double lx, double ly) const { return apply(m, {lx + ox, ly + oy}); }
    P to_layout(double ix, double iy) const { const P t = apply(inv, {ix, iy}); return {t.x - ox, t.y - oy}; }
};

void lay_out(const Spec& s0, Laid& L) {
    const Spec s = normalised(s0);
    // The thread's own font map (Pango >= 1.32.6 hands each thread its own
    // default), its own context: safe on a worker.
    PangoFontMap* fm = pango_cairo_font_map_get_default();
    L.ctx = pango_font_map_create_context(fm);
    // No hinting, no rounding: the same geometry at every zoom and matrix,
    // so the caret the editor draws sits where the glyphs were set.
    cairo_font_options_t* fo = cairo_font_options_create();
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    pango_cairo_context_set_font_options(L.ctx, fo);
    cairo_font_options_destroy(fo);
#if PANGO_VERSION_CHECK(1, 44, 0)
    pango_context_set_round_glyph_positions(L.ctx, FALSE);
#endif
    L.lay = pango_layout_new(L.ctx);

    PangoFontDescription* fd = pango_font_description_new();
    pango_font_description_set_family(fd, s.family.c_str());
    pango_font_description_set_weight(fd, static_cast<PangoWeight>(s.weight));
    pango_font_description_set_style(fd, s.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
    pango_font_description_set_absolute_size(fd, s.size * kS);
    pango_layout_set_font_description(L.lay, fd);
    pango_font_description_free(fd);

    PangoAttrList* at = pango_attr_list_new();
    if (s.tracking != 0)
        pango_attr_list_insert(at, pango_attr_letter_spacing_new(static_cast<int>(std::lround(s.tracking / 1000.0 * s.size * kS))));
#if PANGO_VERSION_CHECK(1, 50, 0)
    if (s.leading > 0) pango_attr_list_insert(at, pango_attr_line_height_new_absolute(static_cast<int>(std::lround(s.leading * kS))));
#endif
    pango_layout_set_attributes(L.lay, at);
    pango_attr_list_unref(at);

    pango_layout_set_text(L.lay, s.text.c_str(), static_cast<int>(s.text.size()));
    L.len = static_cast<int>(s.text.size());
    const PangoAlignment pa = s.align == Align::Center ? PANGO_ALIGN_CENTER
                            : s.align == Align::Right  ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT;
    pango_layout_set_alignment(L.lay, pa);
    if (!s.point()) {
        pango_layout_set_width(L.lay, static_cast<int>(std::lround(s.box_w * kS)));
        pango_layout_set_wrap(L.lay, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_justify(L.lay, s.align == Align::Justify);
        L.ox = s.x;
        L.oy = s.y;
    } else {
        PangoRectangle lg;
        pango_layout_get_extents(L.lay, nullptr, &lg);
        const double w = lg.width / kS;
        const double base = pango_layout_get_baseline(L.lay) / kS;
        L.ox = s.x - (s.align == Align::Center ? w / 2 : s.align == Align::Right ? w : 0) - lg.x / kS;
        L.oy = s.y - base;
    }
    L.m = s.m;
    L.inv = inverse(s.m);
}

// The layout's frame in layout px: the box (box text) or the lines' logical extent.
Box layout_frame(const Spec& s, const Laid& L) {
    if (!s.point()) {
        double h = s.box_h;
        if (!(h > 0)) { PangoRectangle lg; pango_layout_get_extents(L.lay, nullptr, &lg); h = std::max(lg.y + lg.height, 0) / kS; }
        return {0, 0, s.box_w, h};
    }
    PangoRectangle lg;
    pango_layout_get_extents(L.lay, nullptr, &lg);
    return {lg.x / kS, lg.y / kS, lg.width / kS, lg.height / kS};
}

int clamp_index(const std::string& t, int i) {
    i = std::clamp(i, 0, static_cast<int>(t.size()));
    while (i > 0 && i < static_cast<int>(t.size()) && (static_cast<unsigned char>(t[static_cast<std::size_t>(i)]) & 0xC0) == 0x80) --i;
    return i;
}
int forward_chars(const std::string& t, int i, int n) {
    for (int k = 0; k < n; ++k) i = next_char(t, i);
    return i;
}
// The y range of each line, layout px, and its [start, end) bytes.
struct LineBand { double y0, y1; int start, end; };
std::vector<LineBand> bands(const Laid& L) {
    std::vector<LineBand> out;
    PangoLayoutIter* it = pango_layout_get_iter(L.lay);
    do {
        int y0 = 0, y1 = 0;
        pango_layout_iter_get_line_yrange(it, &y0, &y1);
        PangoLayoutLine* ln = pango_layout_iter_get_line_readonly(it);
        out.push_back({y0 / kS, y1 / kS, ln->start_index, ln->start_index + ln->length});
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
    return out;
}
int band_of(const std::vector<LineBand>& b, int index) {
    for (std::size_t k = 0; k < b.size(); ++k) {
        const bool last = k + 1 == b.size();
        if (index < b[k].end || (index == b[k].end && (last || b[k + 1].start > index))) return static_cast<int>(k);
        if (index >= b[k].start && index < b[k].end) return static_cast<int>(k);
    }
    return static_cast<int>(b.size()) - 1;
}

}  // namespace

// ── the Spec ────────────────────────────────────────────────────────────────
std::string refusal(const Spec& s) {
    if (!(s.size >= 1 && s.size <= 5000)) return "size out of range";
    if (s.weight < 100 || s.weight > 900) return "weight out of range";
    if (!(s.tracking >= -1000 && s.tracking <= 5000)) return "tracking out of range";
    if (!(s.leading >= 0 && s.leading <= 10000)) return "leading out of range";
    if (!(s.box_w >= 0 && s.box_w <= 100000) || !(s.box_h >= 0 && s.box_h <= 100000)) return "box out of range";
    if (!std::isfinite(s.x) || !std::isfinite(s.y) || std::abs(s.x) > 1e7 || std::abs(s.y) > 1e7) return "position out of range";
    if (!finite(s.m) || std::abs(det(s.m)) < 1e-9) return "a degenerate matrix";
    for (float c : s.colour) if (!(c >= 0 && c <= 1)) return "colour out of range";
    if (!g_utf8_validate(s.text.data(), static_cast<gssize>(s.text.size()), nullptr)) return "text not UTF-8";
    if (s.family.empty()) return "no family";
    return {};
}

Spec normalised(Spec s) {
    auto fin = [](double v, double lo, double hi, double dflt) { return std::isfinite(v) ? std::clamp(v, lo, hi) : dflt; };
    s.size = fin(s.size, 1, 5000, 48);
    s.weight = std::clamp(s.weight, 100, 900);
    s.tracking = fin(s.tracking, -1000, 5000, 0);
    s.leading = fin(s.leading, 0, 10000, 0);
    s.box_w = fin(s.box_w, 0, 100000, 0);
    s.box_h = fin(s.box_h, 0, 100000, 0);
    s.x = fin(s.x, -1e7, 1e7, 0);
    s.y = fin(s.y, -1e7, 1e7, 0);
    if (!finite(s.m) || std::abs(det(s.m)) < 1e-9) s.m = {1, 0, 0, 1, 0, 0};
    for (float& c : s.colour) c = std::isfinite(c) ? std::clamp(c, 0.f, 1.f) : 0.f;
    if (!g_utf8_validate(s.text.data(), static_cast<gssize>(s.text.size()), nullptr)) {
        gchar* v = g_utf8_make_valid(s.text.data(), static_cast<gssize>(s.text.size()));
        s.text = v;
        g_free(v);
    }
    if (s.family.empty()) s.family = "Sans";
    return s;
}

Spec folded(Spec s) {
    const double k = s.m[0];
    if (!(k > 0) || std::abs(s.m[1]) > 1e-9 || std::abs(s.m[2]) > 1e-9 || std::abs(s.m[3] - k) > 1e-9 * std::max(1.0, k)) return s;
    if (std::abs(k - 1) < 1e-12) return s;
    const double size = s.size * k;
    if (!(size >= 1 && size <= 5000)) return s;   // can't hold it in the field: keep the matrix
    s.size = size;
    s.leading *= k;
    s.x *= k; s.y *= k;
    s.box_w *= k; s.box_h *= k;
    s.m = {1, 0, 0, 1, s.m[4], s.m[5]};
    return s;
}

Spec mapped(Spec s, const std::array<double, 6>& a) {
    s.m = compose(a, s.m);
    return folded(s);
}

// ── setting ─────────────────────────────────────────────────────────────────
Coverage render(const Spec& s, int clip_w, int clip_h) {
    Coverage out;
    if (s.text.empty() || clip_w <= 0 || clip_h <= 0) return out;
    Laid L;
    lay_out(s, L);
    PangoRectangle ink, lg;
    pango_layout_get_extents(L.lay, &ink, &lg);
    const double x0 = std::min(ink.x, lg.x) / kS, y0 = std::min(ink.y, lg.y) / kS;
    const double x1 = std::max(ink.x + ink.width, lg.x + lg.width) / kS, y1 = std::max(ink.y + ink.height, lg.y + lg.height) / kS;
    double bx0 = 1e18, by0 = 1e18, bx1 = -1e18, by1 = -1e18;
    for (const P c : {P{x0, y0}, P{x1, y0}, P{x1, y1}, P{x0, y1}}) {
        const P q = L.to_image(c.x, c.y);
        bx0 = std::min(bx0, q.x); by0 = std::min(by0, q.y);
        bx1 = std::max(bx1, q.x); by1 = std::max(by1, q.y);
    }
    const int ix0 = std::max(0, static_cast<int>(std::floor(bx0)) - 2), iy0 = std::max(0, static_cast<int>(std::floor(by0)) - 2);
    const int ix1 = std::min(clip_w, static_cast<int>(std::ceil(bx1)) + 2), iy1 = std::min(clip_h, static_cast<int>(std::ceil(by1)) + 2);
    if (ix1 <= ix0 || iy1 <= iy0) return out;
    out.at = {ix0, iy0, ix1 - ix0, iy1 - iy0};
    cairo_surface_t* sf = cairo_image_surface_create(CAIRO_FORMAT_A8, out.at.w, out.at.h);
    cairo_t* cr = cairo_create(sf);
    cairo_translate(cr, -ix0, -iy0);
    cairo_matrix_t cm;
    cairo_matrix_init(&cm, L.m[0], L.m[1], L.m[2], L.m[3], L.m[4], L.m[5]);
    cairo_transform(cr, &cm);
    cairo_translate(cr, L.ox, L.oy);
    if (!s.point() && s.box_h > 0) {   // only the lines that fit WHOLE (Photoshop's overflow)
        double fit = 0;
        for (const auto& b : bands(L)) if (b.y1 <= s.box_h + 0.5) fit = b.y1;
        cairo_rectangle(cr, -s.size, 0, s.box_w + 2 * s.size, fit);   // sideways: italics and overhangs may lean out
        cairo_clip(cr);
    }
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    pango_cairo_show_layout(cr, L.lay);
    cairo_destroy(cr);
    cairo_surface_flush(sf);
    const int stride = cairo_image_surface_get_stride(sf);
    const unsigned char* src = cairo_image_surface_get_data(sf);
    out.a.resize(static_cast<std::size_t>(out.at.w) * static_cast<std::size_t>(out.at.h));
    bool any = false;
    for (int y = 0; y < out.at.h; ++y)
        for (int x = 0; x < out.at.w; ++x) {
            const std::uint8_t v = src[static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x)];
            out.a[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.at.w) + static_cast<std::size_t>(x)] = v;
            any = any || v;
        }
    cairo_surface_destroy(sf);
    if (!any) out = {};
    return out;
}

std::array<Box, 4> frame(const Spec& s) {
    Laid L;
    lay_out(s, L);
    const Box f = layout_frame(normalised(s), L);
    std::array<Box, 4> out{};
    const P c[4] = {{f.x, f.y}, {f.x + f.w, f.y}, {f.x + f.w, f.y + f.h}, {f.x, f.y + f.h}};
    for (int k = 0; k < 4; ++k) { const P q = L.to_image(c[k].x, c[k].y); out[static_cast<std::size_t>(k)] = {q.x, q.y, 0, 0}; }
    return out;
}

bool overflows(const Spec& s) {
    if (s.point() || !(s.box_h > 0) || s.text.empty()) return false;
    Laid L;
    lay_out(s, L);
    const auto b = bands(L);
    return !b.empty() && b.back().y1 > s.box_h + 0.5;
}

Box bounds(const Spec& s) {
    const auto f = frame(s);
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const auto& c : f) { x0 = std::min(x0, c.x); y0 = std::min(y0, c.y); x1 = std::max(x1, c.x); y1 = std::max(y1, c.y); }
    return {x0, y0, x1 - x0, y1 - y0};
}

// ── editing ─────────────────────────────────────────────────────────────────
Caret caret(const Spec& s, int index) {
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    index = clamp_index(n.text, index);
    PangoRectangle strong;
    pango_layout_get_cursor_pos(L.lay, index, &strong, nullptr);
    const P a = L.to_image(strong.x / kS, strong.y / kS);
    const P b = L.to_image(strong.x / kS, (strong.y + strong.height) / kS);
    return {a.x, a.y, b.x, b.y};
}

int index_at(const Spec& s, double x, double y) {
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    const P p = L.to_layout(x, y);
    int idx = 0, trailing = 0;
    pango_layout_xy_to_index(L.lay, static_cast<int>(std::lround(p.x * kS)), static_cast<int>(std::lround(p.y * kS)), &idx, &trailing);
    return clamp_index(n.text, forward_chars(n.text, idx, trailing));
}

int step(const Spec& s, int index, int dir) {
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    index = clamp_index(n.text, index);
    int ni = index, tr = 0;
    pango_layout_move_cursor_visually(L.lay, TRUE, index, 0, dir < 0 ? -1 : 1, &ni, &tr);
    if (ni < 0) return 0;
    if (ni >= L.len || ni == G_MAXINT) return L.len;
    return clamp_index(n.text, forward_chars(n.text, ni, tr));
}

int line_step(const Spec& s, int index, int dir) {
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    index = clamp_index(n.text, index);
    const auto b = bands(L);
    const int k = band_of(b, index) + (dir < 0 ? -1 : 1);
    if (k < 0) return 0;
    if (k >= static_cast<int>(b.size())) return L.len;
    PangoRectangle strong;
    pango_layout_get_cursor_pos(L.lay, index, &strong, nullptr);
    const double ym = (b[static_cast<std::size_t>(k)].y0 + b[static_cast<std::size_t>(k)].y1) / 2;
    int idx = 0, tr = 0;
    pango_layout_xy_to_index(L.lay, strong.x, static_cast<int>(std::lround(ym * kS)), &idx, &tr);
    return clamp_index(n.text, forward_chars(n.text, idx, tr));
}

int line_home(const Spec& s, int index) {
    Laid L;
    lay_out(s, L);
    const auto b = bands(L);
    return b[static_cast<std::size_t>(band_of(b, clamp_index(normalised(s).text, index)))].start;
}

int line_end(const Spec& s, int index) {
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    const auto b = bands(L);
    const auto& ln = b[static_cast<std::size_t>(band_of(b, clamp_index(n.text, index)))];
    int e = ln.end;
    // A wrapped line ends in the space it broke at: the caret goes before it.
    if (e > ln.start && e < static_cast<int>(n.text.size()) && n.text[static_cast<std::size_t>(e - 1)] == ' ') --e;
    return e;
}

int word_step(const Spec& s, int index, int dir) {
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    index = clamp_index(n.text, index);
    int na = 0;
    const PangoLogAttr* la = pango_layout_get_log_attrs_readonly(L.lay, &na);
    const char* base = n.text.c_str();
    int c = static_cast<int>(g_utf8_pointer_to_offset(base, base + index));
    const int chars = na - 1;   // na = chars + 1
    if (dir > 0) {
        for (++c; c < chars && !la[c].is_word_end; ++c) {}
        c = std::min(c, chars);
    } else {
        for (--c; c > 0 && !la[c].is_word_start; --c) {}
        c = std::max(c, 0);
    }
    return static_cast<int>(g_utf8_offset_to_pointer(base, c) - base);
}

std::vector<std::array<Box, 4>> range(const Spec& s, int a, int b) {
    std::vector<std::array<Box, 4>> out;
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    a = clamp_index(n.text, a);
    b = clamp_index(n.text, b);
    if (a > b) std::swap(a, b);
    if (a == b) return out;
    for (const auto& ln : bands(L)) {
        const int lo = std::max(a, ln.start), hi = std::min(b, ln.end);
        const bool runs_on = b > ln.end;   // the selection carries past this line's end
        if (lo > hi || (lo == hi && !runs_on)) continue;
        PangoRectangle p0, p1;
        pango_layout_index_to_pos(L.lay, lo, &p0);
        double x0 = p0.x / kS, x1;
        if (hi >= ln.end) {
            PangoRectangle pe;
            pango_layout_get_cursor_pos(L.lay, ln.end, &pe, nullptr);
            x1 = pe.x / kS + (runs_on ? n.size * 0.25 : 0);   // a sliver for the line break
        } else {
            pango_layout_index_to_pos(L.lay, hi, &p1);
            x1 = p1.x / kS;
        }
        if (x1 < x0) std::swap(x0, x1);
        std::array<Box, 4> q{};
        const P c[4] = {{x0, ln.y0}, {x1, ln.y0}, {x1, ln.y1}, {x0, ln.y1}};
        for (int k = 0; k < 4; ++k) { const P t = L.to_image(c[k].x, c[k].y); q[static_cast<std::size_t>(k)] = {t.x, t.y, 0, 0}; }
        out.push_back(q);
    }
    return out;
}

int prev_char(const std::string& t, int i) {
    i = std::clamp(i, 0, static_cast<int>(t.size()));
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(t[static_cast<std::size_t>(i)]) & 0xC0) == 0x80) --i;
    return i;
}
int next_char(const std::string& t, int i) {
    const int n = static_cast<int>(t.size());
    i = std::clamp(i, 0, n);
    if (i == n) return n;
    ++i;
    while (i < n && (static_cast<unsigned char>(t[static_cast<std::size_t>(i)]) & 0xC0) == 0x80) ++i;
    return i;
}

std::vector<std::string> families() {
    PangoFontMap* fm = pango_cairo_font_map_get_default();
    PangoFontFamily** fams = nullptr;
    int n = 0;
    pango_font_map_list_families(fm, &fams, &n);
    std::set<std::string> seen;
    for (int k = 0; k < n; ++k) seen.insert(pango_font_family_get_name(fams[k]));
    g_free(fams);
    std::vector<std::string> out(seen.begin(), seen.end());
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return g_ascii_strcasecmp(a.c_str(), b.c_str()) < 0;
    });
    return out;
}

bool has_family(const std::string& f) {
    for (const auto& x : families()) if (g_ascii_strcasecmp(x.c_str(), f.c_str()) == 0) return true;
    return false;
}

}  // namespace klimt::text
