// libs/text -- setting a Spec with Pango + Cairo (s046). See text/Text.hpp.
#include "text/Text.hpp"
#include "text/Hyphen.hpp"

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

// One line as placed: [start, end) bytes; its band [top, top + lead); its
// baseline; x of its logical box; Pango's own y band and the shift onto ours.
struct Placed {
    int    start = 0, end = 0;
    double top = 0, lead = 0, base = 0;   // the band and the baseline (placed)
    double x = 0, w = 0;                  // the logical box's left and width (alignment in)
    double ox = 0;                        // the line's origin x (pango_cairo_show_layout_line's)
    double ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;   // its ink, placed
    double py0 = 0, py1 = 0, dy = 0;      // Pango's own band; Pango y + dy = placed y
};

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
    // s047: the lines as PLACED (Composix's model): each line takes its
    // leading (the Spec's, or 1.2 x its size when auto); its logical box is
    // centred in it. Layout px, from the layout's top (0) down. `dy` takes a
    // Pango y on this line to the placed y; [py0, py1) is Pango's own band.
    std::vector<Placed> lines;
    // s048: the text Pango lays out -- the Spec's, with a soft hyphen (U+00AD)
    // at each place a word may break when the box hyphenates -- and the byte
    // maps between the two (empty: the same text).
    std::string      lt;
    std::vector<int> fw, bk;   // Spec byte -> layout byte; layout byte -> Spec byte
    int to_lt(int i) const { return fw.empty() ? i : fw[static_cast<std::size_t>(std::clamp(i, 0, static_cast<int>(fw.size()) - 1))]; }
    int to_sp(int i) const { return bk.empty() ? i : bk[static_cast<std::size_t>(std::clamp(i, 0, static_cast<int>(bk.size()) - 1))]; }
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

// s047: a line's logical box without its runs' rise -- a raised or lowered
// word (baseline shift) doesn't move its line in the leading (Composix s260
// line_box). Ascent above the baseline and height, px.
struct LineBox { double asc, h; };
LineBox line_box(PangoLayoutLine* line) {
    PangoRectangle lg;
    pango_layout_line_get_extents(line, nullptr, &lg);
    LineBox out{-lg.y / kS, lg.height / kS};
    bool risen = false;
    for (GSList* l = line->runs; l && !risen; l = l->next) risen = static_cast<PangoGlyphItem*>(l->data)->y_offset != 0;
    if (!risen) return out;
    int top = 0, bot = 0;
    bool any = false;
    for (GSList* l = line->runs; l; l = l->next) {
        auto* gi = static_cast<PangoGlyphItem*>(l->data);
        PangoRectangle r;
        pango_glyph_string_extents(gi->glyphs, gi->item->analysis.font, nullptr, &r);
        if (r.height <= 0) continue;
        top = any ? std::min(top, r.y) : r.y;
        bot = any ? std::max(bot, r.y + r.height) : r.y + r.height;
        any = true;
    }
    if (any) out = {-top / kS, (bot - top) / kS};
    return out;
}

// s047: manual kerning, Composix's kern_fixup (s265). Pango's letter spacing
// goes half before a character and half after it; a kern is space AFTER it.
// So the kerned character gets letter spacing (its tracking + the kern -- the
// line is broken knowing the whole of it) and then its glyph is moved back by
// the kern's share of the left half, so the character stays and what follows
// moves. A line's first character: Pango trims its left half from the width as
// well, so the kern's share of it goes back into the width.
struct KernAt { int byte; int total, track; };   // bytes; Pango units
void kern_fixup(PangoLayout* l, const std::vector<KernAt>& ks) {
    if (ks.empty()) return;
    const auto left = [](int ls) {   // as Pango splits letter spacing
        int a = ls / 2;
        if ((ls & (PANGO_SCALE - 1)) == 0) a = PANGO_UNITS_ROUND(a);
        return a;
    };
    for (GSList* li = pango_layout_get_lines(l); li; li = li->next) {
        auto* line = static_cast<PangoLayoutLine*>(li->data);
        for (GSList* r = line->runs; r; r = r->next) {
            auto* gi = static_cast<PangoGlyphItem*>(r->data);
            for (int i = 0; i < gi->glyphs->num_glyphs; ++i) {
                if (!gi->glyphs->glyphs[i].attr.is_cluster_start) continue;
                const int byte = gi->item->offset + gi->glyphs->log_clusters[i];
                for (const auto& k : ks) {
                    if (k.byte != byte) continue;
                    auto& g = gi->glyphs->glyphs[i].geometry;
                    if (byte == line->start_index) g.width += left(k.total) - left(k.track);
                    else g.x_offset -= left(k.total) - left(k.track);
                    break;
                }
            }
        }
    }
}

// s047: the runs as Pango attributes over the Spec's own (Composix's
// apply_runs + add_look: later attributes win). Tracking is per em of the
// character's own size, so a run with a size carries the letter spacing again.
std::vector<KernAt> add_runs(const Spec& s, const Laid& L, PangoAttrList* at) {
    std::vector<KernAt> kerns;
    for (const Run& r : s.runs) {
        const Look& k = r.look;
        const auto add = [&](PangoAttribute* x) {
            x->start_index = static_cast<guint>(L.to_lt(r.start));
            x->end_index = static_cast<guint>(L.to_lt(r.end));
            pango_attr_list_insert(at, x);
        };
        if (k.family) add(pango_attr_family_new(k.family->c_str()));
        if (k.weight) add(pango_attr_weight_new(static_cast<PangoWeight>(*k.weight)));
        if (k.italic) add(pango_attr_style_new(*k.italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL));
        if (k.stretch) add(pango_attr_stretch_new(static_cast<PangoStretch>(*k.stretch)));
        const double size = k.size.value_or(s.size);
        if (k.size) add(pango_attr_size_new_absolute(static_cast<int>(std::lround(size * kS))));
        const int track = static_cast<int>(std::lround(k.tracking.value_or(s.tracking) / 1000.0 * size * kS));
        const int total = track + (k.kern ? static_cast<int>(std::lround(*k.kern / 1000.0 * size * kS)) : 0);
        if (k.size || k.tracking || k.kern) add(pango_attr_letter_spacing_new(total));
        if (k.kern && *k.kern != 0)
            for (int i = r.start; i < r.end; i = next_char(s.text, i)) kerns.push_back({L.to_lt(i), total, track});
        // s048: super- / subscript (a smaller size, raised or lowered, over any shift)
        const double rise = k.shift.value_or(0) + (k.position == 1 ? kSuperRise * size : k.position == 2 ? -kSubDrop * size : 0.0);
        if (rise != 0) add(pango_attr_rise_new(static_cast<int>(std::lround(rise * kS))));
        if (k.position && *k.position != 0) add(pango_attr_scale_new(kScriptSize));
        if (k.caps == 2) add(pango_attr_variant_new(PANGO_VARIANT_SMALL_CAPS));
#if PANGO_VERSION_CHECK(1, 50, 0)
        if (k.caps == 1) add(pango_attr_text_transform_new(PANGO_TEXT_TRANSFORM_UPPERCASE));
#endif
        if (const auto f = ot_settings(k); !f.empty()) add(pango_attr_font_features_new(f.c_str()));
        if (k.underline) add(pango_attr_underline_new(*k.underline ? PANGO_UNDERLINE_SINGLE : PANGO_UNDERLINE_NONE));
        if (k.strike) add(pango_attr_strikethrough_new(*k.strike ? TRUE : FALSE));
        if (k.colour) {
            const auto c16 = [](float v) { return static_cast<guint16>(std::lround(std::clamp(v, 0.f, 1.f) * 65535.f)); };
            add(pango_attr_foreground_new(c16((*k.colour)[0]), c16((*k.colour)[1]), c16((*k.colour)[2])));
        }
    }
    return kerns;
}

// s048: hyphenation. Box type that hyphenates gets a soft hyphen (U+00AD)
// at each place Liang's patterns let a word break: Pango breaks there when
// the line needs it and draws the hyphen only then. Words of fewer than
// kShortest letters stay whole (Photoshop's "Words Longer Than" 5).
constexpr int kShortest = 6;
void hyphenate_into(const Spec& s, Laid& L) {
    L.lt = s.text;
    L.fw.clear();
    L.bk.clear();
    if (s.point() || !s.hyphenate || s.text.empty()) return;
    std::vector<int> at;   // Spec bytes a soft hyphen goes before
    const int n = static_cast<int>(s.text.size());
    int i = 0;
    while (i < n) {
        const gunichar c = g_utf8_get_char(s.text.c_str() + i);
        if (!g_unichar_isalpha(c)) { i = next_char(s.text, i); continue; }
        int j = i, letters = 0;
        while (j < n && g_unichar_isalpha(g_utf8_get_char(s.text.c_str() + j))) { j = next_char(s.text, j); ++letters; }
        if (letters >= kShortest)
            for (int p : hyphen_points(std::string_view(s.text).substr(static_cast<std::size_t>(i), static_cast<std::size_t>(j - i)), -1, -1, s.lang))
                if (p > 0 && p < j - i) at.push_back(i + p);
        i = j;
    }
    if (at.empty()) return;
    static const std::string kShy = "\xC2\xAD";
    L.lt.clear();
    L.fw.assign(static_cast<std::size_t>(n) + 1, 0);
    L.bk.clear();
    std::size_t k = 0;
    for (int b = 0; b <= n; ++b) {
        if (k < at.size() && at[k] == b) {   // the hyphen's two bytes map to the break's place
            for (char ch : kShy) { L.lt += ch; L.bk.push_back(b); }
            ++k;
        }
        L.fw[static_cast<std::size_t>(b)] = static_cast<int>(L.lt.size());
        L.bk.push_back(b);
        if (b < n) L.lt += s.text[static_cast<std::size_t>(b)];
    }
}

// s047: Composix's leading (layout::leading_of): the Spec's, else 1.2 x the
// size -- the line's LARGEST size, so a big word gets its own room.
double lead_of(const Spec& s, double line_size) { return s.leading > 0 ? s.leading : std::max(1.0, line_size * 1.2); }
double line_size(const Spec& s, int start, int end);   // (below: the runs)

// Lines placed one below another, each at its leading, its logical box
// centred in it (Composix's walk_frame).
void place(const Spec& s, Laid& L) {
    L.lines.clear();
    PangoLayoutIter* it = pango_layout_get_iter(L.lay);
    double top = 0;
    do {
        PangoLayoutLine* ln = pango_layout_iter_get_line_readonly(it);
        PangoRectangle lg;
        pango_layout_iter_get_line_extents(it, nullptr, &lg);
        int y0 = 0, y1 = 0;
        pango_layout_iter_get_line_yrange(it, &y0, &y1);
        const double pbase = pango_layout_iter_get_baseline(it) / kS;
        PangoRectangle ink, own;   // the line's own boxes (origin at its baseline's start)
        pango_layout_line_get_extents(ln, &ink, &own);
        const LineBox lb = line_box(ln);   // its rise left out
        const double asc = lb.asc, h = lb.h;
        Placed p;
        p.start = L.to_sp(ln->start_index);
        p.end = L.to_sp(ln->start_index + ln->length);
        p.top = top;
        p.lead = lead_of(s, line_size(s, p.start, p.end));
        p.base = top + (p.lead - h) / 2 + asc;
        p.x = lg.x / kS;
        p.w = lg.width / kS;
        p.ox = (lg.x - own.x) / kS;
        p.ix0 = p.ox + ink.x / kS;
        p.ix1 = p.ix0 + ink.width / kS;
        p.iy0 = p.base + ink.y / kS;
        p.iy1 = p.iy0 + ink.height / kS;
        p.py0 = y0 / kS;
        p.py1 = y1 / kS;
        p.dy = p.base - pbase;
        L.lines.push_back(p);
        top += p.lead;
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
}

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
    pango_font_description_set_stretch(fd, static_cast<PangoStretch>(s.stretch));
    pango_font_description_set_absolute_size(fd, s.size * kS);
    pango_layout_set_font_description(L.lay, fd);
    pango_font_description_free(fd);

    hyphenate_into(s, L);   // s048: before the runs (their bytes go through its map)
    PangoAttrList* at = pango_attr_list_new();
    if (s.tracking != 0)
        pango_attr_list_insert(at, pango_attr_letter_spacing_new(static_cast<int>(std::lround(s.tracking / 1000.0 * s.size * kS))));
    const auto kerns = add_runs(s, L, at);
    pango_layout_set_attributes(L.lay, at);
    pango_attr_list_unref(at);

    pango_layout_set_text(L.lay, L.lt.c_str(), static_cast<int>(L.lt.size()));
    L.len = static_cast<int>(s.text.size());
    const PangoAlignment pa = s.align == Align::Center ? PANGO_ALIGN_CENTER
                            : s.align == Align::Right  ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT;
    pango_layout_set_alignment(L.lay, pa);
    if (!s.point()) {
        pango_layout_set_width(L.lay, static_cast<int>(std::lround(s.box_w * kS)));
        pango_layout_set_wrap(L.lay, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_justify(L.lay, s.align == Align::Justify);
    }
    kern_fixup(L.lay, kerns);
    place(s, L);
    if (!s.point()) {
        L.ox = s.x;
        L.oy = s.y;
    } else {
        PangoRectangle lg;
        pango_layout_get_extents(L.lay, nullptr, &lg);
        const double w = lg.width / kS;
        L.ox = s.x - (s.align == Align::Center ? w / 2 : s.align == Align::Right ? w : 0) - lg.x / kS;
        L.oy = s.y - L.lines.front().base;
    }
    L.m = s.m;
    L.inv = inverse(s.m);
}

// The lines' whole height (the last one's band ends it).
double lines_h(const Laid& L) { return L.lines.back().top + L.lines.back().lead; }
// How many lines are set: box type with a height sets only the lines whose
// band fits WHOLE (Photoshop's overflow); otherwise all of them.
std::size_t set_lines(const Spec& s, const Laid& L) {
    if (s.point() || !(s.box_h > 0)) return L.lines.size();
    std::size_t n = 0;
    while (n < L.lines.size() && L.lines[n].top + L.lines[n].lead <= s.box_h + 0.5) ++n;
    return n;
}

// The layout's frame in layout px: the box (box text) or the lines' logical extent.
Box layout_frame(const Spec& s, const Laid& L) {
    if (!s.point()) return {0, 0, s.box_w, s.box_h > 0 ? s.box_h : lines_h(L)};
    PangoRectangle lg;
    pango_layout_get_extents(L.lay, nullptr, &lg);
    return {lg.x / kS, 0, lg.width / kS, lines_h(L)};
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
int band_of(const std::vector<Placed>& b, int index) {
    for (std::size_t k = 0; k < b.size(); ++k) {
        const bool last = k + 1 == b.size();
        if (index < b[k].end || (index == b[k].end && (last || b[k + 1].start > index))) return static_cast<int>(k);
        if (index >= b[k].start && index < b[k].end) return static_cast<int>(k);
    }
    return static_cast<int>(b.size()) - 1;
}
// The line Pango put a y on (its own bands).
int line_at_pango_y(const std::vector<Placed>& b, double py) {
    for (std::size_t k = 0; k < b.size(); ++k) if (py < b[k].py1) return static_cast<int>(k);
    return static_cast<int>(b.size()) - 1;
}

}  // namespace

// ── the Spec ────────────────────────────────────────────────────────────────
std::string refusal(const Spec& s) {
    if (!(s.size >= 1 && s.size <= 5000)) return "size out of range";
    if (s.weight < 100 || s.weight > 900) return "weight out of range";
    if (s.stretch < 0 || s.stretch > 8) return "stretch out of range";
    if (!(s.tracking >= -1000 && s.tracking <= 5000)) return "tracking out of range";
    if (!(s.leading >= 0 && s.leading <= 10000)) return "leading out of range";
    if (!(s.box_w >= 0 && s.box_w <= 100000) || !(s.box_h >= 0 && s.box_h <= 100000)) return "box out of range";
    if (!std::isfinite(s.x) || !std::isfinite(s.y) || std::abs(s.x) > 1e7 || std::abs(s.y) > 1e7) return "position out of range";
    if (!finite(s.m) || std::abs(det(s.m)) < 1e-9) return "a degenerate matrix";
    for (float c : s.colour) if (!(c >= 0 && c <= 1)) return "colour out of range";
    if (!g_utf8_validate(s.text.data(), static_cast<gssize>(s.text.size()), nullptr)) return "text not UTF-8";
    if (s.family.empty()) return "no family";
    const int n = static_cast<int>(s.text.size());
    int prev = 0;
    for (const Run& r : s.runs) {   // s047
        if (r.start < prev || r.end <= r.start || r.end > n) return "runs out of order";
        if (r.start < n && (static_cast<unsigned char>(s.text[static_cast<std::size_t>(r.start)]) & 0xC0) == 0x80) return "a run inside a character";
        if (r.end < n && (static_cast<unsigned char>(s.text[static_cast<std::size_t>(r.end)]) & 0xC0) == 0x80) return "a run inside a character";
        const Look& k = r.look;
        if (k.family && k.family->empty()) return "a run's family is empty";
        if (k.weight && (*k.weight < 100 || *k.weight > 900)) return "a run's weight out of range";
        if (k.stretch && (*k.stretch < 0 || *k.stretch > 8)) return "a run's stretch out of range";
        if (k.size && !(*k.size >= 1 && *k.size <= 5000)) return "a run's size out of range";
        if (k.tracking && !(*k.tracking >= -1000 && *k.tracking <= 5000)) return "a run's tracking out of range";
        if (k.kern && !(*k.kern >= -1000 && *k.kern <= 5000)) return "a run's kern out of range";
        if (k.shift && !(*k.shift >= -5000 && *k.shift <= 5000)) return "a run's shift out of range";
        if (k.colour) for (float c : *k.colour) if (!(c >= 0 && c <= 1)) return "a run's colour out of range";
        prev = r.end;
    }
    return {};
}

Spec normalised(Spec s) {
    auto fin = [](double v, double lo, double hi, double dflt) { return std::isfinite(v) ? std::clamp(v, lo, hi) : dflt; };
    s.size = fin(s.size, 1, 5000, 48);
    s.weight = std::clamp(s.weight, 100, 900);
    s.stretch = std::clamp(s.stretch, 0, 8);
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
    if (s.lang.empty()) s.lang = "en-US";   // s048
    // s047: the runs -- on character boundaries, in range, in order, not
    // overlapping; a field the same as the Spec's dropped; an empty look
    // dropped; neighbours with one look joined.
    const int n = static_cast<int>(s.text.size());
    const auto snap = [&](int i) {
        i = std::clamp(i, 0, n);
        while (i > 0 && i < n && (static_cast<unsigned char>(s.text[static_cast<std::size_t>(i)]) & 0xC0) == 0x80) --i;
        return i;
    };
    std::vector<Run> rs;
    for (Run r : s.runs) {
        r.start = snap(r.start);
        r.end = snap(r.end);
        Look& k = r.look;
        if (k.family && (k.family->empty() || *k.family == s.family)) k.family.reset();
        if (k.weight) { k.weight = std::clamp(*k.weight, 100, 900); if (*k.weight == s.weight) k.weight.reset(); }
        if (k.italic && *k.italic == s.italic) k.italic.reset();
        if (k.stretch) { k.stretch = std::clamp(*k.stretch, 0, 8); if (*k.stretch == s.stretch) k.stretch.reset(); }
        if (k.size) { if (!std::isfinite(*k.size)) k.size.reset(); else { k.size = std::clamp(*k.size, 1.0, 5000.0); if (*k.size == s.size) k.size.reset(); } }
        if (k.tracking) { if (!std::isfinite(*k.tracking)) k.tracking.reset(); else { k.tracking = std::clamp(*k.tracking, -1000.0, 5000.0); if (*k.tracking == s.tracking) k.tracking.reset(); } }
        if (k.kern) { if (!std::isfinite(*k.kern) || *k.kern == 0) k.kern.reset(); else k.kern = std::clamp(*k.kern, -1000.0, 5000.0); }
        if (k.shift) { if (!std::isfinite(*k.shift) || *k.shift == 0) k.shift.reset(); else k.shift = std::clamp(*k.shift, -5000.0, 5000.0); }
        if (k.underline && !*k.underline) k.underline.reset();
        // s048: a setting that is the default is no setting
        if (k.caps) { k.caps = std::clamp(*k.caps, 0, 2); if (*k.caps == 0) k.caps.reset(); }
        if (k.position) { k.position = std::clamp(*k.position, 0, 2); if (*k.position == 0) k.position.reset(); }
        if (k.ligatures && *k.ligatures) k.ligatures.reset();
        if (k.font_kern && *k.font_kern) k.font_kern.reset();
        if (k.dlig && !*k.dlig) k.dlig.reset();
        if (k.figures) { k.figures = std::clamp(*k.figures, 0, 2); if (*k.figures == 0) k.figures.reset(); }
        if (k.fig_space) { k.fig_space = std::clamp(*k.fig_space, 0, 2); if (*k.fig_space == 0) k.fig_space.reset(); }
        if (k.frac && !*k.frac) k.frac.reset();
        if (k.ordn && !*k.ordn) k.ordn.reset();
        if (k.swsh && !*k.swsh) k.swsh.reset();
        if (k.strike && !*k.strike) k.strike.reset();
        if (k.colour) {
            for (float& c : *k.colour) c = std::isfinite(c) ? std::clamp(c, 0.f, 1.f) : 0.f;
            if ((*k.colour)[0] == s.colour[0] && (*k.colour)[1] == s.colour[1] && (*k.colour)[2] == s.colour[2]) k.colour.reset();
        }
        if (r.end > r.start && !k.empty()) rs.push_back(std::move(r));
    }
    std::stable_sort(rs.begin(), rs.end(), [](const Run& a, const Run& b) { return a.start < b.start; });
    std::vector<Run> out;
    for (Run& r : rs) {
        if (!out.empty() && r.start < out.back().end) r.start = out.back().end;   // an overlap: the earlier keeps it
        if (r.end <= r.start) continue;
        if (!out.empty() && out.back().end == r.start && out.back().look == r.look) out.back().end = r.end;
        else out.push_back(std::move(r));
    }
    s.runs = std::move(out);
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
    for (Run& r : s.runs) {   // s047: a run's size and shift scale with it
        if (r.look.size) r.look.size = std::clamp(*r.look.size * k, 1.0, 5000.0);
        if (r.look.shift) r.look.shift = *r.look.shift * k;
    }
    s.m = {1, 0, 0, 1, s.m[4], s.m[5]};
    return s;
}

Spec mapped(Spec s, const std::array<double, 6>& a) {
    s.m = compose(a, s.m);
    return folded(s);
}

Spec scaled(Spec s, double sx, double sy, double px, double py) {
    if (!(std::abs(sx) > 1e-6) || !(std::abs(sy) > 1e-6) || !std::isfinite(sx) || !std::isfinite(sy)) return s;
    const M sc{sx, 0, 0, sy, px - sx * px, py - sy * py};   // about (px, py)
    s.m = compose(s.m, sc);
    return folded(s);
}

// ── runs (s047) ─────────────────────────────────────────────────────────────
Look over(Look u, const Look& t) {
    if (t.family) u.family = t.family;
    if (t.weight) u.weight = t.weight;
    if (t.italic) u.italic = t.italic;
    if (t.stretch) u.stretch = t.stretch;
    if (t.size) u.size = t.size;
    if (t.colour) u.colour = t.colour;
    if (t.tracking) u.tracking = t.tracking;
    if (t.shift) u.shift = t.shift;
    if (t.kern) u.kern = t.kern;
    if (t.underline) u.underline = t.underline;
    if (t.strike) u.strike = t.strike;
    if (t.caps) u.caps = t.caps;
    if (t.position) u.position = t.position;
    if (t.ligatures) u.ligatures = t.ligatures;
    if (t.dlig) u.dlig = t.dlig;
    if (t.figures) u.figures = t.figures;
    if (t.fig_space) u.fig_space = t.fig_space;
    if (t.frac) u.frac = t.frac;
    if (t.ordn) u.ordn = t.ordn;
    if (t.swsh) u.swsh = t.swsh;
    if (t.font_kern) u.font_kern = t.font_kern;
    return u;
}

std::string ot_settings(const Look& k) {   // Composix's (s264), over the font's defaults
    std::string out;
    const auto put = [&](const char* tag, int v) { out += (out.empty() ? "" : ",") + std::string(tag) + "=" + std::to_string(v); };
    if (k.ligatures && !*k.ligatures) { put("liga", 0); put("clig", 0); }
    if (k.font_kern && !*k.font_kern) put("kern", 0);
    if (k.figures && *k.figures) { put("lnum", *k.figures == 1); put("onum", *k.figures == 2); }
    if (k.fig_space && *k.fig_space) { put("pnum", *k.fig_space == 1); put("tnum", *k.fig_space == 2); }
    if (k.dlig && *k.dlig) put("dlig", 1);
    if (k.frac && *k.frac) put("frac", 1);
    if (k.ordn && *k.ordn) put("ordn", 1);
    if (k.swsh && *k.swsh) put("swsh", 1);
    return out;
}

namespace {
const Run* run_at(const Spec& s, int byte) {
    for (const Run& r : s.runs) if (byte >= r.start && byte < r.end) return &r;
    return nullptr;
}
}  // namespace

Look look_at(const Spec& s0, int index) {
    const Spec s = normalised(s0);
    Look base;
    base.family = s.family;
    base.weight = s.weight;
    base.italic = s.italic;
    base.stretch = s.stretch;
    base.size = s.size;
    base.colour = std::array<float, 3>{s.colour[0], s.colour[1], s.colour[2]};
    base.tracking = s.tracking;
    base.shift = 0.0;
    base.kern = 0.0;
    base.underline = false;
    base.strike = false;
    base.caps = 0;
    base.position = 0;
    base.ligatures = true;
    base.dlig = false;
    base.figures = 0;
    base.fig_space = 0;
    base.frac = base.ordn = base.swsh = false;
    base.font_kern = true;
    const int n = static_cast<int>(s.text.size());
    if (n == 0) return base;
    index = std::clamp(index, 0, n);
    if (index == n) index = prev_char(s.text, n);
    const Run* r = run_at(s, index);
    return r ? over(base, r->look) : base;
}

Spec styled(Spec s, int a, int b, const Look& add) {
    s = normalised(s);
    const int n = static_cast<int>(s.text.size());
    a = std::clamp(a, 0, n);
    b = std::clamp(b, 0, n);
    if (a > b) std::swap(a, b);
    if (a == b || add.empty()) return s;
    std::vector<int> cuts{a, b};
    for (const Run& r : s.runs) { cuts.push_back(r.start); cuts.push_back(r.end); }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    std::vector<Run> out;
    for (std::size_t k = 0; k + 1 < cuts.size(); ++k) {
        const int p = cuts[k], q = cuts[k + 1];
        const Run* r = run_at(s, p);
        Look lk = r ? r->look : Look{};
        if (p >= a && q <= b) lk = over(lk, add);
        if (!lk.empty()) out.push_back({p, q, lk});
    }
    s.runs = std::move(out);
    return normalised(s);
}

Spec without(Spec s, Field f) {
    for (Run& r : s.runs) {
        Look& k = r.look;
        switch (f) {
            case Field::Family: k.family.reset(); break;
            case Field::Weight: k.weight.reset(); break;
            case Field::Italic: k.italic.reset(); break;
            case Field::Stretch: k.stretch.reset(); break;
            case Field::Size: k.size.reset(); break;
            case Field::Colour: k.colour.reset(); break;
            case Field::Tracking: k.tracking.reset(); break;
            case Field::Shift: k.shift.reset(); break;
            case Field::Kern: k.kern.reset(); break;
            case Field::Underline: k.underline.reset(); break;
            case Field::Strike: k.strike.reset(); break;
            case Field::Caps: k.caps.reset(); break;
            case Field::Position: k.position.reset(); break;
            case Field::Ligatures: k.ligatures.reset(); break;
            case Field::Dlig: k.dlig.reset(); break;
            case Field::Figures: k.figures.reset(); break;
            case Field::FigSpace: k.fig_space.reset(); break;
            case Field::Frac: k.frac.reset(); break;
            case Field::Ordn: k.ordn.reset(); break;
            case Field::Swsh: k.swsh.reset(); break;
            case Field::FontKern: k.font_kern.reset(); break;
        }
    }
    return normalised(s);
}

Spec edited(Spec s, int a, int b, const std::string& ins) {
    s = normalised(s);
    const int n = static_cast<int>(s.text.size());
    a = std::clamp(a, 0, n);
    b = std::clamp(b, 0, n);
    if (a > b) std::swap(a, b);
    // The look the new text takes: the character before it, else the first after.
    Look inh;
    if (const Run* r = a > 0 ? run_at(s, prev_char(s.text, a)) : b < n ? run_at(s, b) : nullptr) inh = r->look;
    inh.kern.reset();
    const int d = static_cast<int>(ins.size()) - (b - a);
    std::vector<Run> out;
    for (const Run& r : s.runs) {
        if (r.start < a) out.push_back({r.start, std::min(r.end, a), r.look});
        if (r.end > b) out.push_back({std::max(r.start, b) + d, r.end + d, r.look});
    }
    if (!ins.empty() && !inh.empty()) out.push_back({a, a + static_cast<int>(ins.size()), inh});
    std::sort(out.begin(), out.end(), [](const Run& x, const Run& y) { return x.start < y.start; });
    s.text.replace(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a), ins);
    s.runs = std::move(out);
    return normalised(s);
}

// ── setting ─────────────────────────────────────────────────────────────────
Coverage render(const Spec& s, int clip_w, int clip_h) {
    Coverage out;
    if (s.text.empty() || clip_w <= 0 || clip_h <= 0) return out;
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    const std::size_t nset = set_lines(n, L);
    if (nset == 0) return out;
    // What the set lines touch: their bands and their ink.
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (std::size_t k = 0; k < nset; ++k) {
        const Placed& p = L.lines[k];
        x0 = std::min({x0, p.x, p.ix0}); x1 = std::max({x1, p.x + p.w, p.ix1});
        y0 = std::min({y0, p.top, p.iy0}); y1 = std::max({y1, p.top + p.lead, p.iy1});
    }
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
    // s047: a run with its own colour: set in colour (ARGB), the Spec's colour
    // under the runs'; else coverage alone (A8) -- the colour exact downstream.
    const bool coloured = std::any_of(n.runs.begin(), n.runs.end(), [](const Run& r) { return r.look.colour.has_value(); });
    cairo_surface_t* sf = cairo_image_surface_create(coloured ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_A8, out.at.w, out.at.h);
    cairo_t* cr = cairo_create(sf);
    cairo_translate(cr, -ix0, -iy0);
    cairo_matrix_t cm;
    cairo_matrix_init(&cm, L.m[0], L.m[1], L.m[2], L.m[3], L.m[4], L.m[5]);
    cairo_transform(cr, &cm);
    cairo_translate(cr, L.ox, L.oy);
    if (coloured) cairo_set_source_rgba(cr, n.colour[0], n.colour[1], n.colour[2], 1);
    else cairo_set_source_rgba(cr, 0, 0, 0, 1);
    for (std::size_t k = 0; k < nset; ++k) {   // each line at its own baseline (s047)
        cairo_move_to(cr, L.lines[k].ox, L.lines[k].base);
        pango_cairo_show_layout_line(cr, pango_layout_get_line_readonly(L.lay, static_cast<int>(k)));
    }
    cairo_destroy(cr);
    cairo_surface_flush(sf);
    const int stride = cairo_image_surface_get_stride(sf);
    const unsigned char* src = cairo_image_surface_get_data(sf);
    out.a.resize(static_cast<std::size_t>(out.at.w) * static_cast<std::size_t>(out.at.h));
    bool any = false;
    if (coloured) {
        out.rgb.assign(out.a.size() * 3, 0);
        for (int y = 0; y < out.at.h; ++y) {
            const auto* row = reinterpret_cast<const std::uint32_t*>(src + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride));
            for (int x = 0; x < out.at.w; ++x) {
                const std::uint32_t px = row[x];
                const std::uint32_t al = px >> 24;
                const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(out.at.w) + static_cast<std::size_t>(x);
                out.a[i] = static_cast<std::uint8_t>(al);
                if (!al) continue;
                any = true;
                for (int c = 0; c < 3; ++c) {   // premultiplied -> straight
                    const std::uint32_t v = (px >> (16 - 8 * c)) & 0xFF;
                    out.rgb[i * 3 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (v * 255 + al / 2) / al));
                }
            }
        }
    }
    for (int y = 0; y < out.at.h && !coloured; ++y)
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
    return set_lines(normalised(s), L) < L.lines.size();
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
    pango_layout_get_cursor_pos(L.lay, L.to_lt(index), &strong, nullptr);
    const double dy = L.lines[static_cast<std::size_t>(line_at_pango_y(L.lines, (strong.y + strong.height / 2.0) / kS))].dy;
    const P a = L.to_image(strong.x / kS, strong.y / kS + dy);
    const P b = L.to_image(strong.x / kS, (strong.y + strong.height) / kS + dy);
    return {a.x, a.y, b.x, b.y};
}

int index_at(const Spec& s, double x, double y) {
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    const P p = L.to_layout(x, y);
    std::size_t k = 0;   // the placed band it falls in (above the first: the first; below the last: the last)
    while (k + 1 < L.lines.size() && p.y >= L.lines[k].top + L.lines[k].lead) ++k;
    const double py = (L.lines[k].py0 + L.lines[k].py1) / 2;
    int idx = 0, trailing = 0;
    pango_layout_xy_to_index(L.lay, static_cast<int>(std::lround(p.x * kS)), static_cast<int>(std::lround(py * kS)), &idx, &trailing);
    return clamp_index(n.text, L.to_sp(forward_chars(L.lt, idx, trailing)));
}

int step(const Spec& s, int index, int dir) {
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    index = clamp_index(n.text, index);
    const int len = static_cast<int>(L.lt.size());
    int li = L.to_lt(index);
    for (int guard = 0; guard < 8; ++guard) {   // a soft hyphen is a stop Pango has and the text doesn't: step past it
        int ni = li, tr = 0;
        pango_layout_move_cursor_visually(L.lay, TRUE, li, 0, dir < 0 ? -1 : 1, &ni, &tr);
        if (ni < 0) return 0;
        if (ni >= len || ni == G_MAXINT) return L.len;
        li = forward_chars(L.lt, ni, tr);
        const int out = clamp_index(n.text, L.to_sp(li));
        if (out != index) return out;
    }
    return index;
}

int line_step(const Spec& s, int index, int dir) {
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    index = clamp_index(n.text, index);
    const auto& b = L.lines;
    const int k = band_of(b, index) + (dir < 0 ? -1 : 1);
    if (k < 0) return 0;
    if (k >= static_cast<int>(b.size())) return L.len;
    PangoRectangle strong;
    pango_layout_get_cursor_pos(L.lay, L.to_lt(index), &strong, nullptr);
    const double ym = (b[static_cast<std::size_t>(k)].py0 + b[static_cast<std::size_t>(k)].py1) / 2;
    int idx = 0, tr = 0;
    pango_layout_xy_to_index(L.lay, strong.x, static_cast<int>(std::lround(ym * kS)), &idx, &tr);
    return clamp_index(n.text, L.to_sp(forward_chars(L.lt, idx, tr)));
}

int line_home(const Spec& s, int index) {
    Laid L;
    lay_out(s, L);
    const auto& b = L.lines;
    return b[static_cast<std::size_t>(band_of(b, clamp_index(normalised(s).text, index)))].start;
}

int line_end(const Spec& s, int index) {
    Laid L;
    lay_out(s, L);
    const Spec n = normalised(s);
    const auto& b = L.lines;
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
    const char* base = L.lt.c_str();   // Pango's characters (soft hyphens in)
    int c = static_cast<int>(g_utf8_pointer_to_offset(base, base + L.to_lt(index)));
    const int chars = na - 1;   // na = chars + 1
    if (dir > 0) {
        for (++c; c < chars && !la[c].is_word_end; ++c) {}
        c = std::min(c, chars);
    } else {
        for (--c; c > 0 && !la[c].is_word_start; --c) {}
        c = std::max(c, 0);
    }
    return clamp_index(n.text, L.to_sp(static_cast<int>(g_utf8_offset_to_pointer(base, c) - base)));
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
    for (const auto& ln : L.lines) {
        const int lo = std::max(a, ln.start), hi = std::min(b, ln.end);
        const bool runs_on = b > ln.end;   // the selection carries past this line's end
        if (lo > hi || (lo == hi && !runs_on)) continue;
        PangoRectangle p0, p1;
        pango_layout_index_to_pos(L.lay, L.to_lt(lo), &p0);
        double x0 = p0.x / kS, x1;
        if (hi >= ln.end) {
            PangoRectangle pe;
            pango_layout_get_cursor_pos(L.lay, L.to_lt(ln.end), &pe, nullptr);
            x1 = pe.x / kS + (runs_on ? n.size * 0.25 : 0);   // a sliver for the line break
        } else {
            pango_layout_index_to_pos(L.lay, L.to_lt(hi), &p1);
            x1 = p1.x / kS;
        }
        if (x1 < x0) std::swap(x0, x1);
        std::array<Box, 4> q{};
        const P c[4] = {{x0, ln.top}, {x1, ln.top}, {x1, ln.top + ln.lead}, {x0, ln.top + ln.lead}};
        for (int k = 0; k < 4; ++k) { const P t = L.to_image(c[k].x, c[k].y); q[static_cast<std::size_t>(k)] = {t.x, t.y, 0, 0}; }
        out.push_back(q);
    }
    return out;
}

namespace {
// The line's largest size (its auto leading's): its runs', and the Spec's
// where no run sets one. An empty line: its break's.
double line_size(const Spec& s, int start, int end) {
    if (end <= start) {
        const Run* r = start > 0 ? run_at(s, prev_char(s.text, start)) : nullptr;
        return r && r->look.size ? *r->look.size : s.size;
    }
    double mx = 0;
    int pos = start;
    bool plain = false;
    for (const Run& r : s.runs) {
        if (r.end <= start || r.start >= end) continue;
        if (!r.look.size) continue;
        if (r.start > pos) plain = true;
        pos = std::max(pos, r.end);
        mx = std::max(mx, *r.look.size);
    }
    if (pos < end) plain = true;
    return plain ? std::max(mx, s.size) : mx;
}
}  // namespace

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

Box to_text(const Spec& s, double x, double y) {
    const P p = apply(inverse(normalised(s).m), {x, y});
    return {p.x, p.y, 0, 0};
}

std::vector<Face> faces(const std::string& family) {
    std::vector<Face> out;
    PangoFontMap* fm = pango_cairo_font_map_get_default();
    PangoFontFamily** fams = nullptr;
    int n = 0;
    pango_font_map_list_families(fm, &fams, &n);
    for (int k = 0; k < n; ++k) {
        if (g_ascii_strcasecmp(pango_font_family_get_name(fams[k]), family.c_str()) != 0) continue;
        PangoFontFace** fcs = nullptr;
        int nf = 0;
        pango_font_family_list_faces(fams[k], &fcs, &nf);
        for (int i = 0; i < nf; ++i) {
            PangoFontDescription* d = pango_font_face_describe(fcs[i]);
            Face f;
            f.name = pango_font_face_get_face_name(fcs[i]);
            f.weight = std::clamp(static_cast<int>(pango_font_description_get_weight(d)), 100, 900);
            f.italic = pango_font_description_get_style(d) != PANGO_STYLE_NORMAL;
            f.stretch = std::clamp(static_cast<int>(pango_font_description_get_stretch(d)), 0, 8);
            f.synthetic = pango_font_face_is_synthesized(fcs[i]);
            pango_font_description_free(d);
            const bool dup = std::any_of(out.begin(), out.end(), [&](const Face& o) {
                return o.weight == f.weight && o.italic == f.italic && o.stretch == f.stretch; });
            if (!dup) out.push_back(f);
            else if (!f.synthetic)   // a real face beats Pango's made one
                for (auto& o : out) if (o.weight == f.weight && o.italic == f.italic && o.stretch == f.stretch && o.synthetic) o = f;
        }
        g_free(fcs);
        break;
    }
    g_free(fams);
    std::sort(out.begin(), out.end(), [](const Face& a, const Face& b) {
        if (a.stretch != b.stretch) return a.stretch < b.stretch;
        if (a.weight != b.weight) return a.weight < b.weight;
        return a.italic < b.italic;
    });
    return out;
}

int nearest_face(const std::vector<Face>& fs, int weight, bool italic, int stretch) {
    int best = -1;
    double bd = 1e18;
    for (std::size_t i = 0; i < fs.size(); ++i) {
        const double d = std::abs(fs[i].weight - weight) + (fs[i].italic != italic ? 1000 : 0) + std::abs(fs[i].stretch - stretch) * 2000.0;
        if (d < bd) { bd = d; best = static_cast<int>(i); }
    }
    return best;
}

bool has_family(const std::string& f) {
    for (const auto& x : families()) if (g_ascii_strcasecmp(x.c_str(), f.c_str()) == 0) return true;
    return false;
}

}  // namespace klimt::text
