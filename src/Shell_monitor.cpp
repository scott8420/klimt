// Shell_monitor.cpp -- THE MONITOR'S PROFILE (s071; WEAKNESSES 14;
// docs/colour.md). GTK 4 has no API for it; colord (the system bus,
// org.freedesktop.ColorManager) knows each display's profile -- what GNOME
// Settings > Colour assigns. Read on a worker (D-Bus calls block), for the
// monitor the window is on, again when it moves to another; the profile
// lands in tint::set_screen and every tile is drawn again. No colord, no
// profile, or an sRGB one: the screen stays sRGB, as before.
#include "Shell.hpp"
#include "Log.hpp"

#include <gdkmm/monitor.h>
#include <gio/gio.h>

#include <fstream>
#include <iterator>

namespace klimt {

namespace {
// One D-Bus property (a variant), or nullptr.
GVariant* property(GDBusConnection* bus, const char* path, const char* iface, const char* name) {
    GError* err = nullptr;
    GVariant* r = g_dbus_connection_call_sync(bus, "org.freedesktop.ColorManager", path, "org.freedesktop.DBus.Properties", "Get",
                                              g_variant_new("(ss)", iface, name), G_VARIANT_TYPE("(v)"),
                                              G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &err);
    if (!r) { if (err) g_error_free(err); return nullptr; }
    GVariant* v = nullptr;
    g_variant_get(r, "(v)", &v);
    g_variant_unref(r);
    return v;
}

// colord's display devices: connector, primary, profile files.
std::vector<core::profile::MonitorDevice> colord_displays(std::string& why) {
    std::vector<core::profile::MonitorDevice> out;
    GError* err = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
    if (!bus) { why = err ? err->message : "no system bus"; if (err) g_error_free(err); return out; }
    GVariant* r = g_dbus_connection_call_sync(bus, "org.freedesktop.ColorManager", "/org/freedesktop/ColorManager",
                                              "org.freedesktop.ColorManager", "GetDevicesByKind", g_variant_new("(s)", "display"),
                                              G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &err);
    if (!r) { why = err ? err->message : "colord didn't answer"; if (err) g_error_free(err); g_object_unref(bus); return out; }
    GVariantIter* it = nullptr;
    g_variant_get(r, "(ao)", &it);
    const gchar* dev = nullptr;
    while (g_variant_iter_loop(it, "&o", &dev)) {
        core::profile::MonitorDevice d;
        if (GVariant* md = property(bus, dev, "org.freedesktop.ColorManager.Device", "Metadata")) {
            GVariantIter mi;
            g_variant_iter_init(&mi, md);
            const gchar *k = nullptr, *v = nullptr;
            while (g_variant_iter_next(&mi, "{&s&s}", &k, &v)) {
                if (g_strcmp0(k, "XRANDR_name") == 0) d.connector = v;
                if (g_strcmp0(k, "OutputPriority") == 0) d.primary = g_strcmp0(v, "primary") == 0;
            }
            g_variant_unref(md);
        }
        if (GVariant* ps = property(bus, dev, "org.freedesktop.ColorManager.Device", "Profiles")) {
            GVariantIter pi;
            g_variant_iter_init(&pi, ps);
            const gchar* prof = nullptr;
            while (g_variant_iter_next(&pi, "&o", &prof))
                if (GVariant* f = property(bus, prof, "org.freedesktop.ColorManager.Profile", "Filename")) {
                    const gchar* file = g_variant_get_string(f, nullptr);
                    if (file && *file) d.profiles.emplace_back(file);
                    g_variant_unref(f);
                }
            g_variant_unref(ps);
        }
        out.push_back(std::move(d));
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
    g_object_unref(bus);
    return out;
}
}  // namespace

std::string Shell::monitor_connector() {
    auto surf = get_surface();
    auto disp = get_display();
    if (!surf || !disp) return {};
    auto mon = disp->get_monitor_at_surface(surf);
    return mon ? std::string(mon->get_connector()) : std::string{};
}

// The window's monitor: its profile from colord, on a worker.
void Shell::read_monitor_profile() {
    const std::string conn = monitor_connector();
    if (m_monitor_asked && conn == m_monitor_connector) return;
    m_monitor_asked = true;
    m_monitor_connector = conn;
    m_jobs.submit("Monitor profile", [this, conn](core::Jobs::Token&) -> core::Jobs::Landing {
        std::string why;
        const auto devices = colord_displays(why);
        const std::string file = core::profile::monitor_profile(devices, conn);
        std::shared_ptr<const std::vector<std::uint8_t>> icc;
        if (!file.empty()) {
            std::ifstream f(file, std::ios::binary);
            if (f) icc = std::make_shared<const std::vector<std::uint8_t>>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        return [this, icc, file, conn, why, n = devices.size()] {
            tint::set_screen(icc);
            const auto used = tint::screen();
            m_monitor_name = used ? tint::inspect(*used).name : file.empty() ? "sRGB (no profile assigned)" : "sRGB (" + file + " is sRGB or unusable)";
            if (auto lg = log::get(log::Area::Shell))
                lg->info("monitor {}: {} display(s) in colord{}; profile: {}", conn.empty() ? "?" : conn, n,
                         why.empty() ? "" : " (" + why + ")", m_monitor_name);
            m_dirty = {};   // every tile again, to this screen
            m_shown = {};
            if (has_image()) queue_render();
        };
    });
}

}  // namespace klimt
