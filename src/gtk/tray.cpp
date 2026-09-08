#include "tray.hpp"

#include "daemon_client.hpp"
#include "prefs.hpp"
#include <tether/i18n.hpp>

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <nlohmann/json.hpp>
#include <tether/log.hpp>
#include <unistd.h>

namespace tether::ui {

    namespace {

        // StatusNotifierItem: the tray protocol, spoken directly over D-Bus.
        constexpr const char* SNI_INTERFACE = "org.kde.StatusNotifierItem";
        constexpr const char* SNI_PATH = "/StatusNotifierItem";
        constexpr const char* WATCHER_NAME = "org.kde.StatusNotifierWatcher";
        constexpr const char* WATCHER_PATH = "/StatusNotifierWatcher";

        constexpr const char* INTROSPECTION = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <signal name="NewIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
  </interface>
</node>
)XML";

        struct RouteState {
            bool ok = false;
            std::string detail;
        };

        GDBusConnection* g_bus = nullptr;
        GDBusNodeInfo* g_node = nullptr;
        std::string g_bus_name;
        guint g_own_id = 0;
        guint g_watch_id = 0;
        bool g_exported = false;

        RouteState g_state[2];
        std::string g_icon = "tether-offline";
        int g_unread = 0;
        std::string g_tooltip;
        std::string g_airpods_name;
        std::string g_airpods_battery;
        bool g_close_to_tray = false;
        std::string g_icon_style = "symbolic";
        std::string g_icon_suffix;

        RouteState& state(Route route) { return g_state[route == Route::WiFi ? 0 : 1]; }
        const char* route_name(Route route) { return route == Route::WiFi ? "Wi-Fi" : "Bluetooth"; }

        void load_prefs() {
            g_close_to_tray = prefs().value("close_to_tray", false);
            g_icon_style = prefs().value("tray_icon", std::string("symbolic"));
        }

        // Symbolic icons are drawn in the panel's foreground color, so the item matches the rest of
        // the tray. Falls back to the color set when the symbolic one is not installed.
        void resolve_icon_style() {
            if (g_icon_style == "color")
                return;
            if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), "tether-symbolic"))
                g_icon_suffix = "-symbolic";
        }

        void present_window() {
            if (GtkWidget* window = main_window())
                gtk_window_present(GTK_WINDOW(window));
        }

        // The only hide affordance on a desktop with no window buttons. Keyed on
        // visibility, not focus: clicking the icon takes focus off the window.
        void toggle_window() {
            GtkWidget* window = main_window();
            if (!window)
                return;
            if (gtk_widget_get_visible(window))
                gtk_widget_hide(window);
            else
                gtk_window_present(GTK_WINDOW(window));
        }

        void emit(const char* signal) {
            if (!g_exported)
                return;
            g_dbus_connection_emit_signal(g_bus, nullptr, SNI_PATH, SNI_INTERFACE, signal, nullptr, nullptr);
        }

        GVariant* build_tooltip() {
            GVariantBuilder pixmaps;
            g_variant_builder_init(&pixmaps, G_VARIANT_TYPE("a(iiay)"));
            return g_variant_new("(sa(iiay)ss)", "", &pixmaps, "Tether", g_tooltip.c_str());
        }

        GVariant* get_property(
            GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property, GError**, gpointer) {
            const std::string name = property;
            if (name == "Category")
                return g_variant_new_string("ApplicationStatus");
            if (name == "Id")
                return g_variant_new_string("tether");
            if (name == "Title")
                return g_variant_new_string("Tether");
            if (name == "Status")
                return g_variant_new_string("Active");
            if (name == "IconName")
                return g_variant_new_string(g_icon.c_str());
            if (name == "ItemIsMenu")
                return g_variant_new_boolean(FALSE);
            if (name == "Menu")
                return g_variant_new_object_path("/NO_DBUSMENU");
            if (name == "ToolTip")
                return build_tooltip();
            return nullptr;
        }

        void on_method(GDBusConnection*,
                       const gchar*,
                       const gchar*,
                       const gchar*,
                       const gchar* method,
                       GVariant*,
                       GDBusMethodInvocation* invocation,
                       gpointer) {
            const std::string name = method;
            if (name == "Activate" || name == "SecondaryActivate")
                toggle_window();
            else if (name == "ContextMenu")
                present_window();
            g_dbus_method_invocation_return_value(invocation, nullptr);
        }

        constexpr GDBusInterfaceVTable VTABLE = {on_method, get_property, nullptr, {nullptr}};

        void register_with_watcher() {
            if (!g_exported)
                return;
            g_dbus_connection_call(g_bus,
                                   WATCHER_NAME,
                                   WATCHER_PATH,
                                   WATCHER_NAME,
                                   "RegisterStatusNotifierItem",
                                   g_variant_new("(s)", g_bus_name.c_str()),
                                   nullptr,
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1,
                                   nullptr,
                                   nullptr,
                                   nullptr);
        }

        void on_name_acquired(GDBusConnection* connection, const gchar*, gpointer) {
            if (g_exported)
                return;
            GError* error = nullptr;
            if (g_dbus_connection_register_object(
                    connection, SNI_PATH, g_node->interfaces[0], &VTABLE, nullptr, nullptr, &error) == 0) {
                debug::log(WARN, "tray: could not export the item ({})", error ? error->message : "unknown");
                g_clear_error(&error);
                return;
            }
            g_exported = true;
            register_with_watcher();
        }

    } // namespace

    void tray_init() {
        if (g_own_id != 0)
            return;

        load_prefs();
        resolve_icon_style();
        g_icon += g_icon_suffix;

        GError* error = nullptr;
        g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!g_bus) {
            debug::log(WARN, "tray: no session bus ({})", error ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }

        g_node = g_dbus_node_info_new_for_xml(INTROSPECTION, &error);
        if (!g_node) {
            debug::log(ERR, "tray: bad introspection data ({})", error ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }

        // Keyed on the process, per spec, so two instances cannot collide.
        g_bus_name = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
        g_own_id = g_bus_own_name_on_connection(
            g_bus, g_bus_name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, on_name_acquired, nullptr, nullptr, nullptr);

        // Re-registering on reappearance is what survives a panel restart.
        g_watch_id = g_bus_watch_name_on_connection(
            g_bus,
            WATCHER_NAME,
            G_BUS_NAME_WATCHER_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, const gchar*, gpointer) { register_with_watcher(); },
            nullptr,
            nullptr,
            nullptr);

        tray_refresh();
    }

    void tray_set_route(Route route, bool ok, const std::string& detail) {
        state(route) = {ok, detail};
        tray_refresh();
    }

    void tray_refresh() {
        std::string tooltip;
        for (Route route : {Route::WiFi, Route::Bluetooth}) {
            const RouteState& r = state(route);
            const std::string status = r.ok ? _("connected") : r.detail.empty() ? _("not connected") : r.detail;
            // TRANSLATORS: {} is a transport name, "Wi-Fi" or "Bluetooth", or a device name.
            tooltip += tether::tr_format(_("{}: {}"), route_name(route), status) + "\n";
        }
        if (!g_airpods_battery.empty())
            tooltip += tether::tr_format(_("{}: {}"), g_airpods_name, g_airpods_battery) + "\n";
        if (!daemon_connected())
            tooltip += std::string(_("Daemon: not running")) + "\n";
        if (g_unread > 0)
            tooltip += tether::tr_format(P_("{} unread message", "{} unread messages", g_unread), g_unread) + "\n";
        if (!tooltip.empty())
            tooltip.pop_back();

        const bool any_up = g_state[0].ok || g_state[1].ok;
        const std::string icon =
            std::string(!any_up ? "tether-offline" : (g_unread > 0 ? "tether-unread" : "tether")) + g_icon_suffix;

        if (icon != g_icon) {
            g_icon = icon;
            emit("NewIcon");
        }
        if (tooltip != g_tooltip) {
            g_tooltip = tooltip;
            emit("NewToolTip");
        }
    }

    void tray_set_airpods(const std::string& name, const std::string& battery) {
        if (name == g_airpods_name && battery == g_airpods_battery)
            return;
        g_airpods_name = name;
        g_airpods_battery = battery;
        tray_refresh();
    }

    void tray_set_unread(int count) {
        if (count == g_unread)
            return;
        g_unread = count;
        tray_refresh();
    }

    bool tray_close_to_tray() { return g_close_to_tray; }

    void tray_set_close_to_tray(bool enabled) {
        if (g_close_to_tray == enabled)
            return;
        g_close_to_tray = enabled;
        prefs()["close_to_tray"] = enabled;
        prefs_save();
    }

} // namespace tether::ui
