#include "ColourSettingsDialog.hpp"
#include "Registry.hpp"

#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/headerbar.h>
#include <giomm/liststore.h>

namespace klimt {

namespace {
// Klimt's own profiles that ARE the plain conversion.
bool plain(const core::profile::Choice& c) {
    return c.path.empty() && (c.name == "Plain CMYK (no dot gain)" || c.name == "Gray (sRGB curve)");
}
}  // namespace

ColourSettingsDialog::ColourSettingsDialog(Gtk::Window& parent)
    : m_cmyk_pick("colour-settings.cmyk"), m_gray_pick("colour-settings.gray"), m_intent("colour-settings.intent"),
      m_note("colour-settings.note"), m_load("colour-settings.load", "Load…"),
      m_ok_button("colour-settings.ok", "OK"), m_cancel_button("colour-settings.cancel", "Cancel") {
    set_name("colour-settings");
    registry::add("colour-settings", this);
    set_title("Colour Settings");
    set_modal(true);
    set_resizable(false);
    set_transient_for(parent);
    set_hide_on_close(true);
    set_default_size(500, -1);
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(false);
    m_ok_button.add_css_class("suggested-action");
    hb->pack_start(m_cancel_button);
    hb->pack_end(m_ok_button);
    set_titlebar(*hb);

    m_cmyk.pick = &m_cmyk_pick;
    m_gray.pick = &m_gray_pick;
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(20);
    const auto row = [&](const char* label, Gtk::Widget& w) {
        auto* r = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
        auto* l = Gtk::make_managed<Gtk::Label>(label);
        l->add_css_class("dim-label");
        l->set_xalign(0.0f);
        l->set_size_request(110, -1);
        r->append(*l);
        w.set_hexpand(true);
        r->append(w);
        box->append(*r);
        return r;
    };
    auto* head = Gtk::make_managed<Gtk::Label>("Working Spaces");
    head->set_xalign(0.0f);
    head->add_css_class("heading");
    box->append(*head);
    for (auto* l : {&m_cmyk, &m_gray}) {
        l->names = Gtk::StringList::create({});
        l->pick->set_model(l->names);
        l->pick->set_enable_search(true);
        l->pick->property_selected().signal_changed().connect([this] { say(); });
    }
    m_cmyk_pick.set_tooltip_text("What an untagged CMYK picture's numbers mean -- and Image > Mode > CMYK's target");
    m_gray_pick.set_tooltip_text("What an untagged Gray picture's numbers mean -- and Image > Mode > Grayscale's target");
    auto* r = row("CMYK", m_cmyk_pick);
    m_load.set_tooltip_text("A CMYK or Gray profile from a file (.icc, .icm) -- it joins its list");
    r->append(m_load);
    row("Gray", m_gray_pick);
    auto* head2 = Gtk::make_managed<Gtk::Label>("Conversion Options");
    head2->set_xalign(0.0f);
    head2->add_css_class("heading");
    head2->set_margin_top(6);
    box->append(*head2);
    m_intent.set_model(Gtk::StringList::create({"Perceptual", "Relative Colorimetric", "Saturation", "Absolute Colorimetric"}));
    m_intent.set_tooltip_text("Image > Mode's conversions; Convert to Profile starts on it");
    row("Intent", m_intent);
    m_bpc.set_name("colour-settings.bpc");
    box->append(m_bpc);
    m_intent.property_selected().signal_changed().connect([this] { m_bpc.set_sensitive(m_intent.get_selected() != 3); say(); });
    m_note.set_xalign(0.0f);
    m_note.set_wrap(true);
    m_note.set_max_width_chars(56);
    m_note.add_css_class("dim-label");
    box->append(m_note);
    set_child(*box);

    m_load.signal_clicked().connect([this] { load(); });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        const guint c = m_cmyk_pick.get_selected(), g = m_gray_pick.get_selected();
        if (c >= m_cmyk.rows.size() || g >= m_gray.rows.size()) return;
        Answer a{m_cmyk.rows[c], m_gray.rows[g], {}};
        const guint i = m_intent.get_selected();
        a.render.intent = i == 0 ? tint::Intent::Perceptual : i == 2 ? tint::Intent::Saturation : i == 3 ? tint::Intent::Absolute : tint::Intent::Relative;
        a.render.bpc = m_bpc.get_active();
        close();
        m_ok.emit(a);
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

ColourSettingsDialog::~ColourSettingsDialog() { registry::remove(this); }

void ColourSettingsDialog::fill(List& l, const core::profile::Choice& now) {
    l.rows.clear();
    for (auto& c : core::profile::catalog())
        if (c.mode == l.mode) l.rows.push_back(c);
    guint pick = 0;
    bool found = false;
    for (std::size_t i = 0; i < l.rows.size(); ++i)
        if (l.rows[i].icc && now.icc && *l.rows[i].icc == *now.icc) { pick = static_cast<guint>(i); found = true; break; }
    if (!found && now.icc && now.mode == l.mode) {   // a loaded file: listed again
        l.rows.push_back(now);
        pick = static_cast<guint>(l.rows.size() - 1);
    }
    std::vector<Glib::ustring> names;
    for (const auto& c : l.rows) names.emplace_back(c.name + (plain(c) ? "  (the plain conversion)" : c.path.empty() ? "  (Klimt's)" : ""));
    l.names->splice(0, l.names->get_n_items(), names);
    l.pick->set_selected(GTK_INVALID_LIST_POSITION);
    l.pick->set_selected(pick);
}

void ColourSettingsDialog::ask(const core::profile::Choice& cmyk, const core::profile::Choice& gray, tint::Render r) {
    fill(m_cmyk, cmyk);
    fill(m_gray, gray);
    m_intent.set_selected(static_cast<guint>(r.intent));
    m_bpc.set_active(r.bpc);
    m_bpc.set_sensitive(r.intent != tint::Intent::Absolute);
    say();
    present();
    m_cmyk_pick.grab_focus();
}

void ColourSettingsDialog::say() {
    const guint c = m_cmyk_pick.get_selected();
    std::string n = "Untagged CMYK and Gray pictures are read through these; Image > Mode converts to them. "
                    "A picture with its own profile keeps it.";
    if (c < m_cmyk.rows.size() && !m_cmyk.rows[c].path.empty()) n += "\n" + m_cmyk.rows[c].path;
    if (m_intent.get_selected() == 0)
        n += "\nPerceptual needs a profile that carries a perceptual table (most press profiles do; Klimt's don't -- "
             "they fall back to Relative).";
    m_note.set_text(n);
}

void ColourSettingsDialog::load() {
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
        if (c->mode != pixel::Mode::CMYK && c->mode != pixel::Mode::Gray) {
            m_note.set_text(c->name + " isn't a CMYK or Gray profile -- the working spaces here are those two.");
            return;
        }
        fill(c->mode == pixel::Mode::CMYK ? m_cmyk : m_gray, *c);
    });
}

}  // namespace klimt
