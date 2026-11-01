#include "ProfileDialog.hpp"
#include "Registry.hpp"
#include "core/Composite.hpp"

#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/headerbar.h>
#include <giomm/liststore.h>

namespace klimt {

namespace {
const char* mode_name(pixel::Mode m) {
    switch (m) {
        case pixel::Mode::RGB:  return "RGB";
        case pixel::Mode::Gray: return "Gray";
        case pixel::Mode::CMYK: return "CMYK";
        case pixel::Mode::Lab:  return "Lab";
    }
    return "";
}
}  // namespace

ProfileDialog::ProfileDialog(Gtk::Window& parent, Kind k)
    : m_kind(k),
      m_current(k == Kind::Assign ? "profile-assign.current" : "profile-convert.current"),
      m_note(k == Kind::Assign ? "profile-assign.note" : "profile-convert.note"),
      m_pick(k == Kind::Assign ? "profile-assign.pick" : "profile-convert.pick"),
      m_load(k == Kind::Assign ? "profile-assign.load" : "profile-convert.load", "Load…"),
      m_ok_button(k == Kind::Assign ? "profile-assign.ok" : "profile-convert.ok", "OK"),
      m_cancel_button(k == Kind::Assign ? "profile-assign.cancel" : "profile-convert.cancel", "Cancel") {
    const std::string name = k == Kind::Assign ? "profile-assign" : "profile-convert";
    set_name(name);
    registry::add(name, this);
    set_title(k == Kind::Assign ? "Assign Profile" : "Convert to Profile");
    set_modal(true);
    set_resizable(false);
    set_transient_for(parent);
    set_hide_on_close(true);
    set_default_size(460, -1);
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(false);
    m_ok_button.add_css_class("suggested-action");
    hb->pack_start(m_cancel_button);
    hb->pack_end(m_ok_button);
    set_titlebar(*hb);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(20);
    const auto row = [&](const char* label, Gtk::Widget& w) {
        auto* r = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        auto* l = Gtk::make_managed<Gtk::Label>(label);
        l->add_css_class("dim-label");
        l->set_xalign(0.0f);
        l->set_size_request(92, -1);
        r->append(*l);
        w.set_hexpand(true);
        r->append(w);
        box->append(*r);
        return r;
    };
    m_current.set_xalign(0.0f);
    m_current.set_ellipsize(Pango::EllipsizeMode::END);
    m_current.set_selectable(true);
    row("Now", m_current);
    m_names = Gtk::StringList::create({});
    m_pick.set_model(m_names);
    m_pick.set_enable_search(true);
    m_pick.set_tooltip_text(k == Kind::Assign ? "What the document's numbers mean -- they stay as they are"
                                              : "The space the numbers go into -- the look stays; its mode becomes the document's");
    auto* r = row(k == Kind::Assign ? "Profile" : "Destination", m_pick);
    m_load.set_tooltip_text("A profile from a file (.icc, .icm)");
    r->append(m_load);
    m_note.set_xalign(0.0f);
    m_note.set_wrap(true);
    m_note.set_max_width_chars(52);
    m_note.add_css_class("dim-label");
    box->append(m_note);
    set_child(*box);

    m_pick.property_selected().signal_changed().connect([this] {
        const guint i = m_pick.get_selected();
        if (i >= m_rows.size()) return;
        const auto& c = m_rows[i];
        std::string n;
        if (m_kind == Kind::Assign)
            n = c.icc ? "The numbers stay; the picture is read as " + c.name + "."
                      : "No profile: read as plain " + std::string(mode_name(m_mode)) + " (sRGB for RGB).";
        else
            n = std::string("Relative colorimetric, black point compensation. ") +
                (c.mode != m_mode ? std::string("The document becomes ") + mode_name(c.mode) + "; text is set again, masks stay."
                                  : "The look stays; the numbers change.");
        if (!c.path.empty()) n += "\n" + c.path;
        m_note.set_text(n);
    });
    m_load.signal_clicked().connect([this] { load(); });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        const guint i = m_pick.get_selected();
        if (i >= m_rows.size()) return;
        const auto icc = m_rows[i].icc;
        close();
        m_ok.emit(icc);
    });
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint key, guint, Gdk::ModifierType) {
        if (key == GDK_KEY_Escape) { close(); return true; }
        if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) { m_ok_button.activate(); return true; }
        return false;
    }, false);
    add_controller(keys);
}

ProfileDialog::~ProfileDialog() { registry::remove(this); }

void ProfileDialog::ask(const core::Image& im) {
    m_mode = im.format.mode;
    const std::string now = core::profile_name(im);
    m_current.set_text(std::string(mode_name(m_mode)) + " · " + (now.empty() ? "no profile (plain " + std::string(mode_name(m_mode)) + ")" : now));
    m_rows.clear();
    if (m_kind == Kind::Assign) m_rows.push_back({"None -- don't colour manage", nullptr, m_mode, {}});
    for (auto& c : core::profile::catalog())
        if (m_kind == Kind::Convert || c.mode == m_mode) m_rows.push_back(c);
    fill();
    // Start on the document's own profile when it is listed; else the first
    // of its mode (Convert: the first of ANOTHER profile -- sRGB for most).
    guint pick = 0;
    for (std::size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].icc && im.icc && *m_rows[i].icc == *im.icc) { pick = static_cast<guint>(i); break; }
    if (m_kind == Kind::Convert && im.icc && pick < m_rows.size() && m_rows[pick].icc && *m_rows[pick].icc == *im.icc)
        pick = pick == 0 ? 1 : 0;
    m_pick.set_selected(GTK_INVALID_LIST_POSITION);
    m_pick.set_selected(std::min<guint>(pick, static_cast<guint>(m_rows.size() - 1)));
    present();
    m_pick.grab_focus();
}

void ProfileDialog::fill() {
    std::vector<Glib::ustring> names;
    for (const auto& c : m_rows)
        names.emplace_back(m_kind == Kind::Convert || c.icc ? c.name + "  (" + mode_name(c.mode) + (c.path.empty() && c.icc ? ", Klimt's" : "") + ")"
                                                            : c.name);
    m_names->splice(0, m_names->get_n_items(), names);
}

void ProfileDialog::load() {
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Load a Colour Profile");
    auto f = Gtk::FileFilter::create();
    f->set_name("ICC profiles (.icc, .icm)");
    for (const char* p : {"*.icc", "*.ICC", "*.icm", "*.ICM"}) f->add_pattern(p);
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(f);
    dlg->set_filters(filters);
    dlg->open(*this, [this, dlg](Glib::RefPtr<Gio::AsyncResult>& res) {
        Glib::RefPtr<Gio::File> file;
        try { file = dlg->open_finish(res); } catch (...) { return; }
        if (!file) return;
        std::string why;
        auto c = core::profile::load(file->get_path(), why);
        if (!c) { m_note.set_text("Couldn't use " + file->get_basename() + ": " + why); return; }
        if (m_kind == Kind::Assign && c->mode != m_mode) {
            m_note.set_text(c->name + " is a " + mode_name(c->mode) + " profile -- Assign takes " + mode_name(m_mode) +
                            " ones; Convert to Profile changes the mode.");
            return;
        }
        m_rows.push_back(std::move(*c));
        fill();
        m_pick.set_selected(static_cast<guint>(m_rows.size() - 1));
    });
}

}  // namespace klimt
