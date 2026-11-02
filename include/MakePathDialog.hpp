#pragma once
// MakePathDialog -- Select > Make Path… (s081; docs/paths.md "The tracer").
// What to trace -- the selection, the picked layer's mask or transparency, a
// saved selection -- and the Tolerance (0.5 .. 10 px: how far the curves may
// stray from the edge; higher is fewer anchors). A readout says how many
// anchors it will make, worked out on a worker as the values change. OK: the
// Shell traces (a job) and adds the path layer -- ONE step. Hide-on-close
// singleton (docs/resources-and-dialog-lifetime.md).
#include "core/Paths.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <string>
#include <vector>

namespace klimt {

class MakePathDialog : public Gtk::Window {
public:
    struct Choice {
        core::paths::Source source;
        std::string         id;      // the layer or "@channel/n"; "" for the selection
        std::string         label;   // "Selection", "Transparency of “A”" ...
    };
    explicit MakePathDialog(Gtk::Window& parent);
    ~MakePathDialog() override;
    void ask(const std::vector<Choice>& choices, std::size_t pick, double tolerance);
    // The readout: "12 anchors", "…" while it works, "nothing to trace".
    void set_count(const std::string& text);
    const std::vector<Choice>& choices() const { return m_choices; }
    // The source (an index into choices) or the tolerance moved; OK.
    sigc::signal<void(std::size_t, double)>& signal_changed() { return m_changed; }
    sigc::signal<void(std::size_t, double)>& signal_ok() { return m_ok; }

private:
    std::vector<Choice>  m_choices;
    bool                 m_asking = false;
    widgets::DropDown    m_source;
    widgets::SpinButton  m_tolerance;
    widgets::Label       m_count;
    widgets::Button      m_ok_button, m_cancel_button;
    sigc::signal<void(std::size_t, double)> m_changed, m_ok;
};

}  // namespace klimt
