#pragma once
// AdjustDialog -- Image > Adjustments (s013): one adjustment, chosen in an
// AdjustEditor, applied to the selected layer's pixels for good on OK (the
// Shell runs it as a job, one step). While it is up the canvas previews it
// live (the Shell's render shows the adjustment on that layer). Escape /
// Cancel / closing: nothing happens. docs/adjustments.md.
#include "AdjustEditor.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

namespace klimt {

class AdjustDialog : public Gtk::Window {
public:
    explicit AdjustDialog(Gtk::Window& parent);
    ~AdjustDialog() override;
    // Ask for one of kind k, about the layer `title` names.
    void ask(core::AdjustKind k, const std::string& layer);
    void set_histogram(const core::adjust::Histogram& h) { m_editor.set_histogram(h); }
    bool preview() const { return m_preview.get_active(); }
    const core::Adjust& value() const { return m_editor.value(); }

    sigc::signal<void()>& signal_changed() { return m_changed; }    // the settings or Preview moved
    sigc::signal<void(const core::Adjust&)>& signal_ok() { return m_ok; }
    sigc::signal<void()>& signal_closed() { return m_closed; }      // OK or not: the preview goes

private:
    AdjustEditor        m_editor;
    Gtk::CheckButton    m_preview{"Preview"};
    widgets::Button     m_ok_button, m_cancel_button;
    sigc::signal<void()> m_changed, m_closed;
    sigc::signal<void(const core::Adjust&)> m_ok;
};

}  // namespace klimt
