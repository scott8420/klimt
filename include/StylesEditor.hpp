#pragma once
// StylesEditor -- a layer's STYLES in Properties (s019): Drop Shadow, Outer
// Glow, Inner Shadow, Color Overlay, Stroke -- each a row with its box (on /
// off) that opens to its controls (colour, opacity, size, distance, angle,
// spread, position). It writes nothing: it says what changed (`changed`) and
// when a slider drag starts and ends (`begin` / `end`), so Properties makes a
// drag ONE step. docs/styles.md.
#include "core/Styles.hpp"

#include <gtkmm/box.h>
#include <gtkmm/scale.h>
#include <sigc++/signal.h>

#include <functional>
#include <vector>

namespace klimt {

class StylesEditor : public Gtk::Box {
public:
    StylesEditor();
    void set(const core::Styles& s);              // repaint from the document
    const core::Styles& value() const { return m_s; }
    sigc::signal<void(const core::Styles&)>& signal_changed() { return m_changed; }
    sigc::signal<void()>& signal_begin() { return m_begin; }
    sigc::signal<void()>& signal_end() { return m_end; }

private:
    // One effect's section: an expander with a box in its label.
    Gtk::Box* section(const char* name, bool* on);
    void mode(Gtk::Box& into, pixel::Blend* field, const char* label = "Mode");   // s025: an effect's blend mode
    void choice(Gtk::Box& into, const char* label, std::vector<Glib::ustring> items, std::function<int()> get, std::function<void(int)> put);   // s021
    void slider(Gtk::Box& into, const char* label, int lo, int hi, const char* unit, int* field);
    void colour(Gtk::Box& into, core::Rgb* field, const char* label = "Colour");
    void emit();
    void paint();

    core::Styles m_s;
    bool m_painting = false;
    std::vector<std::function<void()>> m_painters;   // each control's repaint from m_s
    sigc::signal<void(const core::Styles&)> m_changed;
    sigc::signal<void()> m_begin, m_end;
};

}  // namespace klimt
