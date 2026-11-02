#pragma once
#include <gtkmm/application.h>
#include <memory>
#include <string>
#include <vector>

// App -- the Gtk::Application. Thin: stands up logging in create(), builds the
// Shell on activate. (CANON: thin bootstrap -- main -> App::create() -> Shell.)
namespace klimt {

class Shell;

class App : public Gtk::Application {
public:
    static Glib::RefPtr<App> create();

protected:
    App();
    void on_activate() override;
    void on_startup() override;   // accelerators, driven from core::shortcut_registry()
    // s007: `klimt photo.png` (and Files' Open With): open the first file.
    void on_open(const type_vec_files& files, const Glib::ustring& hint) override;

private:
    Shell* ensure_shell();        // the window, built once
    // s083: `klimt [--composix] FILE...` -- the files (and Composix's hint)
    // read here; a running Klimt is handed them over D-Bus (Open, with the
    // hint), else they wait for this one's activate.
    static gint on_local_options(GApplication* app, GVariantDict* options, gpointer self);
    std::vector<std::string> m_pending;   // files from this process's own command line
    std::string m_pending_hint;
    Shell* m_shell = nullptr;   // owned by the application via add_window
};

}  // namespace klimt
