#pragma once
// ExportDialog -- the format's own settings, asked after the file name (s050;
// WEAKNESSES 56). JPEG quality, WebP quality / lossless, PNG compression,
// TIFF compression; for .ora, what will be baked into a layer's pixels and
// what will be left out -- said BEFORE it writes (core::ora::plan, the same
// lines the toast says after). Hide-on-close singleton
// (docs/resources-and-dialog-lifetime.md): the Shell keeps one and re-asks
// with ask(); what OK hands back is remembered per format in the Prefs.
// s051 (Scott): the FORMAT is the file chooser's type menu (the Shell sets
// the name's extension to it); this dialog shows the name as it will be
// written, and when that is already a file the chooser didn't ask about, its
// OK becomes Replace.
// s080: EPS -- RGB / CMYK, run-length / ZIP, the TIFF preview; and what
// crosses: the clipping path (or that there's none, and where to set it).
#include "core/Ora.hpp"
#include "formats/Formats.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/switch.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <functional>
#include <string>

namespace klimt {

class ExportDialog : public Gtk::Window {
public:
    explicit ExportDialog(Gtk::Window& parent);
    ~ExportDialog() override;
    // `plan_for`: what a layered format (ORA, PSD, PDF) will bake / leave out.
    using PlanFor = std::function<core::ora::Plan(formats::Kind)>;
    // `path`: as it will be written; `chosen`: as the chooser confirmed it.
    void ask(formats::Kind kind, const std::string& path, const std::string& chosen, const formats::WriteOptions& o,
             PlanFor plan_for);
    // OK: the settings, the format, the path (its extension the format's).
    sigc::signal<void(formats::WriteOptions, formats::Kind, std::string)>& signal_ok() { return m_ok; }

private:
    void sync();   // the labels that follow the values
    void fill_ora(const core::ora::Plan& plan, formats::Kind kind);   // s051: PSD and PDF show the same list
    void fill_eps(const core::ora::Plan& plan);                         // s080
    void show_kind(formats::Kind kind);   // s051: the page, the title, the name's extension
    formats::WriteOptions read() const;

    formats::Kind         m_kind = formats::Kind::PNG;
    formats::WriteOptions m_base;
    widgets::Label        m_heading;
    widgets::Label        m_exists;      // s051: "already a file -- Export replaces it"
    std::string           m_path, m_chosen;   // s051: the path as it stands; as the chooser confirmed it
    std::string           m_chosen_existing;  // s051: a file the chooser itself already asked to replace
    PlanFor               m_plan_for;
    widgets::Stack        m_pages;
    widgets::Scale        m_jpeg_quality, m_webp_quality, m_png_level;
    widgets::Label        m_jpeg_value, m_webp_value, m_webp_what, m_webp_hint, m_png_value;
    widgets::Named<Gtk::Switch> m_webp_lossless;
    widgets::DropDown     m_tiff_compression;
    widgets::Box          m_ora_list;
    widgets::Named<Gtk::CheckButton> m_pdf_layers;   // s083: Klimt's layers inside the PDF
    // s080: EPS -- colour, encoding, preview; what crosses (the clipping path).
    widgets::DropDown     m_eps_colour, m_eps_encoding, m_eps_preview;
    widgets::Box          m_eps_list;
    widgets::Button       m_ok_button, m_cancel_button;
    sigc::signal<void(formats::WriteOptions, formats::Kind, std::string)> m_ok;
};

}  // namespace klimt
