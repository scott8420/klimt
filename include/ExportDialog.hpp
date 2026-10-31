#pragma once
// ExportDialog -- the format's own settings, asked after the file name (s050;
// WEAKNESSES 56). JPEG quality, WebP quality / lossless, PNG compression,
// TIFF compression; for .ora, what will be baked into a layer's pixels and
// what will be left out -- said BEFORE it writes (core::ora::plan, the same
// lines the toast says after). Hide-on-close singleton
// (docs/resources-and-dialog-lifetime.md): the Shell keeps one and re-asks
// with ask(); what OK hands back is remembered per format in the Prefs.
#include "core/Ora.hpp"
#include "formats/Formats.hpp"
#include "widgets/Widgets.hpp"

#include <gtkmm/switch.h>
#include <gtkmm/window.h>
#include <sigc++/signal.h>

#include <string>

namespace klimt {

class ExportDialog : public Gtk::Window {
public:
    explicit ExportDialog(Gtk::Window& parent);
    ~ExportDialog() override;
    // `plan` is read for ORA only.
    void ask(formats::Kind kind, const std::string& file_name, const formats::WriteOptions& o,
             const core::ora::Plan& plan = {});
    sigc::signal<void(formats::WriteOptions)>& signal_ok() { return m_ok; }

private:
    void sync();   // the labels that follow the values
    void fill_ora(const core::ora::Plan& plan);
    formats::WriteOptions read() const;

    formats::Kind         m_kind = formats::Kind::PNG;
    formats::WriteOptions m_base;
    widgets::Label        m_heading;
    widgets::Stack        m_pages;
    widgets::Scale        m_jpeg_quality, m_webp_quality, m_png_level;
    widgets::Label        m_jpeg_value, m_webp_value, m_webp_what, m_webp_hint, m_png_value;
    widgets::Named<Gtk::Switch> m_webp_lossless;
    widgets::DropDown     m_tiff_compression;
    widgets::Box          m_ora_list;
    widgets::Button       m_ok_button, m_cancel_button;
    sigc::signal<void(formats::WriteOptions)> m_ok;
};

}  // namespace klimt
