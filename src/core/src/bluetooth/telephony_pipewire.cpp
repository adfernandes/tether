#include "tether/bluetooth/telephony.hpp"
#include "tether/log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>

// PipeWire's hands-free implementation, on the session bus. It owns the same
// profile BlueZ's does and carries the call audio as well, so a phone that
// connected Hands-Free to PipeWire is reachable only here.
namespace tether::bluetooth {

    namespace {

        constexpr const char* PW_NAME = "org.pipewire.Telephony";
        constexpr const char* PW_ROOT = "/org/pipewire/Telephony";
        constexpr const char* PW_GATEWAY = "org.pipewire.Telephony.AudioGateway1";
        constexpr const char* PW_TRANSPORT = "org.pipewire.Telephony.AudioGatewayTransport1";
        constexpr const char* PW_CALL = "org.pipewire.Telephony.Call1";
        constexpr const char* IFACE_OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager";

        constexpr int CALL_TIMEOUT_MS = 5000;
        // How long to leave the session bus alone after finding no service there.
        constexpr int DORMANT_SECONDS = 30;

        bool iequals(const std::string& a, const std::string& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                       return std::tolower(x) == std::tolower(y);
                   });
        }

        // /org/pipewire/Telephony/ag0/call1 -> the owning gateway.
        std::string parent_path(const std::string& path) {
            const size_t slash = path.rfind('/');
            return slash == std::string::npos || slash == 0 ? std::string{} : path.substr(0, slash);
        }

        std::string string_prop(GVariant* props, const char* key) {
            GVariant* value = g_variant_lookup_value(props, key, G_VARIANT_TYPE_STRING);
            if (!value)
                return {};
            std::string out = g_variant_get_string(value, nullptr);
            g_variant_unref(value);
            return out;
        }

        bool bool_prop(GVariant* props, const char* key) {
            GVariant* value = g_variant_lookup_value(props, key, G_VARIANT_TYPE_BOOLEAN);
            if (!value)
                return false;
            const bool out = g_variant_get_boolean(value);
            g_variant_unref(value);
            return out;
        }

        // The interface dictionary for one object, or null when absent.
        GVariant* interface_props(GVariant* interfaces, const char* iface) {
            return g_variant_lookup_value(interfaces, iface, G_VARIANT_TYPE_VARDICT);
        }

        Call read_call(const std::string& path, GVariant* props) {
            Call call;
            call.path = path;
            call.telephony_path = parent_path(path);
            call.number = string_prop(props, "LineIdentification");
            call.name = string_prop(props, "Name");
            call.incoming_line = string_prop(props, "IncomingLine");
            // Passed through unmapped, as the BlueZ side does.
            call.state = string_prop(props, "State");
            call.multiparty = bool_prop(props, "Multiparty");
            return call;
        }

        class PipewireSource : public TelephonySource {
        public:
            explicit PipewireSource(std::string address) : address_(std::move(address)) {}

            ~PipewireSource() override {
                if (conn_)
                    g_object_unref(conn_);
            }

            TelephonyIds ids() const override {
                return {"pipewire", PW_NAME, PW_GATEWAY, PW_TRANSPORT, PW_CALL, false, false};
            }

            GDBusConnection* connection() const override {
                std::lock_guard<std::mutex> lock(mutex_);
                return ensure_locked();
            }

            TelephonySnapshot snapshot() const override {
                std::lock_guard<std::mutex> lock(mutex_);
                if (std::chrono::steady_clock::now() < next_try_)
                    return {};

                GVariant* objects = managed_objects_locked(PW_ROOT);
                if (!objects)
                    return {};

                TelephonySnapshot snap = parse_pipewire_telephony(objects, address_);
                g_variant_unref(objects);

                // Measured on pipewire 1.6.8: root manager has only the gateways, even during a call, and each gateway
                // keeps its calls in an object manager of its own.
                if (!snap.gateway.path.empty()) {
                    if (GVariant* nested = managed_objects_locked(snap.gateway.path.c_str())) {
                        snap.calls = parse_pipewire_calls(nested, snap.gateway.path);
                        g_variant_unref(nested);
                    }
                }
                return snap;
            }

        private:
            GDBusConnection* ensure_locked() const {
                if (conn_)
                    return conn_;
                GError* error = nullptr;
                conn_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
                if (!conn_) {
                    debug::log(DEBUG, "telephony: session bus unavailable ({})", error ? error->message : "unknown");
                    g_clear_error(&error);
                    next_try_ = std::chrono::steady_clock::now() + std::chrono::seconds(DORMANT_SECONDS);
                }
                return conn_;
            }

            GVariant* managed_objects_locked(const char* path) const {
                GDBusConnection* conn = ensure_locked();
                if (!conn)
                    return nullptr;

                GError* error = nullptr;
                GVariant* reply = g_dbus_connection_call_sync(conn,
                                                              PW_NAME,
                                                              path,
                                                              IFACE_OBJECT_MANAGER,
                                                              "GetManagedObjects",
                                                              nullptr,
                                                              G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                              G_DBUS_CALL_FLAGS_NONE,
                                                              CALL_TIMEOUT_MS,
                                                              nullptr,
                                                              &error);
                if (!reply) {
                    // Nothing is publishing the service, which is the normal
                    // state on a machine where BlueZ owns hands-free.
                    if (error && error->domain == G_DBUS_ERROR &&
                        (error->code == G_DBUS_ERROR_SERVICE_UNKNOWN || error->code == G_DBUS_ERROR_NAME_HAS_NO_OWNER))
                        next_try_ = std::chrono::steady_clock::now() + std::chrono::seconds(DORMANT_SECONDS);
                    g_clear_error(&error);
                    return nullptr;
                }

                next_try_ = std::chrono::steady_clock::time_point{};
                GVariant* objects = g_variant_get_child_value(reply, 0);
                g_variant_unref(reply);
                return objects;
            }

            std::string address_;
            mutable std::mutex mutex_;
            mutable GDBusConnection* conn_ = nullptr;
            mutable std::chrono::steady_clock::time_point next_try_{};
        };

    } // namespace

    TelephonySnapshot parse_pipewire_telephony(GVariant* objects, const std::string& address) {
        TelephonySnapshot snap;
        if (!objects)
            return snap;

        GVariantIter iter;
        const char* path = nullptr;
        GVariant* interfaces = nullptr;

        g_variant_iter_init(&iter, objects);
        while (g_variant_iter_loop(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
            GVariant* props = interface_props(interfaces, PW_GATEWAY);
            if (!props)
                continue;
            const std::string gateway_address = string_prop(props, "Address");
            g_variant_unref(props);
            if (!iequals(gateway_address, address))
                continue;

            snap.gateway.path = path;
            snap.gateway.device_path = parent_path(path);
            snap.gateway.state = "connected";
            if (GVariant* transport = interface_props(interfaces, PW_TRANSPORT)) {
                snap.audio_state = string_prop(transport, "State");
                g_variant_unref(transport);
            }
            g_variant_unref(interfaces);
            break;
        }

        if (!snap.gateway.path.empty())
            snap.calls = parse_pipewire_calls(objects, snap.gateway.path);
        return snap;
    }

    std::vector<Call> parse_pipewire_calls(GVariant* objects, const std::string& gateway_path) {
        std::vector<Call> calls;
        if (!objects || gateway_path.empty())
            return calls;

        GVariantIter iter;
        const char* path = nullptr;
        GVariant* interfaces = nullptr;

        g_variant_iter_init(&iter, objects);
        while (g_variant_iter_loop(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
            GVariant* props = interface_props(interfaces, PW_CALL);
            if (!props)
                continue;
            const std::string call_path = path;
            // Scoped by parentage, so a second phone's calls stay its own.
            if (parent_path(call_path) == gateway_path)
                calls.push_back(read_call(call_path, props));
            g_variant_unref(props);
        }

        std::sort(calls.begin(), calls.end(), [](const Call& a, const Call& b) { return a.path < b.path; });
        return calls;
    }

    std::unique_ptr<TelephonySource> make_pipewire_source(std::string address) {
        return std::make_unique<PipewireSource>(std::move(address));
    }

} // namespace tether::bluetooth
