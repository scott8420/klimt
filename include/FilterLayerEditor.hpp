#pragma once
// FilterLayerEditor -- a FILTER LAYER's settings in Properties (s030): its
// filter's sliders, as the Filter dialog's (a label and a value on a line,
// the slider under them; log sliders for radii). A slider drag is a begin ..
// end (ONE step); a typed value is a press. docs/filter-layers.md.
#include "core/Filter.hpp"

#include <gtkmm/box.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/scale.h>
#include <gtkmm/spinbutton.h>
#include <sigc++/signal.h>

#include <vector>

namespace klimt {

class FilterLayerEditor : public Gtk::Box {
public:
    FilterLayerEditor();
    void set(const core::Filter& f);   // a different kind rebuilds; emits nothing
    const core::Filter& value() const { return m_f; }
    sigc::signal<void(const core::Filter&)>& signal_changed() { return m_changed; }
    sigc::signal<void()>& signal_begin() { return m_begin; }
    sigc::signal<void()>& signal_end() { return m_end; }

private:
    void rebuild();
    void sync(int keep_slider = -1);
    void row(const char* name, const char* label, double lo, double hi, int digits, bool log, const char* unit, double* field);
    void emit();
    struct Row { Gtk::Scale* s; Gtk::SpinButton* n; double lo, hi; bool log; double* field; };
    core::Filter m_f;
    double m_threshold = 0, m_angle = 0;
    std::vector<Row> m_rows;
    Gtk::CheckButton* m_gauss = nullptr;
    Gtk::CheckButton* m_mono = nullptr;
    Gtk::Box m_body{Gtk::Orientation::VERTICAL, 4};
    bool m_syncing = false;
    core::FilterKind m_built = core::FilterKind::None;
    sigc::signal<void(const core::Filter&)> m_changed;
    sigc::signal<void()> m_begin, m_end;
};

}  // namespace klimt
