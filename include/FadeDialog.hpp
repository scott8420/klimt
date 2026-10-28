#pragma once
// FadeDialog -- Edit > Fade (s020): the last step's result at an opacity, in
// a blend mode, over what it began from. The canvas previews it live (the
// Shell fades on a worker a moment after the last change); OK lands it as
// its own step. Escape / Cancel / closing: nothing. docs/fade.md.
#include "pixel/Blend.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <string>
#include <vector>

namespace klimt {

class FadeDialog : public Gtk::Window {
public:
    explicit FadeDialog(Gtk::Window& parent);
    ~FadeDialog() override;
    // Ask about `what` ("Gaussian Blur"); opacity and mode as last time.
    void ask(const std::string& what, bool rgb);
    double       opacity() const;             // 0..1
    pixel::Blend mode() const;
    bool         preview() const { return m_preview.get_active(); }
    void         set_busy(bool b);

    sigc::signal<void()>& signal_changed() { return m_changed; }   // a value or Preview moved
    sigc::signal<void()>& signal_ok() { return m_ok; }
    sigc::signal<void()>& signal_closed() { return m_closed; }     // OK or not: the preview goes

private:
    widgets::Label       m_what;
    widgets::Scale       m_slider;
    widgets::SpinButton  m_number;
    widgets::DropDown    m_mode;
    widgets::Label       m_busy;
    Gtk::CheckButton     m_preview{"Preview"};
    widgets::Button      m_ok_button, m_cancel_button;
    std::vector<pixel::Blend> m_modes;        // the drop-down's rows
    bool                 m_syncing = false;
    sigc::signal<void()> m_changed, m_ok, m_closed;
};

}  // namespace klimt
