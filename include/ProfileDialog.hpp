#pragma once
// ProfileDialog -- Image > Assign Profile… and Convert to Profile… (s068;
// WEAKNESSES 14; docs/colour.md). Hide-on-close singleton per kind
// (docs/resources-and-dialog-lifetime.md): the Shell keeps one of each and
// re-asks with ask().
//
//   Assign:  the document's current profile; "None (don't colour manage)",
//            or a profile of the document's own mode.
//   Convert: the current profile; a destination of any mode (its colour space
//            becomes the document's mode); the intent said, not chosen.
// Either list: Klimt's own and the system's profiles (core::profile::
// catalog), and Load… for a file. OK hands back the profile (Assign: nullptr
// is "None").
#include "core/Profile.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <vector>

namespace klimt {

class ProfileDialog : public Gtk::Window {
public:
    enum class Kind { Assign, Convert };
    ProfileDialog(Gtk::Window& parent, Kind k);
    ~ProfileDialog() override;
    void ask(const core::Image& im);
    sigc::signal<void(core::profile::IccPtr)>& signal_ok() { return m_ok; }

private:
    void fill();   // the dropdown's rows from m_rows
    void load();
    Kind m_kind;
    pixel::Mode m_mode = pixel::Mode::RGB;
    std::vector<core::profile::Choice> m_rows;   // Assign: row 0 is None (no icc)
    Glib::RefPtr<Gtk::StringList> m_names;
    widgets::Label    m_current, m_note;
    widgets::DropDown m_pick;
    widgets::Button   m_load, m_ok_button, m_cancel_button;
    sigc::signal<void(core::profile::IccPtr)> m_ok;
};

}  // namespace klimt
