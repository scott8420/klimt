// Shell -- the TEXT TOOL (s046; docs/text.md). Category: text.
//
// A click on type edits it; a click elsewhere sets new point type there (its
// baseline); a drag draws a box for box type. While a VISIT is open the keys
// go to the type through an input method, and the journal's gesture holds
// every change: the visit lands as ONE step ("Type “…”", or "New Text Layer
// “…”"), and a new layer left empty leaves nothing. The strip's options apply
// to the edited layer (or the picked text layers) and set new type.
#include "Shell.hpp"
#include "Log.hpp"
#include "core/Shortcuts.hpp"   // s048: text_accels
#include "core/Text.hpp"

#include <gdkmm/clipboard.h>
#include <gdkmm/display.h>
#include <glibmm/main.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace klimt {

namespace L = core::layers;
namespace T = klimt::text;

namespace {
// Is (x, y) inside the quad (clockwise or not), grown by `pad` px?
bool in_quad(const std::array<T::Box, 4>& q, double x, double y, double pad) {
    // Grow by testing against each edge's line moved outwards by pad.
    double area = 0;
    for (int k = 0; k < 4; ++k) {
        const auto& a = q[static_cast<std::size_t>(k)];
        const auto& b = q[static_cast<std::size_t>((k + 1) % 4)];
        area += a.x * b.y - b.x * a.y;
    }
    const double sgn = area >= 0 ? 1 : -1;
    for (int k = 0; k < 4; ++k) {
        const auto& a = q[static_cast<std::size_t>(k)];
        const auto& b = q[static_cast<std::size_t>((k + 1) % 4)];
        const double ex = b.x - a.x, ey = b.y - a.y, len = std::hypot(ex, ey);
        if (len < 1e-9) continue;
        const double cross = (ex * (y - a.y) - ey * (x - a.x)) * sgn / len;   // >0 inside
        if (cross < -pad) return false;
    }
    return true;
}
CanvasView::TextQuad quad_of(const std::array<T::Box, 4>& q) {
    CanvasView::TextQuad out;
    for (std::size_t k = 0; k < 4; ++k) out.c[k] = {q[k].x, q[k].y};
    return out;
}
std::int64_t now_ms() { return g_get_monotonic_time() / 1000; }
}  // namespace

// ── the strip <-> the type ──────────────────────────────────────────────────

void Shell::text_follow() {
    std::string id = m_te.on ? m_te.id : m_layers.primary();
    const auto l = m_doc.get(id);
    if (!l || l->kind != core::Kind::Text || !l->text) return;
    const auto& t = *l->text;
    // s047: while editing, the look of the letters at the caret (the first
    // selected, else the one before the caret -- what typing continues).
    int at = 0;
    if (m_te.on) {
        const int a = std::min(m_te.caret, m_te.anchor), b = std::max(m_te.caret, m_te.anchor);
        at = a != b ? a : a > 0 ? T::prev_char(t.text, a) : 0;
    }
    const T::Look k = T::look_at(t, at);
    ContextStrip::TextOpts o;
    o.family = *k.family;
    o.weight = *k.weight;
    o.italic = *k.italic;
    o.stretch = *k.stretch;
    o.size = static_cast<int>(std::lround(*k.size));
    o.colour = {(*k.colour)[0], (*k.colour)[1], (*k.colour)[2], t.colour[3]};
    o.align = static_cast<int>(t.align);
    o.tracking = static_cast<int>(std::lround(*k.tracking));
    o.leading = static_cast<int>(std::lround(t.leading));
    o.underline = *k.underline;
    o.strike = *k.strike;
    o.shift = static_cast<int>(std::lround(*k.shift));
    o.caps = *k.caps;   // s048
    o.position = *k.position;
    o.ligatures = *k.ligatures;
    o.dlig = *k.dlig;
    o.figures = *k.figures;
    o.fig_space = *k.fig_space;
    o.frac = *k.frac;
    o.ordn = *k.ordn;
    o.swsh = *k.swsh;
    o.font_kern = *k.font_kern;
    o.hyphenate = t.hyphenate;
    o.lang = t.lang;
    m_strip.show_text(o);
}

void Shell::on_text_style(const std::string& what) {
    std::vector<std::string> ids;
    if (m_te.on) ids = {m_te.id};
    else
        for (const auto& id : m_layers.selection())
            if (const auto l = m_doc.get(id); l && l->kind == core::Kind::Text) ids.push_back(id);
    if (ids.empty()) return;   // only what new type starts from
    const auto o = m_strip.text();
    static const std::map<std::string, std::string> labels = {
        {"family", "Font"}, {"face", "Font Face"}, {"weight", "Font Weight"}, {"italic", "Italic"}, {"size", "Font Size"}, {"colour", "Text Colour"},
        {"align", "Align Text"}, {"tracking", "Tracking"}, {"leading", "Leading"},
        {"underline", "Underline"}, {"strike", "Strikethrough"}, {"shift", "Baseline Shift"},
        {"caps", "Caps"}, {"position", "Super / Subscript"}, {"ligatures", "Ligatures"}, {"dlig", "Discretionary Ligatures"},
        {"figures", "Figures"}, {"fig_space", "Figure Width"}, {"frac", "Fractions"}, {"ordn", "Ordinals"}, {"swsh", "Swashes"},
        {"font_kern", "Font Kerning"}, {"hyphenate", "Hyphenate"}, {"lang", "Language"}};
    const auto it = labels.find(what);
    const std::string label = it == labels.end() ? "Type" : it->second;
    // s047: the letters' look (a run) -- only a character setting; align and
    // leading are the layer's.
    T::Look add;
    std::optional<T::Field> field;
    if (what == "family") { add.family = o.family; field = T::Field::Family; }
    else if (what == "weight") { add.weight = o.weight; field = T::Field::Weight; }
    else if (what == "face") { add.weight = o.weight; add.italic = o.italic; add.stretch = o.stretch; field = T::Field::Weight; }
    else if (what == "italic") { add.italic = o.italic; field = T::Field::Italic; }
    else if (what == "size") { add.size = o.size; field = T::Field::Size; }
    else if (what == "colour") { add.colour = std::array<float, 3>{o.colour[0], o.colour[1], o.colour[2]}; field = T::Field::Colour; }
    else if (what == "tracking") { add.tracking = o.tracking; field = T::Field::Tracking; }
    else if (what == "underline") { add.underline = o.underline; field = T::Field::Underline; }
    else if (what == "strike") { add.strike = o.strike; field = T::Field::Strike; }
    else if (what == "shift") { add.shift = o.shift; field = T::Field::Shift; }
    else if (what == "caps") { add.caps = o.caps; field = T::Field::Caps; }   // s048
    else if (what == "position") { add.position = o.position; field = T::Field::Position; }
    else if (what == "ligatures") { add.ligatures = o.ligatures; field = T::Field::Ligatures; }
    else if (what == "dlig") { add.dlig = o.dlig; field = T::Field::Dlig; }
    else if (what == "figures") { add.figures = o.figures; field = T::Field::Figures; }
    else if (what == "fig_space") { add.fig_space = o.fig_space; field = T::Field::FigSpace; }
    else if (what == "frac") { add.frac = o.frac; field = T::Field::Frac; }
    else if (what == "ordn") { add.ordn = o.ordn; field = T::Field::Ordn; }
    else if (what == "swsh") { add.swsh = o.swsh; field = T::Field::Swsh; }
    else if (what == "font_kern") { add.font_kern = o.font_kern; field = T::Field::FontKern; }
    const int sa = std::min(m_te.caret, m_te.anchor), sb = std::max(m_te.caret, m_te.anchor);
    if (m_te.on && sa != sb && field) {   // the selected letters
        const auto l = m_doc.get(m_te.id);
        if (!l || !l->text) return;
        L::set_text(m_journal, m_doc, m_te.id, T::styled(*l->text, sa, sb, add), label, "text:" + what);
        text_show();
        return;
    }
    // The whole layer: the Spec's own setting, and no run keeps its own
    // (colour's alpha is the layer's, always).
    const auto f = [o, what, field, add](T::Spec& s) {
        if (what == "family") s.family = o.family;
        else if (what == "weight") s.weight = o.weight;
        else if (what == "italic") s.italic = o.italic;
        else if (what == "face") {
            s.weight = o.weight; s.italic = o.italic; s.stretch = o.stretch;
            s = T::without(T::without(s, T::Field::Italic), T::Field::Stretch);
        }
        else if (what == "size") s.size = o.size;
        else if (what == "colour") s.colour = o.colour;
        else if (what == "align") s.align = static_cast<T::Align>(std::clamp(o.align, 0, 3));
        else if (what == "tracking") s.tracking = o.tracking;
        else if (what == "leading") s.leading = o.leading;
        else if (what == "hyphenate") s.hyphenate = o.hyphenate;   // s048: the layer's
        else if (what == "lang") s.lang = o.lang;
        if (field) s = T::without(s, *field);
        // underline / strike / shift have no layer-wide field: a run over all of it
        // (s048: nor have caps, scripts or the features)
        static const std::set<std::string> lettered = {"underline", "strike", "shift", "caps", "position", "ligatures", "dlig",
                                                       "figures", "fig_space", "frac", "ordn", "swsh", "font_kern"};
        if (lettered.count(what)) s = T::styled(s, 0, static_cast<int>(s.text.size()), add);
    };
    L::set_text_on(m_journal, m_doc, ids, f, label, "text:" + what);
    if (m_te.on) text_show();
}

// ── a visit ─────────────────────────────────────────────────────────────────

// s048: the application's accelerators fire before the canvas hears a key,
// so while type is edited the ones the type needs are lent to it: Zoom In's
// Ctrl+= / Ctrl++ (Ctrl+Shift+= is superscript) -- the keypad's + still zooms.
void Shell::text_accels(bool visiting) {
    auto app = get_application();
    if (!app) return;
    for (const auto& s : core::shortcut_registry()) {
        if (s.action != "win.zoom-in") continue;
        std::vector<Glib::ustring> keep;
        for (const auto& a : s.accels)
            if (!visiting || (a != "<Ctrl>plus" && a != "<Ctrl>equal")) keep.emplace_back(a);
        app->set_accels_for_action(s.action, keep);
    }
}

void Shell::text_ime(bool on) {
    if (!m_im) {
        m_im = gtk_im_multicontext_new();
        gtk_im_context_set_client_widget(m_im, GTK_WIDGET(m_canvas.gobj()));
        g_signal_connect(m_im, "commit", G_CALLBACK(+[](GtkIMContext*, const char* s, gpointer self) {
            auto* sh = static_cast<Shell*>(self);
            if (sh->m_te.on && s) sh->text_insert(s);
        }), this);
    }
    if (!m_te_keys) return;
    if (on) {
        gtk_event_controller_key_set_im_context(m_te_keys->gobj(), m_im);
        gtk_im_context_focus_in(m_im);
    } else {
        gtk_im_context_reset(m_im);
        gtk_im_context_focus_out(m_im);
        gtk_event_controller_key_set_im_context(m_te_keys->gobj(), nullptr);
    }
}

void Shell::text_open(const std::string& id, bool made) {
    const auto l = m_doc.get(id);
    if (!l || l->kind != core::Kind::Text) return;
    if (m_layers.primary() != id) m_layers.select({id}, false);   // before the visit: on_selection would end it
    if (!made) L::text_begin(m_journal, m_doc, id);
    m_te.on = true;
    m_te.id = id;
    m_te.made = made;
    m_te.blink = true;
    text_ime(true);
    text_accels(true);   // s048
    m_canvas.grab_focus();
    m_te_blink.disconnect();
    m_te_blink = Glib::signal_timeout().connect([this] {
        if (!m_te.on) return false;
        m_te.blink = !m_te.blink;
        text_show();
        return true;
    }, 530);
    text_follow();
    text_show();
    if (auto lg = log::get(log::Area::Shell)) lg->info("text: visit {} ({})", id, made ? "new" : "edit");
}

void Shell::text_close() {
    if (!m_te.on) return;
    const auto id = m_te.id;
    const bool made = m_te.made;
    m_te_blink.disconnect();
    text_ime(false);
    text_accels(false);   // s048
    m_te = {};
    const bool kept = L::text_end(m_journal, m_doc, id, made);
    m_canvas.set_text_overlay({});
    update_layer_actions();
    if (auto lg = log::get(log::Area::Shell)) lg->info("text: visit ends ({})", kept ? "a step" : "nothing");
}

// s047: box type's eight handles, image px: 0 TL, 1 T, 2 TR, 3 R, 4 BR, 5 B, 6 BL, 7 L.
std::vector<CanvasView::Cross> Shell::text_handles(const T::Spec& s) const {
    const auto f = T::frame(s);
    const auto mid = [](const T::Box& a, const T::Box& b) { return CanvasView::Cross{(a.x + b.x) / 2, (a.y + b.y) / 2}; };
    return {{f[0].x, f[0].y}, mid(f[0], f[1]), {f[1].x, f[1].y}, mid(f[1], f[2]),
            {f[2].x, f[2].y}, mid(f[2], f[3]), {f[3].x, f[3].y}, mid(f[3], f[0])};
}
int Shell::text_handle_at(const T::Spec& s, double x, double y) const {
    const double r = 7 / std::max(0.01, m_canvas.zoom());
    const auto hs = text_handles(s);
    int best = -1;
    double bd = r;
    for (std::size_t k = 0; k < hs.size(); ++k)
        if (const double d = std::hypot(hs[k].x - x, hs[k].y - y); d <= bd) { bd = d; best = static_cast<int>(k); }
    return best;
}

std::string Shell::text_hit(double x, double y) const {
    const double pad = 4 / std::max(0.01, m_canvas.zoom());
    for (const auto& id : m_doc.walk()) {   // top first
        const auto l = m_doc.get(id);
        if (!l || l->kind != core::Kind::Text || !l->text || !m_doc.shown(id)) continue;
        if (in_quad(T::frame(*l->text), x, y, pad)) return id;
    }
    return {};
}

void Shell::text_show() {
    const auto l = m_te.on ? m_doc.get(m_te.id) : std::nullopt;
    if (!l || !l->text) { m_canvas.set_text_overlay({}); return; }
    const auto& s = *l->text;
    CanvasView::TextOverlay o;
    o.on = true;
    o.frame_on = true;
    o.frame = quad_of(T::frame(s));
    o.overflow = T::overflows(s);
    o.handles = text_handles(s);   // s047 box type's; s048 point type's too
    for (const auto& q : T::range(s, std::min(m_te.caret, m_te.anchor), std::max(m_te.caret, m_te.anchor))) o.sel.push_back(quad_of(q));
    const auto c = T::caret(s, m_te.caret);
    o.caret = m_te.blink;
    o.c0 = {c.x0, c.y0};
    o.c1 = {c.x1, c.y1};
    m_canvas.set_text_overlay(o);
    text_follow();   // s047: the strip shows the caret's look
    if (m_im) {   // the input method's popup beside the caret
        GdkRectangle r;
        r.x = static_cast<int>(m_canvas.screen_x_of(std::min(c.x0, c.x1)));
        r.y = static_cast<int>(m_canvas.screen_y_of(std::min(c.y0, c.y1)));
        r.width = 1;
        r.height = std::max(1, static_cast<int>(std::abs(m_canvas.screen_y_of(c.y1) - m_canvas.screen_y_of(c.y0))));
        gtk_im_context_set_cursor_location(m_im, &r);
    }
}

// ── editing ─────────────────────────────────────────────────────────────────

void Shell::text_insert(const std::string& in) {
    const auto l = m_te.on ? m_doc.get(m_te.id) : std::nullopt;
    if (!l || !l->text) return;
    std::string add;
    for (char ch : in) if (ch != '\r') add += ch;   // pasted CRLF: one break
    const int a = std::min(m_te.caret, m_te.anchor), b = std::max(m_te.caret, m_te.anchor);
    const T::Spec s = T::edited(*l->text, a, b, add);   // s047: the runs carried
    if (!T::refusal(s).empty()) return;
    L::set_text(m_journal, m_doc, m_te.id, s);
    m_te.caret = m_te.anchor = a + static_cast<int>(add.size());
    m_te.blink = true;
    text_show();
}

void Shell::text_erase(int a, int b) {
    const auto l = m_te.on ? m_doc.get(m_te.id) : std::nullopt;
    if (!l || !l->text) return;
    if (a > b) std::swap(a, b);
    const int n = static_cast<int>(l->text->text.size());
    a = std::clamp(a, 0, n);
    b = std::clamp(b, 0, n);
    if (a == b) return;
    L::set_text(m_journal, m_doc, m_te.id, T::edited(*l->text, a, b, {}));   // s047: the runs carried
    m_te.caret = m_te.anchor = a;
    m_te.blink = true;
    text_show();
}

bool Shell::text_key(guint k, Gdk::ModifierType state) {
    if (!m_te.on) return false;
    const auto l = m_doc.get(m_te.id);
    if (!l || !l->text) { text_close(); return false; }
    const auto& s = *l->text;
    const int len = static_cast<int>(s.text.size());
    const bool ctrl = (state & Gdk::ModifierType::CONTROL_MASK) != Gdk::ModifierType{};
    const bool shift = (state & Gdk::ModifierType::SHIFT_MASK) != Gdk::ModifierType{};
    const bool alt = (state & Gdk::ModifierType::ALT_MASK) != Gdk::ModifierType{};
    const int a = std::min(m_te.caret, m_te.anchor), b = std::max(m_te.caret, m_te.anchor);
    const auto go = [&](int to) {
        m_te.caret = std::clamp(to, 0, len);
        if (!shift) m_te.anchor = m_te.caret;
        m_te.blink = true;
        text_show();
        return true;
    };
    // s047: Alt+Left / Right -- kern the pair at the caret (Composix's kern:
    // space after the character before it), or track the selection; 20/1000
    // em a press, 100 with Ctrl (Photoshop's steps).
    if (alt && (k == GDK_KEY_Left || k == GDK_KEY_Right || k == GDK_KEY_KP_Left || k == GDK_KEY_KP_Right)) {
        const double d = ((k == GDK_KEY_Left || k == GDK_KEY_KP_Left) ? -1 : 1) * (ctrl ? 100 : 20);
        if (a != b) {
            T::Spec n = s;
            for (int i = a; i < b; i = T::next_char(s.text, i)) {   // each letter's own tracking, moved
                const int j = T::next_char(s.text, i);
                T::Look lk;
                lk.tracking = std::clamp(*T::look_at(s, i).tracking + d, -1000.0, 5000.0);
                n = T::styled(n, i, j, lk);
            }
            L::set_text(m_journal, m_doc, m_te.id, n, "Tracking", "text:tracking");
        } else if (m_te.caret > 0) {
            const int i = T::prev_char(s.text, m_te.caret);
            T::Look lk;
            lk.kern = std::clamp(*T::look_at(s, i).kern + d, -1000.0, 5000.0);
            L::set_text(m_journal, m_doc, m_te.id, T::styled(s, i, m_te.caret, lk), "Kerning", "text:kern");
        }
        m_te.blink = true;
        text_show();
        return true;
    }
    switch (k) {
        case GDK_KEY_Escape: text_close(); return true;
        case GDK_KEY_KP_Enter: text_close(); return true;
        case GDK_KEY_Return:
            if (ctrl) { text_close(); return true; }
            text_insert("\n");
            return true;
        case GDK_KEY_BackSpace:
            if (a != b) text_erase(a, b);
            else text_erase(ctrl ? T::word_step(s, m_te.caret, -1) : T::prev_char(s.text, m_te.caret), m_te.caret);
            return true;
        case GDK_KEY_Delete: case GDK_KEY_KP_Delete:
            if (a != b) text_erase(a, b);
            else text_erase(m_te.caret, ctrl ? T::word_step(s, m_te.caret, 1) : T::next_char(s.text, m_te.caret));
            return true;
        case GDK_KEY_Left: case GDK_KEY_KP_Left:
            if (a != b && !shift) { m_te.caret = m_te.anchor = a; text_show(); return true; }
            return go(ctrl ? T::word_step(s, m_te.caret, -1) : T::step(s, m_te.caret, -1));
        case GDK_KEY_Right: case GDK_KEY_KP_Right:
            if (a != b && !shift) { m_te.caret = m_te.anchor = b; text_show(); return true; }
            return go(ctrl ? T::word_step(s, m_te.caret, 1) : T::step(s, m_te.caret, 1));
        case GDK_KEY_Up: case GDK_KEY_KP_Up:     return go(T::line_step(s, m_te.caret, -1));
        case GDK_KEY_Down: case GDK_KEY_KP_Down: return go(T::line_step(s, m_te.caret, 1));
        case GDK_KEY_Home: case GDK_KEY_KP_Home: return go(ctrl ? 0 : T::line_home(s, m_te.caret));
        case GDK_KEY_End: case GDK_KEY_KP_End:   return go(ctrl ? len : T::line_end(s, m_te.caret));
        case GDK_KEY_Tab: text_insert("\t"); return true;
        default: break;
    }
    // s048: Photoshop's keys -- Ctrl+Shift+K All Caps, Ctrl+Shift+H Small
    // Caps, Ctrl+Shift+= superscript (with Alt: subscript); again: back.
    if (ctrl && shift) {
        const T::Look at = T::look_at(s, a != b ? a : a > 0 ? T::prev_char(s.text, a) : 0);
        ContextStrip::TextOpts o = m_strip.text();
        std::string what;
        if (k == GDK_KEY_K || k == GDK_KEY_k) { o.caps = *at.caps == 1 ? 0 : 1; what = "caps"; }
        else if (k == GDK_KEY_H || k == GDK_KEY_h) { o.caps = *at.caps == 2 ? 0 : 2; what = "caps"; }
        else if (k == GDK_KEY_plus || k == GDK_KEY_equal) {
            const int want = alt ? 2 : 1;
            o.position = *at.position == want ? 0 : want;
            what = "position";
        }
        if (!what.empty()) {
            m_strip.show_text(o);
            on_text_style(what);
            return true;
        }
    }
    if (ctrl && !alt) {
        switch (k) {
            case GDK_KEY_a: case GDK_KEY_A:
                m_te.anchor = 0; m_te.caret = len; text_show(); return true;
            case GDK_KEY_b: case GDK_KEY_B: case GDK_KEY_i: case GDK_KEY_I: case GDK_KEY_u: case GDK_KEY_U: {
                // s047: Bold / Italic / Underline -- the selected letters, else the layer
                const T::Look at = T::look_at(s, a != b ? a : a > 0 ? T::prev_char(s.text, a) : 0);
                ContextStrip::TextOpts o = m_strip.text();
                std::string what;
                if (k == GDK_KEY_b || k == GDK_KEY_B) {
                    const auto fs = T::faces(*at.family);
                    const bool bold = *at.weight >= 600;
                    o.weight = bold ? 400 : 700;
                    if (const int f = T::nearest_face(fs, o.weight, *at.italic, *at.stretch); f >= 0) o.weight = fs[static_cast<std::size_t>(f)].weight;
                    if (bold == (o.weight >= 600)) o.weight = bold ? 400 : 700;   // no such face: Pango's nearest
                    o.italic = *at.italic; o.stretch = *at.stretch;
                    what = "face";
                } else if (k == GDK_KEY_i || k == GDK_KEY_I) {
                    o.weight = *at.weight; o.italic = !*at.italic; o.stretch = *at.stretch;
                    what = "face";
                } else {
                    o.underline = !*at.underline;
                    what = "underline";
                }
                m_strip.show_text(o);
                on_text_style(what);
                return true;
            }
            case GDK_KEY_c: case GDK_KEY_C: case GDK_KEY_x: case GDK_KEY_X:
                if (a == b) return true;
                get_clipboard()->set_text(s.text.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a)));
                if (k == GDK_KEY_x || k == GDK_KEY_X) text_erase(a, b);
                return true;
            case GDK_KEY_v: case GDK_KEY_V: {
                const auto id = m_te.id;
                auto cb = get_clipboard();
                cb->read_text_async([this, cb, id](Glib::RefPtr<Gio::AsyncResult>& r) {
                    try {
                        const auto t = cb->read_text_finish(r);
                        if (m_te.on && m_te.id == id && !t.empty()) text_insert(t);
                    } catch (...) {}
                });
                return true;
            }
            case GDK_KEY_z: case GDK_KEY_Z: case GDK_KEY_y: case GDK_KEY_Y:
                return false;   // Undo / Redo end the visit first (on_undo), then take it back whole
            default: return false;   // Ctrl+S and the rest: the window's
        }
    }
    // Anything else the input method didn't take (F-keys, bare modifiers):
    // not ours, but a plain key never reaches the tool letters while typing.
    const gunichar ch = gdk_keyval_to_unicode(k);
    return ch != 0 && !alt;
}

// ── the tool on the canvas ──────────────────────────────────────────────────

void Shell::on_text(CanvasView::TextAt at, double x, double y, bool shift) {
    using At = CanvasView::TextAt;
    if (!has_image()) return;
    if (at == At::Press) {
        if (m_te.on) {
            const auto l = m_doc.get(m_te.id);
            const double pad = 6 / std::max(0.01, m_canvas.zoom());
            // s047: a box's handle -- resize it (inside the visit: one step with it)
            if (l && l->text) {   // s048: point type's too (they scale it)
                const int h = text_handle_at(*l->text, x, y);
                if (h >= 0) {
                    m_te.handle = h;
                    m_te.hspec = *l->text;
                    m_te.hshift = shift;
                    return;
                }
            }
            if (l && l->text && in_quad(T::frame(*l->text), x, y, pad)) {
                const auto& s = *l->text;
                const auto t = now_ms();
                m_te.clicks = t - m_te.last_ms < 400 ? m_te.clicks + 1 : 1;
                m_te.last_ms = t;
                const int i = T::index_at(s, x, y);
                if (m_te.clicks == 2) {   // a word
                    m_te.anchor = T::word_step(s, T::next_char(s.text, i), -1);
                    m_te.caret = T::word_step(s, i, 1);
                } else if (m_te.clicks >= 3) {   // everything
                    m_te.anchor = 0;
                    m_te.caret = static_cast<int>(s.text.size());
                } else {
                    m_te.caret = i;
                    if (!shift) m_te.anchor = i;
                    m_te.selecting = true;
                }
                m_te.blink = true;
                text_show();
                return;
            }
            text_close();   // a click away ends the visit -- and may start another
        }
        const auto hit = text_hit(x, y);
        if (!hit.empty()) {
            if (m_doc.locked_all(hit)) { toast("“" + m_doc.get(hit)->name + "” is locked"); return; }
            text_open(hit, false);
            const auto l = m_doc.get(hit);
            m_te.caret = m_te.anchor = T::index_at(*l->text, x, y);
            m_te.selecting = true;
            m_te.last_ms = now_ms();
            m_te.clicks = 1;
            text_show();
            return;
        }
        m_te.pressing = true;
        m_te.boxing = false;
        m_te.px = x;
        m_te.py = y;
        return;
    }
    if (at == At::Drag) {
        if (m_te.on && m_te.handle >= 0 && m_te.hspec.point()) {
            // s048: point type -- scaled about the opposite handle, in text space.
            // A corner keeps the proportions (Shift: free); a side stretches.
            const T::Spec& s0 = m_te.hspec;
            const auto f = T::frame(s0);
            const auto t0 = T::to_text(s0, f[0].x, f[0].y), t2 = T::to_text(s0, f[2].x, f[2].y);
            const double x0 = std::min(t0.x, t2.x), x1 = std::max(t0.x, t2.x), y0 = std::min(t0.y, t2.y), y1 = std::max(t0.y, t2.y);
            const int h = m_te.handle;   // 0 TL, 1 T, 2 TR, 3 R, 4 BR, 5 B, 6 BL, 7 L
            const double hx = (h == 0 || h == 6 || h == 7) ? x0 : (h == 2 || h == 3 || h == 4) ? x1 : (x0 + x1) / 2;
            const double hy = (h == 0 || h == 1 || h == 2) ? y0 : (h == 4 || h == 5 || h == 6) ? y1 : (y0 + y1) / 2;
            const double px = (h == 0 || h == 6 || h == 7) ? x1 : (h == 2 || h == 3 || h == 4) ? x0 : (x0 + x1) / 2;
            const double py = (h == 0 || h == 1 || h == 2) ? y1 : (h == 4 || h == 5 || h == 6) ? y0 : (y0 + y1) / 2;
            const auto p = T::to_text(s0, x, y);
            const bool corner = h % 2 == 0, sideways = h == 3 || h == 7, upright = h == 1 || h == 5;
            double sx = 1, sy = 1;
            if (!upright && std::abs(hx - px) > 1e-6) sx = (p.x - px) / (hx - px);
            if (!sideways && std::abs(hy - py) > 1e-6) sy = (p.y - py) / (hy - py);
            if (corner && !m_te.hshift && !shift) {   // proportional: the pointer projected on the diagonal
                const double dx = hx - px, dy = hy - py, d2 = dx * dx + dy * dy;
                const double k = d2 > 1e-9 ? ((p.x - px) * dx + (p.y - py) * dy) / d2 : 1;
                sx = sy = k;
            }
            sx = std::max(0.02, sx);
            sy = std::max(0.02, sy);
            L::set_text(m_journal, m_doc, m_te.id, T::scaled(s0, sx, sy, px, py), "Scale Type", "text:scale");
            m_te.blink = true;
            text_show();
            return;
        }
        if (m_te.on && m_te.handle >= 0) {
            T::Spec s = m_te.hspec;
            const auto p = T::to_text(s, x, y);
            double x0 = s.x, y0 = s.y, x1 = s.x + s.box_w;
            double y1 = s.y + (s.box_h > 0 ? s.box_h : T::frame(s)[2].y - T::frame(s)[0].y);
            const int h = m_te.handle;   // 0 TL, 1 T, 2 TR, 3 R, 4 BR, 5 B, 6 BL, 7 L
            constexpr double kMin = 8;
            if (h == 0 || h == 6 || h == 7) x0 = std::min(p.x, x1 - kMin);
            if (h == 2 || h == 3 || h == 4) x1 = std::max(p.x, x0 + kMin);
            if (h == 0 || h == 1 || h == 2) y0 = std::min(p.y, y1 - kMin);
            if (h == 4 || h == 5 || h == 6) y1 = std::max(p.y, y0 + kMin);
            s.x = x0; s.y = y0; s.box_w = x1 - x0; s.box_h = y1 - y0;
            L::set_text(m_journal, m_doc, m_te.id, s, "Resize Text Box", "text:box");
            m_te.blink = true;
            text_show();
            return;
        }
        if (m_te.on && m_te.selecting) {
            const auto l = m_doc.get(m_te.id);
            if (l && l->text) { m_te.caret = T::index_at(*l->text, x, y); m_te.blink = true; text_show(); }
            return;
        }
        if (m_te.pressing) {
            const double z = m_canvas.zoom();
            if (!m_te.boxing && std::hypot(x - m_te.px, y - m_te.py) * z >= 6) m_te.boxing = true;
            if (m_te.boxing) {
                CanvasView::TextOverlay o;
                o.on = true;
                o.box = true;
                o.b0 = {m_te.px, m_te.py};
                o.b1 = {x, y};
                m_canvas.set_text_overlay(o);
            }
        }
        return;
    }
    // Release.
    if (m_te.on) { m_te.selecting = false; m_te.handle = -1; return; }
    if (!m_te.pressing) return;
    m_te.pressing = false;
    const auto o = m_strip.text();
    T::Spec s;
    s.family = o.family;
    s.weight = o.weight;
    s.italic = o.italic;
    s.stretch = o.stretch;
    s.size = o.size;
    s.colour = o.colour;
    s.align = static_cast<T::Align>(std::clamp(o.align, 0, 3));
    s.tracking = o.tracking;
    s.leading = o.leading;
    s.hyphenate = o.hyphenate;   // s048
    s.lang = o.lang;
    if (m_te.boxing) {
        s.x = std::min(m_te.px, x);
        s.y = std::min(m_te.py, y);
        s.box_w = std::max(8.0, std::abs(x - m_te.px));
        s.box_h = std::max(8.0, std::abs(y - m_te.py));
    } else {
        if (s.align == T::Align::Justify) s.align = T::Align::Left;   // point type has no width to fill
        s.x = m_te.px;
        s.y = m_te.py;
    }
    m_te.boxing = false;
    m_canvas.set_text_overlay({});
    L::text_begin(m_journal, m_doc, {});
    const auto id = L::add_text(m_journal, m_doc, s, m_layers.primary());
    if (id.empty()) { m_journal.cancel(); toast("Text: can't set type here"); return; }
    text_open(id, true);
    m_te.caret = m_te.anchor = 0;
    text_show();
}

}  // namespace klimt
