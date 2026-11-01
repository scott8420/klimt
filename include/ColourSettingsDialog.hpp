#pragma once
// ColourSettingsDialog -- Edit > Colour Settings… (s070; WEAKNESSES 14;
// docs/colour.md). Hide-on-close singleton (docs/resources-and-dialog-
// lifetime.md): the Shell keeps one and re-asks with ask().
//
//   Working CMYK / Gray: what an UNTAGGED CMYK / Gray document's numbers
//     mean, and Image > Mode's target for that mode. Klimt's Plain CMYK /
//     Gray (sRGB curve) are the plain conversion. Every profile of that
//     mode Klimt knows (core::profile::catalog), and Load… for a file.
//   Intent + Black Point Compensation: Image > Mode's conversions; Convert
//     to Profile starts on them.
// OK hands back the three (signal_ok). Not a step: settings.
#include "core/Profile.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <vector>

namespace klimt {

class ColourSettingsDialog : public Gtk::Window {
public:
    struct Answer {
        core::profile::Choice cmyk, gray;
        tint::Render render;
    };
    explicit ColourSettingsDialog(Gtk::Window& parent);
    ~ColourSettingsDialog() override;
    void ask(const core::profile::Choice& cmyk, const core::profile::Choice& gray, tint::Render r);
    sigc::signal<void(Answer)>& signal_ok() { return m_ok; }

private:
    struct List {
        pixel::Mode mode;
        std::vector<core::profile::Choice> rows;
        widgets::DropDown* pick = nullptr;
        Glib::RefPtr<Gtk::StringList> names;
    };
    void fill(List& l, const core::profile::Choice& now);
    void load();
    void say();
    List m_cmyk{pixel::Mode::CMYK, {}, nullptr, {}}, m_gray{pixel::Mode::Gray, {}, nullptr, {}};
    widgets::DropDown m_cmyk_pick, m_gray_pick, m_intent;
    widgets::Label    m_note;
    widgets::Button   m_load, m_ok_button, m_cancel_button;
    Gtk::CheckButton  m_bpc{"Black Point Compensation"};
    sigc::signal<void(Answer)> m_ok;
};

}  // namespace klimt
