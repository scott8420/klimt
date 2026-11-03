#include "Shell.hpp"
#include "core/Recents.hpp"   // s054
#include <glibmm/main.h>
#include "NewDialog.hpp"
#include "ExportDialog.hpp"   // s050
#include "core/Transfer.hpp"   // s022: place
#include "core/Session.hpp"    // s023
#include "core/Ora.hpp"
#include <gtkmm/filelauncher.h>
#include "core/Pdf.hpp"        // s051: Export as PDF
#include "core/Eps.hpp"        // s080: Export as EPS
#include "core/Smart.hpp"      // s082: Place, smart layers
#include "core/RoundTrip.hpp"  // s083: the Composix round-trip
#include "core/Raw.hpp"        // s085: camera RAW
#include "core/Canvas.hpp"
#include "core/Transfer.hpp"
#include "core/Psd.hpp"        // s049: Export as OpenRaster
#include <glibmm/miscutils.h>
#include "Log.hpp"

#include <adwaita.h>
#include <giomm/file.h>
#include <giomm/liststore.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/error.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>

#include <algorithm>
#include <filesystem>

// Shell_files.cpp -- FILES (s007 m3). New, Open, Save, Save As, Export, and
// the unsaved-changes question. The disk work is a JOB (core::file on a
// worker: the UI never waits); its landing changes the window. Opening is a
// LOAD, not a step: the journal starts again with the new document.

namespace klimt {

namespace {
namespace fs = std::filesystem;
std::string lower_ext(const std::string& path) {
    std::string e = fs::path(path).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}
std::string stem_of(const std::string& name) {
    const auto s = fs::path(name).stem().string();
    return s.empty() ? std::string("Untitled") : s;
}
Glib::RefPtr<Gtk::FileFilter> filter(const char* name, std::initializer_list<const char*> suffixes) {
    auto f = Gtk::FileFilter::create();
    f->set_name(name);
    for (const char* s : suffixes) f->add_suffix(s);
    return f;
}
// The Progress a worker hands core::file: its token's cancel and progress.
formats::Progress progress_of(core::Jobs::Token& t) {
    formats::Progress p;
    p.cancelled = [&t] { return t.cancelled(); };
    p.progress = [&t](double f) { t.progress(f); };
    return p;
}
}  // namespace

void Shell::update_title() {
    const std::string t = (dirty() ? "• " : "") + name_();
    if (m_title) adw_window_title_set_title(m_title, t.c_str());
    set_title(name_() + " — Klimt");
    sync_tabs();   // s020: the tab says it too
}

bool Shell::file_job_busy() {
    using S = core::Jobs::State;
    if (!m_file_job) return false;
    const auto st = m_jobs.state(m_file_job);
    if (st == S::Queued || st == S::Running || st == S::Done) {
        toast("Wait for “" + m_jobs.label(m_file_job) + "” to finish, or cancel it");
        return true;
    }
    return false;
}

// The document becomes `c`: a LOAD (Document::Loading), so no step; the
// journal starts again and the file is "saved" as it stands.
void Shell::adopt(core::file::Contents c, const std::string& path, const std::string& name, const std::string& source) {
    text_close();   // s046: a visit ends before its document goes
    {
        core::Document::Loading load(m_doc);
        m_doc.replace(c.image, std::move(c.layers), c.next_id);
        m_doc.replace_channels(std::move(c.channels));   // s016
        if (c.selection && c.selection->width() == c.image.width && c.selection->height() == c.image.height &&
            c.selection->format() == m_doc.mask_format()) {   // s026: the selection as it was saved
            core::Layer s;
            s.name = "Selection";
            s.pixels = c.selection;
            m_doc.put(core::kSelectionId, s);
        }
    }
    m_journal.clear();
    live_snapshots().clear(); update_snapshots();   // s077: a new picture in the tab -- its snapshots go
    m_tabs.live_info().saved_mark = m_journal.mark();
    path_() = path;
    name_() = name;
    m_tabs.live_info().source = source;   // s023
    m_layers.forget_view();
    const auto top = m_doc.panel("");
    if (!top.empty()) m_layers.select({top.front()}, false);
    // The old picture goes now; the next render starts the new one fresh.
    m_canvas.set_document(c.image.width, c.image.height);
    m_dirty = {};
    m_canvas.fit();
    if (c.view.set) {   // s056: the view the .klimt was saved with
        m_canvas.set_view({c.view.zoom, c.view.cx, c.view.cy, c.view.fit});
        m_layers.set_collapsed(c.view.collapsed);
    }
    update_title();
    update_summary();
    std::string note;
    for (const auto& n : c.notes) note += (note.empty() ? "" : ", ") + n;
    if (!note.empty()) toast(name + ": " + note);
    if (auto lg = log::get(log::Area::Shell)) lg->info("document: {} ({} x {})", name, c.image.width, c.image.height);
}

// Save / Discard / Cancel, then `go` (unless Cancel). Nothing unsaved: `go`.
void Shell::ask_unsaved(const std::string& doing, std::function<void()> go) {
    if (!dirty()) { go(); return; }
    auto a = Gtk::AlertDialog::create("Save changes to “" + name_() + "” before " + doing + "?");
    a->set_detail("Changes not saved are lost.");
    a->set_buttons({"Cancel", "Discard", "Save"});
    a->set_cancel_button(0);
    a->set_default_button(2);
    a->choose(*this, [this, a, go](Glib::RefPtr<Gio::AsyncResult>& res) {
        int i = 0;
        try { i = a->choose_finish(res); } catch (const Glib::Error&) { return; }
        if (i == 1) { go(); return; }
        if (i != 2) return;
        if (!m_tabs.live_info().back.empty() || m_tabs.live_info().owner_key) { save_back(go); return; }   // s083
        if (!path_().empty()) { save_to(path_(), go); return; }
        // Never saved: Save As first, then go.
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Save As");
        dlg->set_initial_name(stem_of(name_()) + ".klimt");
        if (!m_folder.empty()) dlg->set_initial_folder(Gio::File::create_for_path(m_folder));
        dlg->save(*this, [this, dlg, go](Glib::RefPtr<Gio::AsyncResult>& r) {
            Glib::RefPtr<Gio::File> f;
            try { f = dlg->save_finish(r); } catch (const Glib::Error&) { return; }
            if (!f) return;
            std::string path = f->get_path();
            if (lower_ext(path) != ".klimt") path = (fs::path(path).replace_extension(".klimt")).string();
            save_to(path, go);
        });
    });
}

// s020: every tab with unsaved changes asks, in turn (each shown first).
bool Shell::on_close_request() {
    text_close();   // s046
    if (m_closing) { save_session(); return false; }   // s023: the tabs, for next time
    if (file_job_busy()) return true;
    bool any = false;
    for (std::size_t i = 0; i < m_tabs.count(); ++i) any = any || m_tabs.dirty(i);
    if (!any) { save_session(); return false; }
    // From idle: closing inside the dialog's own callback left GLib criticals
    // on the way out (seen under Xvfb, s019 too).
    close_all_then([this] { Glib::signal_idle().connect_once([this] { m_closing = true; close(); }); });
    return true;
}

// ── New ─────────────────────────────────────────────────────────────────────
// s009: launched without a file: the New dialog, once the window is up.
void Shell::start() {
    if (const char* s = std::getenv("KLIMT_SWEEP"); s && (*s == '1' || *s == '2')) { start_sweep(); return; }   // s090 (2: with a selection made)
    if (has_image()) return;
    Glib::signal_idle().connect_once([this] {
        if (has_image()) return;
        if (!restore_session()) on_new();   // s023: last time's tabs, else the New dialog
    });
}

// ── s023: the open tabs, remembered ─────────────────────────────────────────
namespace {
std::string session_file() { return Glib::build_filename(Glib::get_user_config_dir(), "klimt", "session.json"); }
}

void Shell::save_session() {
    core::Session s;
    for (std::size_t i = 0; i < m_tabs.count(); ++i) {
        const auto& info = m_tabs.info(i);
        const std::string f = !info.path.empty() ? info.path : info.source;
        if (f.empty()) continue;
        if (i == m_tabs.live()) s.active = static_cast<int>(s.files.size());
        s.files.push_back(f);
    }
    if (!core::save_session(session_file(), s))
        if (auto lg = log::get(log::Area::Shell)) lg->warn("session: couldn't write {}", session_file());
}

bool Shell::restore_session() {
    const auto s = core::existing(core::load_session(session_file()));
    if (s.files.empty()) return false;
    m_restore_front = s.active >= 0 ? s.files[static_cast<std::size_t>(s.active)] : std::string{};
    if (auto lg = log::get(log::Area::Shell)) lg->info("session: reopening {} file(s)", s.files.size());
    open_paths(s.files);
    return true;
}

// s009: Help > Open Demo -- the demo document in place of this one.
void Shell::open_demo() {
    if (file_job_busy()) return;
    text_close();   // s046
    // s020: in a tab of its own (the live one when it is empty).
    if (!tab_reusable() && !new_tab()) return;
    {
        {
            core::Document::Loading load(m_doc);
            m_doc.replace({}, {}, 1);
        }
        m_layers.forget_view();
        load_demo();
        m_journal.clear();
        live_snapshots().clear(); update_snapshots();   // s077
        m_tabs.live_info().saved_mark = m_journal.mark();
        path_().clear();
        name_() = "Demo";
        m_canvas.set_document(m_doc.image().width, m_doc.image().height);
        m_dirty = {};
        m_canvas.fit();
        update_title();
        update_summary();
        update_layer_actions();
    }
}

// s020: New and Open make a TAB (the live one when it is empty and clean);
// nothing is replaced, so nothing asks.
void Shell::on_new() {
    if (file_job_busy()) return;
    if (!m_new) {
        m_new = std::make_unique<NewDialog>(*this);
        m_new->signal_create().connect([this](const core::file::NewSpec& s) {
            if (!tab_reusable() && !new_tab()) return;
            adopt(core::file::make_new(s), "", "Untitled");
        });
    }
    m_new->open();
}

// ── Open ────────────────────────────────────────────────────────────────────
void Shell::on_open() {
    if (file_job_busy()) return;
    {
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Open");
        auto filters = Gio::ListStore<Gtk::FileFilter>::create();
        auto all = filter("Images and documents", {"klimt", "ora", "psd", "png", "jpg", "jpeg", "tif", "tiff", "webp", "pdf",
                                                   "cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "dng", "raf", "orf", "rw2",
                                                   "rwl", "pef", "srw", "x3f", "3fr", "fff", "iiq", "mos", "mef", "mrw", "erf", "kdc", "dcr"});
        filters->append(all);
        filters->append(filter("Klimt document (.klimt)", {"klimt"}));
        filters->append(filter("OpenRaster (.ora)", {"ora"}));   // s049
        filters->append(filter("Photoshop (.psd)", {"psd"}));    // s050
        filters->append(filter("PNG", {"png"}));
        filters->append(filter("JPEG", {"jpg", "jpeg"}));
        filters->append(filter("TIFF", {"tif", "tiff"}));
        filters->append(filter("WebP", {"webp"}));                // s049
        filters->append(filter("PDF written by Klimt", {"pdf"}));  // s083: its layers ride inside
        filters->append(filter("Camera RAW", {"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "dng", "raf", "orf", "rw2",
                                              "rwl", "pef", "srw", "x3f", "3fr", "fff", "iiq", "mos", "mef", "mrw", "erf", "kdc", "dcr"}));   // s085
        dlg->set_filters(filters);
        dlg->set_default_filter(all);
        if (!m_folder.empty()) dlg->set_initial_folder(Gio::File::create_for_path(m_folder));
        dlg->open(*this, [this, dlg](Glib::RefPtr<Gio::AsyncResult>& res) {
            Glib::RefPtr<Gio::File> f;
            try { f = dlg->open_finish(res); } catch (const Glib::Error&) { return; }
            if (!f || f->get_path().empty()) return;
            // s085: a camera RAW -- Develop first.
            if (core::raw::is_raw_name(f->get_path()) && core::raw::available()) ask_raw(f->get_path());
            else open_path(f->get_path());
        });
    }
}

// ── s085: camera RAW -- the Develop dialog ─────────────────────────────────
namespace {
// A picture for the dialog: at most `side` px a side, 8-bit RGBA bytes.
struct Shown { int w = 0, h = 0; std::shared_ptr<const std::vector<std::uint8_t>> rgba; };
std::optional<Shown> shown_of(const pixel::TileMap& m, int side, const std::function<bool()>& cancelled) {
    int w = m.width(), h = m.height();
    if (w <= 0 || h <= 0) return std::nullopt;
    std::optional<pixel::TileMap> small;
    if (std::max(w, h) > side) {
        const double k = static_cast<double>(side) / std::max(w, h);
        w = std::max(1, static_cast<int>(w * k));
        h = std::max(1, static_cast<int>(h * k));
        small = core::canvas::resampled(m, w, h, core::canvas::Filter::Smooth, cancelled);
        if (!small) return std::nullopt;
    }
    const pixel::Format f8{pixel::Depth::U8, pixel::Mode::RGB, true};
    const pixel::TileMap& src = small ? *small : m;
    const pixel::TileMap m8 = src.format() == f8 ? src : core::transfer::converted(src, f8);
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) m8.row_bytes(y, bytes->data() + static_cast<std::size_t>(y) * w * 4);
    return Shown{w, h, bytes};
}
}  // namespace

void Shell::ask_raw(const std::string& path) {
    namespace R = core::raw;
    if (!m_raw_dialog) {
        m_raw_dialog = std::make_unique<RawDialog>(*this);
        m_raw_dialog->signal_changed().connect([this](R::Develop d) { raw_preview_soon(d); });
        m_raw_dialog->signal_gone().connect([this] {
            if (m_raw_soon.connected()) m_raw_soon.disconnect();
            if (m_raw_job) { m_jobs.cancel(m_raw_job); m_raw_job = 0; }
        });
        m_raw_dialog->signal_open().connect([this](std::string p, R::Develop d) {
            if (m_raw_soon.connected()) m_raw_soon.disconnect();
            if (m_raw_job) { m_jobs.cancel(m_raw_job); m_raw_job = 0; }
            R::set_defaults(d);   // what a plain open of a RAW uses from now on
            m_prefs.raw_develop = R::develop_text(d);
            core::save_prefs(m_prefs_file, m_prefs);
            if (auto lg = log::get(log::Area::Shell)) lg->info("raw: develop {} with {}", p, m_prefs.raw_develop);
            open_paths({p});
        });
    }
    m_folder = fs::path(path).parent_path().string();
    m_raw_dialog->ask(path, R::defaults());
    // At once: what the file says and the camera's own preview; then a develop.
    if (m_raw_job) { m_jobs.cancel(m_raw_job); m_raw_job = 0; }
    m_raw_job = start_job("Read " + fs::path(path).filename().string(), [this, path](core::Jobs::Token& t) -> core::Jobs::Landing {
        std::string err;
        const auto info = R::info(path, err);
        const std::string line = info ? info->line() : "Can't read it: " + err;
        std::string e2;
        auto pv = R::preview(path, e2);
        std::optional<Shown> s;
        if (pv) s = shown_of(*pv, 1100, [&t] { return t.cancelled(); });
        return [this, path, line, s] {
            m_raw_job = 0;
            if (!m_raw_dialog || !m_raw_dialog->get_visible() || m_raw_dialog->path() != path) return;
            m_raw_dialog->show_info(line);
            if (s) m_raw_dialog->show_picture(s->w, s->h, s->rgba, "The camera's own preview -- developing…");
            raw_preview_soon(m_raw_dialog->current());
        };
    });
}

void Shell::raw_preview_soon(const core::raw::Develop& d) {
    if (m_raw_soon.connected()) m_raw_soon.disconnect();
    if (!m_raw_dialog) return;
    const std::string path = m_raw_dialog->path();
    m_raw_soon = Glib::signal_timeout().connect([this, d, path] {
        if (m_raw_job) { m_jobs.cancel(m_raw_job); m_raw_job = 0; }
        m_raw_job = start_job("Develop preview", [this, d, path](core::Jobs::Token& t) -> core::Jobs::Landing {
            formats::Progress p;
            p.cancelled = [&t] { return t.cancelled(); };
            std::string err;
            auto dev = core::raw::develop(path, d, true, err, p);
            if (!dev) {
                if (t.cancelled()) return [] {};
                return [this, err] { m_raw_job = 0; if (m_raw_dialog) m_raw_dialog->show_info("Can't develop it: " + err); };
            }
            auto s = shown_of(dev->pixels, 1100, [&t] { return t.cancelled(); });
            if (!s) return [] {};
            return [this, s, d, path] {
                m_raw_job = 0;
                if (!m_raw_dialog || !m_raw_dialog->get_visible() || m_raw_dialog->path() != path || !(m_raw_dialog->current() == d)) return;
                m_raw_dialog->show_picture(s->w, s->h, s->rgba, "Developed at half size -- Open develops it whole");
            };
        });
        return false;
    }, 150);
}

// ── s082: smart layers -- File > Place, Layer > Smart Layer ────────────────
namespace L = core::layers;

void Shell::on_place(bool linked) {
    if (!has_image() || file_job_busy()) return;
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title(linked ? "Place Linked" : "Place Embedded");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto all = filter("Images and documents", {"klimt", "ora", "psd", "png", "jpg", "jpeg", "tif", "tiff", "webp"});
    filters->append(all);
    dlg->set_filters(filters);
    dlg->set_default_filter(all);
    if (!m_folder.empty()) dlg->set_initial_folder(Gio::File::create_for_path(m_folder));
    dlg->open(*this, [this, dlg, linked](Glib::RefPtr<Gio::AsyncResult>& res) {
        Glib::RefPtr<Gio::File> f;
        try { f = dlg->open_finish(res); } catch (const Glib::Error&) { return; }
        if (f && !f->get_path().empty()) place_path(f->get_path(), linked, {}, linked ? "Place Linked" : "Place");
    });
}

void Shell::place_path(const std::string& path, bool linked, const std::string& replace_id, const std::string& label) {
    if (!has_image() || file_job_busy()) return;
    const std::string name = fs::path(path).filename().string();
    m_file_job = start_job(label + " " + name, [this, path, linked, replace_id, label, name](core::Jobs::Token& t) -> core::Jobs::Landing {
        std::string err;
        auto got = core::smart::load(path, err, progress_of(t));
        if (!got) {
            if (t.cancelled()) return [] {};
            return [this, name, err, label] { toast(label + ": couldn't read \u201c" + name + "\u201d -- " + err); };
        }
        return [this, path, linked, replace_id, label, got = std::make_shared<core::smart::Loaded>(std::move(*got))] {
            const auto why = core::smart::refusal(got->pixels.format(), m_doc.image());
            if (!why.empty()) { toast(label + ": " + why); return; }
            const std::string link = linked ? path : std::string{};
            if (replace_id.empty()) {
                const auto made = L::place_smart(m_journal, m_doc, got->pixels, got->name, got->dpi, link, got->stamp, got->bytes, m_layers.primary());
                if (made.empty()) { toast(label + ": refused"); return; }
                m_layers.select({made}, false);
                on_selection();
                toast(label + " \u201c" + got->name + "\u201d -- a smart layer: transform it freely, it stays whole", true);
            } else if (L::replace_smart(m_journal, m_doc, replace_id, got->pixels, got->name, got->dpi, link, got->stamp, got->bytes, label)) {
                toast(m_journal.undo_label(), true);
            } else {
                toast(label + ": nothing changed");
            }
            if (auto lg = log::get(log::Area::Shell)) lg->info("smart: {} {} ({} x {}, {})", label, got->name, got->pixels.width(), got->pixels.height(), linked ? "linked" : "embedded");
        };
    });
}

void Shell::check_links() {
    if (!has_image()) return;
    std::string said;
    bool any = false;
    for (const auto& id : m_doc.walk()) {
        const auto l = m_doc.get(id);
        if (!l || l->kind != core::Kind::Smart || !l->smart || l->smart->link.empty()) continue;
        any = true;
        const auto now = core::smart::stamp_of(l->smart->link);
        if (core::smart::status(*l->smart) != core::smart::Link::Modified) continue;
        auto& seen = m_link_said[l->smart->link];
        if (seen == now.stamp) continue;   // told already
        seen = now.stamp;
        said = "\u201c" + l->smart->name + "\u201d has changed -- Layer > Smart Layer > Update Modified Content";
    }
    if (!any) return;
    m_layers.queue_repaint();
    on_selection();   // Properties' link line
    if (!said.empty()) toast(said);
}

void Shell::on_smart(const std::string& verb) {
    const auto id = m_layers.primary();
    const auto l = id.empty() ? std::nullopt : m_doc.get(id);
    if (!l) { toast("Pick a layer first"); return; }
    if (verb == "convert") {
        if (!L::to_smart(m_journal, m_doc, id)) toast("Convert to Smart Layer: " + (l->kind == core::Kind::Pixel ? std::string("the layer is empty or locked") : L::not_pixels(m_doc, id)));
        return;
    }
    if (l->kind != core::Kind::Smart || !l->smart) { toast("\u201c" + l->name + "\u201d isn't a smart layer"); return; }
    if (verb == "rasterize") { L::rasterize_smart(m_journal, m_doc, id); return; }
    if (verb == "edit") { edit_contents(id); return; }   // s083
    if (verb == "embed") { if (!L::embed_smart(m_journal, m_doc, id)) toast("Embed Linked: it's embedded already"); return; }
    if (verb == "update") {
        const auto st = core::smart::status(*l->smart);
        if (st == core::smart::Link::Embedded) { toast("Update: it's embedded -- there's no file to read"); return; }
        if (st == core::smart::Link::Missing) { toast("Update: \u201c" + l->smart->link + "\u201d isn't there -- Replace Contents to point it at another file"); return; }
        if (st == core::smart::Link::Current) { toast("Update: the file hasn't changed"); return; }
        place_path(l->smart->link, true, id, "Update Modified Content");
        return;
    }
    if (verb == "replace") {
        const bool linked = !l->smart->link.empty();
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Replace Contents");
        auto filters = Gio::ListStore<Gtk::FileFilter>::create();
        auto all = filter("Images and documents", {"klimt", "ora", "psd", "png", "jpg", "jpeg", "tif", "tiff", "webp"});
        filters->append(all);
        dlg->set_filters(filters);
        dlg->set_default_filter(all);
        if (!m_folder.empty()) dlg->set_initial_folder(Gio::File::create_for_path(m_folder));
        dlg->open(*this, [this, dlg, linked, id](Glib::RefPtr<Gio::AsyncResult>& res) {
            Glib::RefPtr<Gio::File> f;
            try { f = dlg->open_finish(res); } catch (const Glib::Error&) { return; }
            if (f && !f->get_path().empty()) place_path(f->get_path(), linked, id, "Replace Contents");
        });
    }
}

// s020: several files -- each in its own tab, one open job at a time.
void Shell::open_paths(const std::vector<std::string>& paths, const std::string& hint) {
    for (const auto& p : paths) {
        if (!hint.empty()) m_open_hint[p] = hint;
        if (auto lg = log::get(log::Area::Shell)) lg->info("open requested: {}{}", p, hint.empty() ? "" : " (" + hint + ")");
    }
    m_open_queue.insert(m_open_queue.end(), paths.begin(), paths.end());
    open_next();
}

void Shell::open_next() {
    using S = core::Jobs::State;
    if (m_open_queue.empty()) {
        // s023: a restored session -- its front tab, once all are open.
        if (!m_restore_front.empty()) {
            const auto want = m_restore_front;
            m_restore_front.clear();
            for (std::size_t i = 0; i < m_tabs.count(); ++i)
                if (m_tabs.info(i).path == want || m_tabs.info(i).source == want) { show_tab(i); break; }
        }
        return;
    }
    if (m_file_job) {
        const auto st = m_jobs.state(m_file_job);
        if (st == S::Queued || st == S::Running || st == S::Done) return;   // its landing calls again
    }
    const std::string p = m_open_queue.front();
    m_open_queue.erase(m_open_queue.begin());
    open_path(p);
    if (!m_file_job || m_jobs.state(m_file_job) == S::Landed) open_next();   // nothing started (already open)
}

void Shell::open_path(const std::string& path) {
    if (file_job_busy()) return;
    const std::string name = fs::path(path).filename().string();
    m_folder = fs::path(path).parent_path().string();
    // s020: already open in a tab? That tab, not a second copy.
    std::error_code ec;
    const std::string canon = fs::weakly_canonical(path, ec).string();
    for (std::size_t i = 0; i < m_tabs.count(); ++i) {
        const auto& p = m_tabs.info(i).path;
        if (!p.empty() && (p == path || fs::weakly_canonical(p, ec).string() == canon)) {
            if (show_tab(i)) toast("“" + name + "” is open here already");
            m_open_hint.erase(path);
            return;
        }
        // s083: Composix asking again for a file a tab already writes back to: that tab.
        const auto& b = m_tabs.info(i).back;
        if (!b.empty() && (b == path || fs::weakly_canonical(b, ec).string() == canon)) {
            m_open_hint.erase(path);
            if (show_tab(i)) toast("“" + name + "” is open here already -- Save writes it back");
            return;
        }
    }
    std::string hint;
    if (const auto h = m_open_hint.find(path); h != m_open_hint.end()) { hint = h->second; m_open_hint.erase(h); }
    {
        m_file_job = start_job("Open " + name, [this, path, name, hint](core::Jobs::Token& t) -> core::Jobs::Landing {
            std::string err;
            auto c = core::file::open(path, err, progress_of(t));
            if (!c) {
                if (t.cancelled()) return [] {};
                return [this, name, err] {
                    toast("Couldn't open “" + name + "”: " + err);
                    Glib::signal_idle().connect_once([this] { open_next(); });
                };
            }
            auto shared = std::make_shared<core::file::Contents>(std::move(*c));
            return [this, shared, path, name, hint] {
                const bool native = shared->kind == formats::Kind::Klimt;
                const auto kind = shared->kind;
                // s020: a tab of its own -- made now, when there is something to show.
                if (!tab_reusable() && !new_tab()) { toast("“" + name + "” opened, but the image can't be shown now -- try again"); return; }
                adopt(std::move(*shared), native ? path : std::string{}, name, path);
                if (hint == core::roundtrip::kComposixHint) mark_back(m_tabs.live(), path, kind, "Composix");   // s083
                if (hint.rfind("smart:", 0) == 0) {   // s083 m3: Edit Contents of a linked smart layer
                    const auto c2 = hint.find(':', 6);
                    auto& info = m_tabs.live_info();
                    info.owner_key = std::stoull(hint.substr(6, c2 - 6));
                    info.owner_layer = hint.substr(c2 + 1);
                    mark_back(m_tabs.live(), path, kind, "");
                    toast("Editing “" + name + "” -- Save (Ctrl+S) writes the file and updates its smart layer");
                }
                note_recent(path);   // s054
                Glib::signal_idle().connect_once([this] { open_next(); });
            };
        });
    }
}

// ── s022: files dropped from the file manager ──────────────────────────────
namespace {
std::vector<std::string> paths_of(const GValue* value) {
    std::vector<std::string> out;
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return out;
    auto* list = static_cast<GdkFileList*>(g_value_get_boxed(value));
    GSList* files = list ? gdk_file_list_get_files(list) : nullptr;
    for (GSList* l = files; l; l = l->next)
        if (char* p = g_file_get_path(G_FILE(l->data))) { out.emplace_back(p); g_free(p); }
    g_slist_free(files);
    return out;
}
}  // namespace

// Onto the canvas with an image open: each file a layer (or, a .klimt, its
// layers) in this document. Anywhere else, or no image: each file a tab.
void Shell::build_file_drops() {
    const auto make = [this](GtkWidget* on, bool place) {
        auto* t = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
        g_object_set_data(G_OBJECT(t), "klimt-place", GINT_TO_POINTER(place ? 1 : 0));
        g_signal_connect(t, "drop", G_CALLBACK(+[](GtkDropTarget* tg, const GValue* value, double, double, gpointer self) -> gboolean {
            auto* sh = static_cast<Shell*>(self);
            const auto paths = paths_of(value);
            if (paths.empty()) return FALSE;
            const bool place = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(tg), "klimt-place")) != 0 && sh->has_image();
            if (auto lg = log::get(log::Area::Shell)) lg->info("files dropped: {} ({})", paths.size(), place ? "place" : "open");
            Glib::signal_idle().connect_once([sh, paths, place] {
                if (place) sh->place_paths(paths);
                else sh->open_paths(paths);
            });
            return TRUE;
        }), this);
        gtk_widget_add_controller(on, GTK_EVENT_CONTROLLER(t));
    };
    make(GTK_WIDGET(m_canvas.gobj()), true);
    make(GTK_WIDGET(gobj()), false);
}

void Shell::place_paths(const std::vector<std::string>& paths) {
    if (file_job_busy() || doc_busy(true)) return;
    const auto key = m_tabs.live_info().key;
    m_file_job = start_job(paths.size() == 1 ? "Place " + fs::path(paths[0]).filename().string() : "Place " + std::to_string(paths.size()) + " files",
                           [this, paths, key](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto got = std::make_shared<std::vector<std::pair<std::string, core::file::Contents>>>();
        std::string errors;
        for (const auto& p : paths) {
            if (t.cancelled()) return [] {};
            std::string err;
            auto c = core::file::open(p, err, progress_of(t));
            const auto name = fs::path(p).filename().string();
            if (c) got->emplace_back(name, std::move(*c));
            else errors += (errors.empty() ? "" : "; ") + name + ": " + err;
        }
        return [this, got, errors, key] {
            if (!errors.empty()) toast("Couldn't place " + errors);
            if (m_tabs.live_info().key != key) { toast("Place: the image changed tabs meanwhile -- nothing done"); return; }
            std::vector<core::Document::Id> all;
            for (auto& [name, c] : *got) {
                // The file as a document of its own, then its layers carried in.
                core::Document tmp;
                {
                    core::Document::Loading load(tmp);
                    tmp.replace(c.image, std::move(c.layers), c.next_id);
                }
                auto b = core::transfer::take(tmp, tmp.panel(""));
                if (b.layers.size() == 1 && c.kind != formats::Kind::Klimt) {   // a flat image: one layer, its file's name
                    b.layers[0].second.name = name;
                    b.name = name;
                }
                const auto why = core::transfer::refusal(b, m_doc.image());
                if (!why.empty()) { toast("Can't place “" + name + "”: " + why); continue; }
                const auto made = core::layers::paste(m_journal, m_doc, b, m_layers.primary());
                all.insert(all.end(), made.begin(), made.end());
            }
            if (!all.empty()) {
                m_journal.reselect(all);
                m_layers.select(all, true);
            }
        };
    });
}

// ── Save ────────────────────────────────────────────────────────────────────
void Shell::on_save() {
    text_close();   // s046: the visit is a step before it is saved
    if (!has_image()) { toast("No image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (!m_tabs.live_info().back.empty() || m_tabs.live_info().owner_key) { save_back(); return; }   // s083: back where it came from
    if (path_().empty()) { on_save_as(); return; }
    if (file_job_busy()) return;
    save_to(path_());
}

void Shell::on_save_as() {
    text_close();   // s046: the visit is a step before it is saved
    if (!has_image()) { toast("No image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (file_job_busy()) return;
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Save As");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter("Klimt document (.klimt)", {"klimt"}));
    dlg->set_filters(filters);
    dlg->set_initial_name(stem_of(name_()) + ".klimt");
    if (!m_folder.empty()) dlg->set_initial_folder(Gio::File::create_for_path(m_folder));
    dlg->save(*this, [this, dlg](Glib::RefPtr<Gio::AsyncResult>& res) {
        Glib::RefPtr<Gio::File> f;
        try { f = dlg->save_finish(res); } catch (const Glib::Error&) { return; }
        if (!f) return;
        std::string path = f->get_path();
        // Save As writes Klimt's own file; an image name becomes a .klimt
        // (Export writes images).
        if (lower_ext(path) != ".klimt") path = fs::path(path).replace_extension(".klimt").string();
        save_to(path);
    });
}

// The save as a job, from a SNAPSHOT: the document may change while it
// writes, so the "saved" mark is the one the snapshot was taken at.
core::file::Contents Shell::contents_now() {
    core::file::Contents c;
    c.image = m_doc.image();
    c.layers = m_doc.snapshot();
    c.channels = m_doc.channels();   // s016
    c.selection = m_doc.selection_ptr();   // s026
    c.next_id = m_doc.next_id();
    {   // s056: the view goes with it (WEAKNESSES 8, 21)
        const auto v = m_canvas.view();
        c.view = {true, v.fit, v.zoom, v.cx, v.cy, m_layers.collapsed()};
    }
    return c;
}

void Shell::save_to(const std::string& path, std::function<void()> then) {
    if (file_job_busy()) return;
    core::file::Contents c = contents_now();
    const auto mark = m_journal.mark();
    const std::string name = fs::path(path).filename().string();
    m_folder = fs::path(path).parent_path().string();
    const auto key = m_tabs.live_info().key;   // s020: the landing finds its tab by key
    m_file_job = start_job("Save " + name, [this, c = std::move(c), path, name, mark, then, key](core::Jobs::Token& t)
                                               -> core::Jobs::Landing {
        std::string err;
        if (!core::file::save_klimt(path, c, err, progress_of(t))) {
            if (t.cancelled()) return [this] { toast("Save cancelled -- the file is as it was"); };
            return [this, name, err] { toast("Couldn't save “" + name + "”: " + err); };
        }
        return [this, path, name, mark, then, key] {
            const auto i = m_tabs.by_key(key);
            if (!i) { toast("Saved “" + name + "”"); return; }   // its tab was closed meanwhile
            auto& info = m_tabs.info(*i);
            // s083: saved as a .klimt of its own -- it no longer writes back.
            const std::string was = info.back.empty() ? std::string{} : info.from;
            const bool owned = info.owner_key != 0;
            info.back.clear(); info.from.clear(); info.back_kind = formats::Kind::Unknown;
            info.owner_key = 0; info.owner_layer.clear();
            info.path = path;
            info.name = name;
            info.saved_mark = mark;
            note_recent(path);   // s054
            update_title();
            toast("Saved “" + name + "”" + (!was.empty() ? " -- " + was + "'s file is no longer saved from this tab"
                                            : owned ? " -- the smart layer is no longer saved from this tab" : ""));
            if (then) then();
            Glib::signal_idle().connect_once([this] { open_next(); });   // s020: a file waiting to open
        };
    });
}

// ── s083: Save writes back (the Composix round-trip) ───────────────────────
// The tab Composix handed us: Save writes ITS file, in its format (a PDF with
// the document inside it -- core/RoundTrip.hpp), atomically; Composix's
// file monitor sees the change. Snapshot, then a job, as save_to.
void Shell::mark_back(std::size_t tab, const std::string& file, formats::Kind kind, const std::string& from) {
    if (tab >= m_tabs.count()) return;
    const std::string name = fs::path(file).filename().string();
    if (!core::roundtrip::writes_back(kind)) {
        if (from.empty()) return;
        toast(from + " asked to edit “" + name + "”, but Klimt can't write that kind of file -- Save makes a .klimt");
        return;
    }
    auto& info = m_tabs.info(tab);
    info.back = file;
    info.back_kind = kind;
    info.from = from;
    if (kind == formats::Kind::Klimt) info.path = file;
    update_summary();
    if (auto lg = log::get(log::Area::Shell)) lg->info("tab {} writes back: {} ({}) for {}", info.key, file, core::roundtrip::writes_back(kind), from);
    update_title();
    if (!from.empty()) toast("“" + name + "” from " + from + " -- Save (Ctrl+S) writes it back as " + core::roundtrip::writes_back(kind));
}

std::string Shell::back_line(std::size_t tab) const {
    if (tab >= m_tabs.count()) return {};
    const auto& info = m_tabs.info(tab);
    std::string line;
    if (!info.back.empty())
        line = (info.from.empty() ? std::string{} : "for " + info.from + ": ") + "Save writes " + fs::path(info.back).filename().string() + " (" +
               core::roundtrip::writes_back(info.back_kind) + ")";
    if (info.owner_key) {
        const auto o = m_tabs.by_key(info.owner_key);
        line += std::string(line.empty() ? "a smart layer's picture: Save puts it back" : ", and into its smart layer") +
                (o ? " in “" + m_tabs.info(*o).name + "”" : " (its image was closed)");
    }
    return line;
}

void Shell::save_back(std::function<void()> then) {
    if (file_job_busy()) return;
    auto& in = m_tabs.live_info();
    if (in.back.empty()) { save_into_owner(then); return; }   // m3: Edit Contents
    const std::string path = in.back;
    const formats::Kind kind = in.back_kind;
    const std::string from = in.from;
    // m3: a LINKED smart layer's file -- written, then its layer updated from it.
    const auto owner = in.owner_key ? m_tabs.by_key(in.owner_key) : std::nullopt;
    const std::optional<pixel::Format> owner_format = owner ? std::optional(m_tabs.image(*owner).format) : std::nullopt;
    core::file::Contents c = contents_now();
    const auto mark = m_journal.mark();
    const std::string name = fs::path(path).filename().string();
    const auto key = in.key;
    const formats::WriteOptions o = m_prefs.export_options;
    m_file_job = start_job("Save " + name, [this, c = std::move(c), path, kind, name, mark, then, key, from, o, owner_format](core::Jobs::Token& t)
                                               -> core::Jobs::Landing {
        std::string err;
        std::vector<std::string> notes;
        if (!core::roundtrip::save_back(path, kind, c, err, notes, progress_of(t), o)) {
            if (t.cancelled()) return [this] { toast("Save cancelled -- the file is as it was"); };
            return [this, name, err] { toast("Couldn't save “" + name + "”: " + err + " -- the file is as it was"); };
        }
        std::shared_ptr<const pixel::TileMap> pic;
        if (owner_format) {
            formats::Progress cancel_only;
            cancel_only.cancelled = [&t] { return t.cancelled(); };
            if (auto m = core::roundtrip::picture_of(c, *owner_format, cancel_only)) pic = std::make_shared<const pixel::TileMap>(std::move(*m));
        }
        return [this, path, name, mark, then, key, from, notes, pic] {
            if (pic) {
                const auto st = core::smart::stamp_of(path);
                into_owner(key, *pic, st.stamp, st.bytes);
            }
            std::string said;
            for (const auto& n : notes) said += " -- " + n;
            if (auto lg = log::get(log::Area::Shell)) lg->info("saved back: {}{}", path, said);
            const auto i = m_tabs.by_key(key);
            if (i) m_tabs.info(*i).saved_mark = mark;
            update_title();
            if (!from.empty()) toast("Saved “" + name + "” for " + from + said);
            else toast("Saved “" + name + "”" + said);
            if (then) then();
            Glib::signal_idle().connect_once([this] { open_next(); });
        };
    });
}

// ── s083 m3: Edit Contents ─────────────────────────────────────────────────
// Layer > Smart Layer > Edit Contents: the picture in a tab of its own (a
// linked one: its FILE, which Save then writes as well). Save puts what that
// tab draws back into the smart layer -- ONE step "Edit Contents" in its
// image, which comes forward for it and then gives way again.
void Shell::edit_contents(const core::Document::Id& id) {
    const auto l = m_doc.get(id);
    if (!l || l->kind != core::Kind::Smart || !l->smart || !l->smart->source) return;
    const auto owner = m_tabs.live_info().key;
    for (std::size_t i = 0; i < m_tabs.count(); ++i)   // being edited already: that tab
        if (m_tabs.info(i).owner_key == owner && m_tabs.info(i).owner_layer == id) { show_tab(i); return; }
    const auto& sm = *l->smart;
    if (!sm.link.empty() && core::smart::status(sm) != core::smart::Link::Missing &&
        core::roundtrip::writes_back(core::roundtrip::kind_of(sm.link))) {
        if (core::smart::status(sm) == core::smart::Link::Modified)
            toast("“" + sm.name + "” changed on disk -- this opens the file as it is now");
        m_open_hint[sm.link] = "smart:" + std::to_string(owner) + ":" + id;
        open_paths({sm.link});
        return;
    }
    if (file_job_busy()) return;
    auto c = core::roundtrip::contents_of(sm, m_doc.image());
    const std::string name = sm.name.empty() ? std::string("Picture") : sm.name;
    if (!new_tab()) return;
    adopt(std::move(c), "", name);
    auto& info = m_tabs.live_info();
    info.owner_key = owner;
    info.owner_layer = id;
    update_title();
    update_summary();
    if (auto lg = log::get(log::Area::Shell)) lg->info("edit contents: {} (embedded) from tab {}", name, owner);
    toast("Editing “" + name + "” -- Save (Ctrl+S) puts it back in its smart layer");
}

void Shell::save_into_owner(std::function<void()> then) {
    if (file_job_busy()) return;
    auto& in = m_tabs.live_info();
    const auto owner = m_tabs.by_key(in.owner_key);
    if (!owner) { toast("Its image was closed -- Save As keeps this picture as a .klimt"); return; }
    const pixel::Format f = m_tabs.image(*owner).format;
    core::file::Contents c = contents_now();
    const auto mark = m_journal.mark();
    const auto key = in.key;
    const std::string name = in.name;
    m_file_job = start_job("Edit Contents " + name, [this, c = std::move(c), f, mark, key, then](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto pic = core::roundtrip::picture_of(c, f, progress_of(t));
        if (!pic) return [] {};
        auto shared = std::make_shared<const pixel::TileMap>(std::move(*pic));
        return [this, shared, mark, key, then] {
            if (!into_owner(key, *shared, 0, 0)) return;
            if (const auto i = m_tabs.by_key(key)) m_tabs.info(*i).saved_mark = mark;
            update_title();
            if (then) then();
            Glib::signal_idle().connect_once([this] { open_next(); });
        };
    });
}

// The picture into the smart layer the tab `key` edits: its image comes
// forward, the step lands there, and the tab `key` comes back. False when
// the image or the layer is gone (said).
bool Shell::into_owner(std::uint64_t key, const pixel::TileMap& picture, std::int64_t stamp, std::uint64_t bytes) {
    const auto me = m_tabs.by_key(key);
    if (!me) return false;
    const auto owner_key = m_tabs.info(*me).owner_key;
    const auto layer = m_tabs.info(*me).owner_layer;
    const auto o = m_tabs.by_key(owner_key);
    if (!o) { toast("Its image was closed -- Save As keeps this picture as a .klimt"); return false; }
    const std::string owner_name = m_tabs.info(*o).name;
    if (!show_tab(*o)) { toast("Edit Contents: “" + owner_name + "” is busy -- Save again in a moment"); return false; }
    const auto l = m_doc.get(layer);
    bool ok = false;
    if (!l || l->kind != core::Kind::Smart || !l->smart) {
        toast("Edit Contents: the smart layer isn't in “" + owner_name + "” any more -- Save As keeps this picture");
    } else {
        const auto& sm = *l->smart;
        ok = L::replace_smart(m_journal, m_doc, layer, picture, sm.name, sm.dpi, sm.link, sm.link.empty() ? 0 : stamp,
                              sm.link.empty() ? 0 : bytes, "Edit Contents");
        if (!sm.link.empty()) m_link_said[sm.link] = stamp;   // our own write: no "it changed" toast
        m_layers.queue_repaint();
        on_selection();
        if (auto lg = log::get(log::Area::Shell)) lg->info("edit contents: back into {} in {} ({})", layer, owner_name, ok ? "a step" : "no change");
    }
    if (const auto back = m_tabs.by_key(key)) show_tab(*back);
    if (ok) toast("“" + m_tabs.info(*m_tabs.by_key(key)).name + "” put back in “" + owner_name + "” -- one step there (Undo in that tab)");
    else if (l && l->kind == core::Kind::Smart) toast("Edit Contents: nothing changed");
    return true;
}

// ── Export ──────────────────────────────────────────────────────────────────
void Shell::on_export() {
    text_close();   // s046: the visit is a step before it is saved
    if (!has_image()) { toast("No image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (file_job_busy()) return;
    // s051 (Scott: "I expect that the dropdown in the save dlg have the file
    // types"): the chooser's type menu IS the format -- one type per format,
    // starting on the last one exported; the type picked decides the format
    // and adds the extension. GtkFileChooserNative is deprecated in GTK 4.10
    // but whole, and the only API that reports the type picked.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    // s051 (Scott: "remove all extension and when saved it auto adds" --
    // "show extensions removed on purpose"): GNOME's portal chooser never
    // says its type changed, so the name goes in WITHOUT an extension, the
    // title says why, and the type picked adds it on Export. Its own
    // "replace?" can't see the real name, so the settings dialog asks
    // (its OK becomes Replace).
    GtkFileChooserNative* n = gtk_file_chooser_native_new("Export \u2014 the extension comes from the type below",
                                                          GTK_WINDOW(gobj()), GTK_FILE_CHOOSER_ACTION_SAVE, "_Export", "_Cancel");
    auto* fc = GTK_FILE_CHOOSER(n);
    const auto* last = formats::export_format(m_prefs.export_format);
    if (!last) last = formats::export_format(formats::Kind::PNG);
    for (const auto& ef : formats::export_formats()) {
        GtkFileFilter* f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, (std::string(ef.label) + "  (" + ef.ext + ")").c_str());
        gtk_file_filter_add_suffix(f, ef.ext + 1);
        if (ef.kind == formats::Kind::JPEG) gtk_file_filter_add_suffix(f, "jpeg");
        if (ef.kind == formats::Kind::TIFF) gtk_file_filter_add_suffix(f, "tiff");
        g_object_set_data(G_OBJECT(f), "klimt-kind", GINT_TO_POINTER(static_cast<int>(ef.kind) + 1));
        gtk_file_chooser_add_filter(fc, f);
        if (&ef == last) gtk_file_chooser_set_filter(fc, f);
        g_object_unref(f);
    }
    gtk_file_chooser_set_current_name(fc, stem_of(name_()).c_str());   // no extension: the type adds it
    // Scott: "as a hint or an info bubble" -- the portal takes no hint in its
    // name box, but it shows an app's CHOICES: a ticked box saying so, inside
    // the dialog, which also lets a name be written exactly as typed.
    gtk_file_chooser_add_choice(fc, "klimt-ext", "Add the extension from the type (.png, .pdf ...)", nullptr, nullptr);
    gtk_file_chooser_set_choice(fc, "klimt-ext", "true");
    if (!m_folder.empty()) {
        GFile* dir = g_file_new_for_path(m_folder.c_str());
        gtk_file_chooser_set_current_folder(fc, dir, nullptr);
        g_object_unref(dir);
    }
    struct Ctx { Shell* self; };
    g_signal_connect(n, "response", G_CALLBACK(+[](GtkNativeDialog* d, int response, gpointer data) {
        auto* self = static_cast<Ctx*>(data)->self;
        auto* fc = GTK_FILE_CHOOSER(d);
        std::string path;
        formats::Kind kind = formats::Kind::Unknown;
        bool as_typed = false;
        if (response == GTK_RESPONSE_ACCEPT) {
            if (GFile* f = gtk_file_chooser_get_file(fc)) {
                if (char* p = g_file_get_path(f)) { path = p; g_free(p); }
                g_object_unref(f);
            }
            if (GtkFileFilter* flt = gtk_file_chooser_get_filter(fc))
                kind = static_cast<formats::Kind>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(flt), "klimt-kind")) - 1);
            const char* c = gtk_file_chooser_get_choice(fc, "klimt-ext");
            as_typed = c && std::string(c) == "false";
        }
        g_object_unref(d);
        delete static_cast<Ctx*>(data);
        if (!path.empty()) self->export_asked(path, kind, as_typed);
    }), new Ctx{this});
    gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(n), TRUE);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(n));
#pragma GCC diagnostic pop
}

// s051: the chooser answered -- its type picked (Unknown: it didn't say; the
// name's extension, else the last format), the name's extension set to it.
void Shell::export_asked(const std::string& chosen, formats::Kind kind, bool as_typed) {
    if (!formats::export_format(kind)) {
        const auto* by_name = formats::export_format_of_name(chosen);
        const auto* last = formats::export_format(m_prefs.export_format);
        kind = by_name ? by_name->kind : last ? last->kind : formats::Kind::PNG;
    }
    // The box unticked: the name exactly as typed (the type still picks the format).
    const std::string path = as_typed ? chosen : formats::with_extension(chosen, kind);
    if (!m_export_dialog) {
        m_export_dialog = std::make_unique<ExportDialog>(*this);
        m_export_dialog->signal_ok().connect([this](formats::WriteOptions o, formats::Kind k, std::string p) {
            m_prefs.export_options = o;   // remembered per format
            if (const auto* ef = formats::export_format(k)) m_prefs.export_format = ef->key;   // s051
            core::save_prefs(m_prefs_file, m_prefs);
            export_to(p, k, o);
        });
    }
    m_export_dialog->ask(kind, path, chosen, m_prefs.export_options, [this](formats::Kind k) {
        const auto im = m_doc.image();
        const auto ls = m_doc.snapshot();
        return k == formats::Kind::ORA   ? core::ora::plan(im, ls)
             : k == formats::Kind::PSD   ? core::psd::plan(im, ls)
             : k == formats::Kind::PDF   ? core::pdf::plan(im, ls).list
             : k == formats::Kind::EPS   ? core::eps::plan(im, ls)   // s080
                                         : core::ora::Plan{};
    });
}

void Shell::export_to(const std::string& path, formats::Kind kind, const formats::WriteOptions& o) {
    if (!has_image() || file_job_busy()) return;
    const core::Image im = m_doc.image();
    auto layers = m_doc.snapshot();
    const std::string name = fs::path(path).filename().string();
    // s083: a PDF with Klimt's layers inside -- the whole document goes along.
    auto whole = kind == formats::Kind::PDF && o.pdf_layers ? std::make_shared<const core::file::Contents>(contents_now()) : nullptr;
    m_file_job = start_job("Export " + name, [this, im, layers = std::move(layers), path, kind, name, o, whole](core::Jobs::Token& t)
                                                 -> core::Jobs::Landing {
        std::string err;
        std::vector<std::string> notes;
        const bool ok = whole ? core::roundtrip::save_back(path, kind, *whole, err, notes, progress_of(t), o)
                        : kind == formats::Kind::ORA   ? core::ora::save(path, im, layers, err, notes, progress_of(t))
                        : kind == formats::Kind::PSD ? core::psd::save(path, im, layers, err, notes, progress_of(t))   // s051
                        : kind == formats::Kind::PDF ? core::pdf::save(path, im, layers, fs::path(path).stem().string(), err, notes, progress_of(t))
                        : kind == formats::Kind::EPS ? core::eps::save(path, im, layers, fs::path(path).stem().string(), o, err, notes, progress_of(t))   // s080
                                                     : core::file::export_image(path, kind, im, layers, err, progress_of(t), o);
        if (!ok) {
            if (t.cancelled()) return [] {};
            return [this, name, err] { toast("Couldn't export \u201c" + name + "\u201d: " + err); };
        }
        std::string said;
        for (const auto& n : notes) said += (said.empty() ? " -- " : ", ") + n;
        // s051 (Scott): a final word -- the name and where it went, as a dialog.
        return [this, name, said, path, notes] {
            toast("Exported \u201c" + name + "\u201d" + said);
            std::string detail = "Saved as \u201c" + name + "\u201d\nin " + fs::path(path).parent_path().string();
            for (const auto& n : notes) detail += "\n\u2022 " + n;
            auto a = Gtk::AlertDialog::create("Exported");
            a->set_detail(detail);
            a->set_buttons({"Show in Folder", "OK"});
            a->set_default_button(1);
            a->set_cancel_button(1);
            a->choose(*this, [this, a, path](Glib::RefPtr<Gio::AsyncResult>& r) {
                int b = 1;
                try { b = a->choose_finish(r); } catch (const Glib::Error&) {}
                if (b != 0) return;
                auto l = Gtk::FileLauncher::create(Gio::File::create_for_path(path));
                l->open_containing_folder(*this, [l](Glib::RefPtr<Gio::AsyncResult>& rr) {
                    try { l->open_containing_folder_finish(rr); } catch (const Glib::Error&) {}
                });
            });
        };
    });
}

// ── s054: File > Open Recent ────────────────────────────────────────────────
void Shell::note_recent(const std::string& path) {
    core::recents_add(m_recents, path);
    core::save_recents(m_recents_file, m_recents);
    sync_recents();
}

void Shell::sync_recents() {
    if (!m_recent_menu) return;
    m_recent_menu->remove_all();
    auto files = Gio::Menu::create();
    for (const auto& p : m_recents) {
        // The name, then its folder (two files of one name told apart); a
        // menu label's "_" is a mnemonic, so doubled.
        std::string label = fs::path(p).filename().string() + "   " + fs::path(p).parent_path().filename().string();
        for (std::size_t i = 0; (i = label.find('_', i)) != std::string::npos; i += 2) label.insert(i, "_");
        auto item = Gio::MenuItem::create(label, "");
        item->set_action_and_target("win.open-recent", Glib::Variant<Glib::ustring>::create(p));
        files->append_item(item);
    }
    if (m_recents.empty()) files->append("(nothing yet)", "win.none");
    m_recent_menu->append_section(files);
    if (!m_recents.empty()) {
        auto clear = Gio::Menu::create();
        clear->append("Clear Recent", "win.recent-clear");
        m_recent_menu->append_section(clear);
    }
}

}  // namespace klimt
