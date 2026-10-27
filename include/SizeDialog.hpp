#pragma once
// SizeDialog -- Image > Image Size and Image > Canvas Size (s009 m2). One
// class, two modes: Image Size asks width / height (linked by default),
// resolution and the resample filter; Canvas Size asks width / height and
// where the old image sits (a 3 x 3 anchor). Hide-on-close singleton per mode
// (docs/resources-and-dialog-lifetime.md): the Shell keeps one of each and
// re-asks with ask(), filled from the document as it is now.
#include "core/Canvas.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <array>

namespace klimt {

class SizeDialog : public Gtk::Window {
public:
    enum class Mode { Image, Canvas };
    SizeDialog(Gtk::Window& parent, Mode mode);
    ~SizeDialog() override;
    void ask(int width, int height, double dpi);

    struct Answer {
        int    width = 0, height = 0;
        double dpi = 72;                                       // Image Size
        core::canvas::Filter filter = core::canvas::Filter::Smooth;
        int    anchor_x = 1, anchor_y = 1;                     // Canvas Size
    };
    sigc::signal<void(const Answer&)>& signal_ok() { return m_ok; }

private:
    void on_width();
    void on_height();
    void refresh();                                            // the line under the fields, OK's greying
    void set_anchor(int x, int y);

    Mode                 m_mode;
    int                  m_w0 = 0, m_h0 = 0;                   // the size when asked
    bool                 m_syncing = false;
    widgets::SpinButton  m_width, m_height, m_dpi;
    widgets::ToggleButton m_link;
    widgets::DropDown    m_filter;
    widgets::Label       m_info;
    widgets::Button      m_ok_button, m_cancel_button;
    std::array<Gtk::ToggleButton*, 9> m_anchors{};             // managed, row-major
    int                  m_ax = 1, m_ay = 1;
    sigc::signal<void(const Answer&)> m_ok;
};

}  // namespace klimt
