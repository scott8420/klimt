#include "Shell.hpp"
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
    const std::string t = (dirty() ? "• " : "") + m_name;
    if (m_title) adw_window_title_set_title(m_title, t.c_str());
    set_title(m_name + " — Klimt");
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
    }
    m_journal.clear();
    m_saved_mark = m_journal.mark();
    m_path = path;
    m_name = name;
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
    auto a = Gtk::AlertDialog::create("Save changes to “" + m_name + "” before " + doing + "?");
    a->set_detail("Changes not saved are lost.");
    a->set_buttons({"Cancel", "Discard", "Save"});
    a->set_cancel_button(0);
    a->set_default_button(2);
    a->choose(*this, [this, a, go](Glib::RefPtr<Gio::AsyncResult>& res) {
        int i = 0;
        try { i = a->choose_finish(res); } catch (const Glib::Error&) { return; }
        if (i == 1) { go(); return; }
        if (i != 2) return;
        if (!m_path.empty()) { save_to(m_path, go); return; }
        // Never saved: Save As first, then go.
        auto dlg = Gtk::FileDialog::create();
        dlg->set_title("Save As");
        dlg->set_initial_name(stem_of(m_name) + ".klimt");
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

bool Shell::on_close_request() {
    if (m_closing) return false;
    if (file_job_busy()) return true;
    if (!dirty()) return false;
    ask_unsaved("closing", [this] { m_closing = true; close(); });
    return true;
}

// ── New ─────────────────────────────────────────────────────────────────────
void Shell::on_new() {
    if (file_job_busy()) return;
    ask_unsaved("making a new image", [this] {
        if (!m_new) {
            m_new = std::make_unique<NewDialog>(*this);
            m_new->signal_create().connect([this](const core::file::NewSpec& s) {
                adopt(core::file::make_new(s), "", "Untitled");
            });
        }
        m_new->open();
    });
}

// ── Open ────────────────────────────────────────────────────────────────────
void Shell::on_open() {
    if (file_job_busy()) return;
    ask_unsaved("opening another", [this] {
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
    });
}

void Shell::open_path(const std::string& path) {
    if (file_job_busy()) return;
    const std::string name = fs::path(path).filename().string();
    m_folder = fs::path(path).parent_path().string();
    // The command line comes straight here: unsaved changes still ask.
    ask_unsaved("opening “" + name + "”", [this, path, name] {
        m_file_job = start_job("Open " + name, [this, path, name](core::Jobs::Token& t) -> core::Jobs::Landing {
            std::string err;
            auto c = core::file::open(path, err, progress_of(t));
            if (!c) {
                if (t.cancelled()) return [] {};
                return [this, name, err] { toast("Couldn't open “" + name + "”: " + err); };
            }
            auto shared = std::make_shared<core::file::Contents>(std::move(*c));
            return [this, shared, path, name] {
                const bool native = shared->kind == formats::Kind::Klimt;
                adopt(std::move(*shared), native ? path : std::string{}, name);
            };
        });
    });
}

// ── Save ────────────────────────────────────────────────────────────────────
void Shell::on_save() {
    if (m_path.empty()) { on_save_as(); return; }
    if (file_job_busy()) return;
    save_to(m_path);
}

void Shell::on_save_as() {
    if (file_job_busy()) return;
    auto dlg = Gtk::FileDialog::create();
    dlg->set_title("Save As");
    auto filters = Gio::ListStore<Gtk::FileFilter>::create();
    filters->append(filter("Klimt document (.klimt)", {"klimt"}));
    dlg->set_filters(filters);
    dlg->set_initial_name(stem_of(m_name) + ".klimt");
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
    c.next_id = m_doc.next_id();
    const auto mark = m_journal.mark();
    const std::string name = fs::path(path).filename().string();
    m_folder = fs::path(path).parent_path().string();
    m_file_job = start_job("Save " + name, [this, c = std::move(c), path, name, mark, then](core::Jobs::Token& t)
                                               -> core::Jobs::Landing {
        std::string err;
        if (!core::file::save_klimt(path, c, err, progress_of(t))) {
            if (t.cancelled()) return [this] { toast("Save cancelled -- the file is as it was"); };
            return [this, name, err] { toast("Couldn't save “" + name + "”: " + err); };
        }
        return [this, path, name, mark, then] {
            m_path = path;
            m_name = name;
            m_saved_mark = mark;
            update_title();
            toast("Saved “" + name + "”");
            if (then) then();
        };
    });
}

// ── Export ──────────────────────────────────────────────────────────────────
void Shell::on_export() {
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
    dlg->set_initial_name(stem_of(m_name) + ".png");
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
