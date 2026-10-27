#pragma once
// StatusLine -- the window's foot (s005, docs/ui.md 6): what the document is
// at the left; the running job (its progress, Cancel) and the zoom at the
// right. The s003 job bar moved here.
#include "widgets/Widgets.hpp"

#include <gtkmm/progressbar.h>
#include <sigc++/signal.h>

#include <string>

namespace klimt {

class StatusLine : public widgets::Box {
public:
    StatusLine();
    void set_summary(const std::string& text);
    void set_zoom(const std::string& text);
    void show_job(const std::string& label, double fraction);
    void hide_job();
    sigc::signal<void()>& signal_cancel() { return m_cancel_signal; }

private:
    widgets::Label   m_summary;
    widgets::Label   m_job_label;
    Gtk::ProgressBar m_job_progress;
    widgets::Button  m_job_cancel;
    widgets::Label   m_zoom;
    sigc::signal<void()> m_cancel_signal;
};

}  // namespace klimt
