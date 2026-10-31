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
#include "core/Text.hpp"

#include <gdkmm/clipboard.h>
#include <gdkmm/display.h>
#include <glibmm/main.h>

#include <algorithm>
#include <cmath>

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
    ContextStrip::TextOpts o;
    o.family = t.family;
    o.weight = t.weight;
    o.italic = t.italic;
    o.size = static_cast<int>(std::lround(t.size));
    o.colour = t.colour;
    o.align = static_cast<int>(t.align);
    o.tracking = static_cast<int>(std::lround(t.tracking));
    o.leading = static_cast<int>(std::lround(t.leading));
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
    const auto f = [o, what](T::Spec& s) {
        if (what == "family") s.family = o.family;
        else if (what == "weight") s.weight = o.weight;
        else if (what == "italic") s.italic = o.italic;
        else if (what == "size") s.size = o.size;
        else if (what == "colour") s.colour = o.colour;
        else if (what == "align") s.align = static_cast<T::Align>(std::clamp(o.align, 0, 3));
        else if (what == "tracking") s.tracking = o.tracking;
        else if (what == "leading") s.leading = o.leading;
    };
    static const std::map<std::string, std::string> labels = {
        {"family", "Font"}, {"weight", "Font Weight"}, {"italic", "Italic"}, {"size", "Font Size"}, {"colour", "Text Colour"},
        {"align", "Align Text"}, {"tracking", "Tracking"}, {"leading", "Leading"}};
    const auto it = labels.find(what);
    L::set_text_on(m_journal, m_doc, ids, f, it == labels.end() ? "Type" : it->second, "text:" + what);
    if (m_te.on) text_show();
}

// ── a visit ─────────────────────────────────────────────────────────────────

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
    m_te = {};
    const bool kept = L::text_end(m_journal, m_doc, id, made);
    m_canvas.set_text_overlay({});
    update_layer_actions();
    if (auto lg = log::get(log::Area::Shell)) lg->info("text: visit ends ({})", kept ? "a step" : "nothing");
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
    for (const auto& q : T::range(s, std::min(m_te.caret, m_te.anchor), std::max(m_te.caret, m_te.anchor))) o.sel.push_back(quad_of(q));
    const auto c = T::caret(s, m_te.caret);
    o.caret = m_te.blink;
    o.c0 = {c.x0, c.y0};
    o.c1 = {c.x1, c.y1};
    m_canvas.set_text_overlay(o);
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
    T::Spec s = *l->text;
    const int a = std::min(m_te.caret, m_te.anchor), b = std::max(m_te.caret, m_te.anchor);
    s.text.replace(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a), add);
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
    T::Spec s = *l->text;
    a = std::clamp(a, 0, static_cast<int>(s.text.size()));
    b = std::clamp(b, 0, static_cast<int>(s.text.size()));
    if (a == b) return;
    s.text.erase(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
    L::set_text(m_journal, m_doc, m_te.id, s);
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
    if (ctrl && !alt) {
        switch (k) {
            case GDK_KEY_a: case GDK_KEY_A:
                m_te.anchor = 0; m_te.caret = len; text_show(); return true;
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
    if (m_te.on) { m_te.selecting = false; return; }
    if (!m_te.pressing) return;
    m_te.pressing = false;
    const auto o = m_strip.text();
    T::Spec s;
    s.family = o.family;
    s.weight = o.weight;
    s.italic = o.italic;
    s.size = o.size;
    s.colour = o.colour;
    s.align = static_cast<T::Align>(std::clamp(o.align, 0, 3));
    s.tracking = o.tracking;
    s.leading = o.leading;
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
