#pragma once
// ColourRangeDialog -- Select > Color Range (s024). NOT modal: the canvas
// stays live, so the Eyedropper (I) picks the colours -- the first sample
// follows the foreground colour; "Add" keeps the current one and picks on.
// The Shell previews the selection as Quick Mask's red. docs/selection.md.
#include "core/ColourRange.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/drawingarea.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

namespace klimt {

class ColourRangeDialog : public Gtk::Window {
public:
    explicit ColourRangeDialog(Gtk::Window& parent);
    ~ColourRangeDialog() override;
    void ask(const std::array<float, 3>& foreground);   // starts from the foreground colour
    void follow(const std::array<float, 3>& foreground); // the Eyedropper picked: the live sample
    core::select::ColourRange value() const;
    core::select::Op op() const;
    bool preview() const { return m_preview.get_active(); }
    void set_busy(bool b);

    sigc::signal<void()>& signal_changed() { return m_changed; }
    sigc::signal<void()>& signal_ok() { return m_ok; }
    sigc::signal<void()>& signal_closed() { return m_closed; }

private:
    std::vector<std::array<float, 3>> m_kept;   // added samples
    std::array<float, 3> m_live{0, 0, 0};       // the one that follows the foreground
    widgets::Scale       m_fuzz;
    widgets::SpinButton  m_fuzz_n;
    widgets::DropDown    m_op;
    widgets::Label       m_busy;
    Gtk::DrawingArea     m_swatches;
    Gtk::CheckButton     m_invert{"Invert"};
    Gtk::CheckButton     m_preview{"Preview"};
    widgets::Button      m_add, m_clear, m_ok_button, m_cancel_button;
    bool                 m_syncing = false;
    sigc::signal<void()> m_changed, m_ok, m_closed;
};

}  // namespace klimt
