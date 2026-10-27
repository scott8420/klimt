#pragma once
// NewDialog -- File > New (s007 m3): size, resolution, mode, depth and
// background, with presets, and a line saying what the image will cost.
// Hide-on-close singleton, like About (docs/resources-and-dialog-lifetime.md):
// built once, re-presented, remembers the last spec for the session.
#include "core/File.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/window.h>
#include <sigc++/signal.h>

namespace klimt {

class NewDialog : public Gtk::Window {
public:
    explicit NewDialog(Gtk::Window& parent);
    ~NewDialog() override;
    void open();                                        // present, the size field focused
    // Create: the spec, checked (core::file::check passed).
    sigc::signal<void(const core::file::NewSpec&)>& signal_create() { return m_create; }

private:
    core::file::NewSpec spec() const;
    void on_preset();
    void refresh();                                     // the cost line, Create's greying

    widgets::DropDown   m_preset;
    widgets::SpinButton m_width, m_height, m_dpi;
    widgets::DropDown   m_mode, m_depth, m_background;
    widgets::Label      m_cost;
    widgets::Button     m_create_button, m_cancel_button, m_swap;
    bool                m_applying = false;             // a preset is filling the fields
    sigc::signal<void(const core::file::NewSpec&)> m_create;
};

}  // namespace klimt
