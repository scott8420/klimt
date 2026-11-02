#pragma once
// RawDialog -- File > Open of a camera RAW: DEVELOP first (s085; docs/raw.md).
// The picture on the left -- the camera's own preview at once, then a
// half-size develop of the settings as they change (the Shell runs it on a
// worker and hands it back) -- the settings on the right: White Balance and
// Warmth, Exposure and Auto Brightness, Highlights, Noise Reduction, Colour.
// Open develops the whole picture with them (and they become what a plain
// open of a RAW uses). Hide-on-close singleton (docs/resources-and-dialog-
// lifetime.md).
#include "core/Raw.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace klimt {

class RawDialog : public Gtk::Window {
public:
    explicit RawDialog(Gtk::Window& parent);
    ~RawDialog() override;
    void ask(const std::string& path, const core::raw::Develop& d);
    const std::string& path() const { return m_path; }
    core::raw::Develop current() const;
    // The Shell's answers: the file's line ("Canon EOS R5 · ISO 400 …"), and a
    // picture (8-bit RGBA, w x h) with what it is ("the camera's preview").
    void show_info(const std::string& line);
    void show_picture(int w, int h, std::shared_ptr<const std::vector<std::uint8_t>> rgba, const std::string& what);
    sigc::signal<void(core::raw::Develop)>& signal_changed() { return m_changed; }   // a preview wanted
    sigc::signal<void(std::string, core::raw::Develop)>& signal_open() { return m_open; }
    sigc::signal<void()>& signal_gone() { return m_gone; }                            // closed without Open

private:
    std::string m_path;
    bool m_setting = false;
    widgets::Picture   m_picture;
    widgets::Label     m_info, m_what;
    widgets::DropDown  m_balance, m_highlights, m_space;
    widgets::Scale     m_warmth, m_exposure, m_noise;
    widgets::Named<Gtk::CheckButton> m_auto;
    widgets::Button    m_ok_button, m_cancel_button, m_reset;
    sigc::signal<void(core::raw::Develop)> m_changed;
    sigc::signal<void(std::string, core::raw::Develop)> m_open;
    sigc::signal<void()> m_gone;
    bool m_opening = false;
    void set(const core::raw::Develop& d);
    void changed();
};

}  // namespace klimt
