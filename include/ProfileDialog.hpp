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
//   Proof:   s069 -- View > Proof Setup…: the press to simulate (any mode;
//            Proof Colors and Gamut Warning show the picture through it),
//            Simulate Paper Colour. OK hands back the choice and the flag
//            (signal_proof) -- view state, not a step.
// Every list: Klimt's own and the system's profiles (core::profile::
// catalog), and Load… for a file. OK hands back the profile (Assign: nullptr
// is "None").
#include "core/Profile.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/checkbutton.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <vector>

namespace klimt {

class ProfileDialog : public Gtk::Window {
public:
    enum class Kind { Assign, Convert, Proof };
    ProfileDialog(Gtk::Window& parent, Kind k);
    ~ProfileDialog() override;
    void ask(const core::Image& im, tint::Render r = {});   // s070: Convert starts on `r`
    // s069: Proof -- start on `now`, the paper flag as given; s070: the intent.
    void ask_proof(const core::Image& im, const core::profile::Choice& now, bool paper, tint::Render r = {});
    tint::Render render() const;   // s070: Convert / Proof -- the intent and BPC chosen
    sigc::signal<void(core::profile::IccPtr)>& signal_ok() { return m_ok; }
    sigc::signal<void(core::profile::Choice, bool)>& signal_proof() { return m_proof; }

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
    Gtk::CheckButton  m_paper{"Simulate Paper Colour"};   // s069: Proof only
    widgets::DropDown m_intent;                            // s070: Convert / Proof
    Gtk::CheckButton  m_bpc{"Black Point Compensation"};
    void set_render(tint::Render r);
    sigc::signal<void(core::profile::IccPtr)> m_ok;
    sigc::signal<void(core::profile::Choice, bool)> m_proof;
};

}  // namespace klimt
