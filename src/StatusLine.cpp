#include "StatusLine.hpp"

namespace klimt {

StatusLine::StatusLine()
    : widgets::Box("status", Gtk::Orientation::HORIZONTAL, 12),
      m_summary("status.summary"),
      m_job_label("status.job.label"),
      m_job_cancel("status.job.cancel"),
      m_zoom("status.zoom"),
      m_position("status.position") {
    add_css_class("klimt-status");
    set_margin_start(12);
    set_margin_end(12);
    set_margin_top(3);
    set_margin_bottom(3);
    m_summary.set_xalign(0.0f);
    m_summary.set_hexpand(true);
    m_summary.set_ellipsize(Pango::EllipsizeMode::END);
    m_summary.add_css_class("dim-label");
    append(m_summary);

    m_job_progress.set_size_request(140, -1);
    m_job_progress.set_valign(Gtk::Align::CENTER);
    m_job_cancel.set_label("Cancel");
    m_job_cancel.add_css_class("flat");
    m_job_cancel.signal_clicked().connect([this] { m_cancel_signal.emit(); });
    append(m_job_label);
    append(m_job_progress);
    append(m_job_cancel);
    hide_job();

    m_position.add_css_class("dim-label");   // s032
    m_position.add_css_class("numeric");
    m_position.set_tooltip_text("Where the pointer is, from the rulers' zero point");
    append(m_position);
    m_zoom.add_css_class("dim-label");
    m_zoom.set_tooltip_text("Zoom");
    append(m_zoom);
}

void StatusLine::set_summary(const std::string& text) { m_summary.set_text(text); }
void StatusLine::set_zoom(const std::string& text) { m_zoom.set_text(text); }
void StatusLine::set_position(const std::string& text) { m_position.set_text(text); }

void StatusLine::show_job(const std::string& label, double fraction) {
    m_job_label.set_text(label);
    m_job_progress.set_fraction(fraction);
    m_job_label.set_visible(true);
    m_job_progress.set_visible(true);
    m_job_cancel.set_visible(true);
}

void StatusLine::hide_job() {
    m_job_label.set_visible(false);
    m_job_progress.set_visible(false);
    m_job_cancel.set_visible(false);
}

}  // namespace klimt
