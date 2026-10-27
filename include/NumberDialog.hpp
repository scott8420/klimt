#pragma once
// NumberDialog -- one number, asked (s008 m3): Select > Feather's radius, and
// the next "how many pixels?" (Grow, Shrink, Border). Hide-on-close singleton
// per use (docs/resources-and-dialog-lifetime.md): the Shell keeps one,
// re-asks with ask(), and it remembers the last value.
#include "widgets/Widgets.hpp"

#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <string>

namespace klimt {

class NumberDialog : public Gtk::Window {
public:
    NumberDialog(Gtk::Window& parent, const std::string& name, const std::string& title, const std::string& label,
                 const std::string& unit, double lo, double hi, double value, int digits);
    ~NumberDialog() override;
    void ask();
    sigc::signal<void(double)>& signal_ok() { return m_ok; }

private:
    widgets::SpinButton m_value;
    widgets::Button     m_ok_button, m_cancel_button;
    sigc::signal<void(double)> m_ok;
};

}  // namespace klimt
