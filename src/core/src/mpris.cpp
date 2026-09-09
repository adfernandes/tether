#include "tether/mpris.hpp"

#include <tether/log.hpp>

#include <gio/gio.h>
#include <mutex>

namespace tether {

    MediaControl* g_media = nullptr;

    namespace {

        constexpr const char* MPRIS_PREFIX = "org.mpris.MediaPlayer2.";
        constexpr const char* MPRIS_PATH = "/org/mpris/MediaPlayer2";
        constexpr const char* PLAYER_IFACE = "org.mpris.MediaPlayer2.Player";

        constexpr int CALL_TIMEOUT_MS = 1000;

        std::string playback_status(GDBusConnection* bus, const std::string& name) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(bus,
                                                          name.c_str(),
                                                          MPRIS_PATH,
                                                          "org.freedesktop.DBus.Properties",
                                                          "Get",
                                                          g_variant_new("(ss)", PLAYER_IFACE, "PlaybackStatus"),
                                                          G_VARIANT_TYPE("(v)"),
                                                          G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                          CALL_TIMEOUT_MS,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                g_clear_error(&error);
                return {};
            }

            GVariant* boxed = nullptr;
            g_variant_get(reply, "(v)", &boxed);
            std::string status;
            if (boxed) {
                if (const char* text = g_variant_get_string(boxed, nullptr))
                    status = text;
                g_variant_unref(boxed);
            }
            g_variant_unref(reply);
            return status;
        }

        bool call_player(GDBusConnection* bus, const std::string& name, const char* method) {
            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(bus,
                                                          name.c_str(),
                                                          MPRIS_PATH,
                                                          PLAYER_IFACE,
                                                          method,
                                                          nullptr,
                                                          nullptr,
                                                          G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                                          CALL_TIMEOUT_MS,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                debug::log(DEBUG, "mpris: {} on {} failed: {}", method, name, error ? error->message : "unknown");
                g_clear_error(&error);
                return false;
            }
            g_variant_unref(reply);
            return true;
        }

    } // namespace

    struct MediaControl::Impl {
        mutable std::mutex mutex;
        std::vector<std::string> paused;

        // Resolved on first use: the daemon starts before the session bus is necessarily interesting.
        GDBusConnection* bus() const {
            static GDBusConnection* connection = [] {
                GError* error = nullptr;
                GDBusConnection* c = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
                if (!c) {
                    debug::log(DEBUG, "mpris: no session bus: {}", error ? error->message : "unknown");
                    g_clear_error(&error);
                }
                return c;
            }();
            return connection;
        }

        std::vector<std::string> players() const {
            GDBusConnection* connection = bus();
            if (!connection)
                return {};

            GError* error = nullptr;
            GVariant* reply = g_dbus_connection_call_sync(connection,
                                                          "org.freedesktop.DBus",
                                                          "/org/freedesktop/DBus",
                                                          "org.freedesktop.DBus",
                                                          "ListNames",
                                                          nullptr,
                                                          G_VARIANT_TYPE("(as)"),
                                                          G_DBUS_CALL_FLAGS_NONE,
                                                          CALL_TIMEOUT_MS,
                                                          nullptr,
                                                          &error);
            if (!reply) {
                debug::log(DEBUG, "mpris: ListNames failed: {}", error ? error->message : "unknown");
                g_clear_error(&error);
                return {};
            }

            std::vector<std::string> names;
            GVariantIter* iter = nullptr;
            const char* name = nullptr;
            g_variant_get(reply, "(as)", &iter);
            while (g_variant_iter_next(iter, "&s", &name)) {
                if (g_str_has_prefix(name, MPRIS_PREFIX))
                    names.emplace_back(name);
            }
            g_variant_iter_free(iter);
            g_variant_unref(reply);
            return names;
        }
    };

    MediaControl::MediaControl() : impl_(std::make_unique<Impl>()) {}
    MediaControl::~MediaControl() = default;

    std::vector<std::string> MediaControl::playing() const {
        GDBusConnection* bus = impl_->bus();
        if (!bus)
            return {};

        std::vector<std::string> result;
        for (const auto& name : impl_->players()) {
            if (playback_status(bus, name) == "Playing")
                result.push_back(name);
        }
        return result;
    }

    size_t MediaControl::pause() {
        GDBusConnection* bus = impl_->bus();
        if (!bus)
            return 0;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!impl_->paused.empty())
                return 0;
        }

        std::vector<std::string> held;
        for (const auto& name : playing()) {
            if (call_player(bus, name, "Pause"))
                held.push_back(name);
        }
        if (held.empty())
            return 0;

        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->paused = std::move(held);
        return impl_->paused.size();
    }

    size_t MediaControl::resume() {
        GDBusConnection* bus = impl_->bus();
        std::vector<std::string> held;
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            held.swap(impl_->paused);
        }
        if (!bus)
            return 0;

        size_t resumed = 0;
        for (const auto& name : held) {
            if (call_player(bus, name, "Play"))
                ++resumed;
        }
        return resumed;
    }

    bool MediaControl::holding() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return !impl_->paused.empty();
    }

    void MediaControl::forget() {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->paused.clear();
    }

} // namespace tether
