#pragma once
// ClipDialog -- Select > Clipping Path… (s080; Photoshop's Paths panel menu
// item). Which path layer is the document's clipping path (None, or one of
// them) and its flatness. Hide-on-close singleton
// (docs/resources-and-dialog-lifetime.md): the Shell keeps one and re-asks
// with the document's path layers each time. OK: the Shell sets it -- ONE
// step (core::clip::set). Properties shows the same for the picked path.
#include "widgets/Widgets.hpp"

#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <string>
#include <vector>

namespace klimt {

class ClipDialog : public Gtk::Window {
public:
    struct Choice { std::string id, name; };
    explicit ClipDialog(Gtk::Window& parent);
    ~ClipDialog() override;
    // The path layers (panel order), the clipping path now ("" none), its flatness.
    void ask(const std::vector<Choice>& paths, const std::string& current, double flatness);
    // OK: the path layer's id ("" none) and the flatness.
    sigc::signal<void(std::string, double)>& signal_ok() { return m_ok; }

private:
    std::vector<Choice>  m_paths;
    widgets::DropDown    m_path;
    widgets::SpinButton  m_flatness;
    widgets::Button      m_ok_button, m_cancel_button;
    sigc::signal<void(std::string, double)> m_ok;
};

}  // namespace klimt
