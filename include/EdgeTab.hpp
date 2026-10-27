#pragma once
// EdgeTab -- the hiding tab (s006; Sudoku's drawer tab, from Folio's focus
// mode). A pull-tab on a panel's edge, invisible until the pointer is over it
// (pure CSS: opacity 0, :hover 1 -- the reliable idiom; hover logic in C++
// was tried in Folio and abandoned). A CLICK hides / shows its panel; on the
// dock's edge a DRAG sizes the panel (Scott, s005: "use our hiding tab to
// drag panel sizes"). One gesture tells the two apart: under 4 px of travel
// is a click.
#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <sigc++/signal.h>

namespace klimt {

class EdgeTab : public Gtk::Box {
public:
    enum class Side { Left, Right };   // which side of the CONTENT it sits on
    EdgeTab(const char* name, Side side, bool resizes);
    // The panel is open: the glyph points the way a click would push it.
    void set_open(bool open);
    sigc::signal<void()>& signal_toggle() { return m_toggle; }
    sigc::signal<void()>& signal_drag_begin() { return m_begin; }
    sigc::signal<void(double)>& signal_drag_by() { return m_by; }   // total dx since begin
    sigc::signal<void()>& signal_drag_end() { return m_end; }

private:
    Side       m_side;
    bool       m_resizes;
    bool       m_dragging = false;
    double     m_start_x = 0;       // window coordinates at press
    double window_x(double x) const;   // widget x -> window x (the tab moves)
    Gtk::Label m_glyph;
    sigc::signal<void()> m_toggle, m_begin, m_end;
    sigc::signal<void(double)> m_by;
};

}  // namespace klimt
