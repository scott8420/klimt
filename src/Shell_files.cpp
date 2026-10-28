#include "Shell.hpp"
#include <glibmm/main.h>
#include "NewDialog.hpp"
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
void Shell::adopt(core::file::Contents c, const std::string& path, const std::string& name) {
    {
        core::Document::Loading load(m_doc);
        m_doc.replace(c.image, std::move(c.layers), c.next_id);
        m_doc.replace_channels(std::move(c.channels));   // s016
    }
    m_journal.clear();
    m_tabs.live_info().saved_mark = m_journal.mark();
    path_() = path;
    name_() = name;
    m_layers.forget_view();
    const auto top = m_doc.panel("");
    if (!top.empty()) m_layers.select({top.front()}, false);
    // The old picture goes now; the next render starts the new one fresh.
    m_canvas.set_document(c.image.width, c.image.height);
    m_dirty = {};
    m_canvas.fit();
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
    if (m_closing) return false;
    if (file_job_busy()) return true;
    bool any = false;
    for (std::size_t i = 0; i < m_tabs.count(); ++i) any = any || m_tabs.dirty(i);
    if (!any) return false;
    // From idle: closing inside the dialog's own callback left GLib criticals
    // on the way out (seen under Xvfb, s019 too).
    close_all_then([this] { Glib::signal_idle().connect_once([this] { m_closing = true; close(); }); });
    return true;
}

// ── New ─────────────────────────────────────────────────────────────────────
// s009: launched without a file: the New dialog, once the window is up.
void Shell::start() {
    if (has_image()) return;
    Glib::signal_idle().connect_once([this] { if (!has_image()) on_new(); });
}

// s009: Help > Open Demo -- the demo document in place of this one.
void Shell::open_demo() {
    if (file_job_busy()) return;
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
        auto all = filter("Images and Klimt documents", {"klimt", "png", "jpg", "jpeg", "tif", "tiff"});
        filters->append(all);
        filters->append(filter("Klimt document (.klimt)", {"klimt"}));
        filters->append(filter("PNG", {"png"}));
        filters->append(filter("JPEG", {"jpg", "jpeg"}));
        filters->append(filter("TIFF", {"tif", "tiff"}));
        dlg->set_filters(filters);
        dlg->set_default_filter(all);
        if (!m_folder.empty()) dlg->set_initial_folder(Gio::File::create_for_path(m_folder));
        dlg->open(*this, [this, dlg](Glib::RefPtr<Gio::AsyncResult>& res) {
            Glib::RefPtr<Gio::File> f;
            try { f = dlg->open_finish(res); } catch (const Glib::Error&) { return; }
            if (f && !f->get_path().empty()) open_path(f->get_path());
        });
    }
}

// s020: several files -- each in its own tab, one open job at a time.
void Shell::open_paths(const std::vector<std::string>& paths) {
    m_open_queue.insert(m_open_queue.end(), paths.begin(), paths.end());
    open_next();
}

void Shell::open_next() {
    using S = core::Jobs::State;
    if (m_open_queue.empty()) return;
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
            return;
        }
    }
    {
        m_file_job = start_job("Open " + name, [this, path, name](core::Jobs::Token& t) -> core::Jobs::Landing {
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
            return [this, shared, path, name] {
                const bool native = shared->kind == formats::Kind::Klimt;
                // s020: a tab of its own -- made now, when there is something to show.
                if (!tab_reusable() && !new_tab()) { toast("“" + name + "” opened, but the image can't be shown now -- try again"); return; }
                adopt(std::move(*shared), native ? path : std::string{}, name);
                Glib::signal_idle().connect_once([this] { open_next(); });
            };
        });
    }
}

// ── Save ────────────────────────────────────────────────────────────────────
void Shell::on_save() {
    if (!has_image()) { toast("No image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (path_().empty()) { on_save_as(); return; }
    if (file_job_busy()) return;
    save_to(path_());
}

void Shell::on_save_as() {
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
void Shell::save_to(const std::string& path, std::function<void()> then) {
    if (file_job_busy()) return;
    core::file::Contents c;
    c.image = m_doc.image();
    c.layers = m_doc.snapshot();
    c.channels = m_doc.channels();   // s016
    c.next_id = m_doc.next_id();
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
            info.path = path;
            info.name = name;
            info.saved_mark = mark;
            update_title();
            toast("Saved “" + name + "”");
            if (then) then();
            Glib::signal_idle().connect_once([this] { open_next(); });   // s020: a file waiting to open
        };
    });
}

// ── Export ──────────────────────────────────────────────────────────────────
void Shell::on_export() {
    if (!has_image()) { toast("No image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (file_job_busy()) return;
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Export");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    auto any = filter("PNG, JPEG or TIFF", {"png", "jpg", "jpeg", "tif", "tiff"});
    filters->append(any);
    filters->append(filter("PNG", {"png"}));
    filters->append(filter("JPEG", {"jpg", "jpeg"}));
    filters->append(filter("TIFF", {"tif", "tiff"}));
    dlg->set_filters(filters);
    dlg->set_default_filter(any);
    dlg->set_initial_name(stem_of(name_()) + ".png");
    if (!m_folder.empty()) dlg->set_initial_folder(Gio::File::create_for_path(m_folder));
    dlg->save(*this, [this, dlg](Glib::RefPtr<Gio::AsyncResult>& res) {
        Glib::RefPtr<Gio::File> f;
        try { f = dlg->save_finish(res); } catch (const Glib::Error&) { return; }
        if (!f) return;
        std::string path = f->get_path();
        const auto ext = lower_ext(path);
        formats::Kind kind = formats::Kind::PNG;
        if (ext == ".jpg" || ext == ".jpeg") kind = formats::Kind::JPEG;
        else if (ext == ".tif" || ext == ".tiff") kind = formats::Kind::TIFF;
        else if (ext != ".png") path += ".png";
        const core::Image im = m_doc.image();
        auto layers = m_doc.snapshot();
        const std::string name = fs::path(path).filename().string();
        m_file_job = start_job("Export " + name, [this, im, layers = std::move(layers), path, kind, name](core::Jobs::Token& t)
                                                     -> core::Jobs::Landing {
            std::string err;
            if (!core::file::export_image(path, kind, im, layers, err, progress_of(t))) {
                if (t.cancelled()) return [] {};
                return [this, name, err] { toast("Couldn't export “" + name + "”: " + err); };
            }
            return [this, name] { toast("Exported “" + name + "”"); };
        });
    });
}

}  // namespace klimt
