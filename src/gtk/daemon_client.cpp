#include "daemon_client.hpp"
#include "tray.hpp"
#include "ui_util.hpp"
#include <tether/i18n.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gtk/gtk.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <tether/client.hpp>
#include <tether/core.hpp>
#include <tether/log.hpp>
#include <tether/net.hpp>
#include <unistd.h>

namespace tether::ui {

    namespace {

        int g_event_fd = -1;
        GIOChannel* g_event_channel = nullptr;
        guint g_event_watch_id = 0;
        guint g_event_retry_id = 0;
        DaemonEventFn g_on_event;
        DaemonDisconnectFn g_on_disconnect;

        gboolean start_event_subscription(gpointer);

        void stop_event_subscription() {
            if (g_event_watch_id != 0) {
                g_source_remove(g_event_watch_id);
                g_event_watch_id = 0;
            }
            if (g_event_channel) {
                g_io_channel_shutdown(g_event_channel, TRUE, nullptr);
                g_io_channel_unref(g_event_channel);
                g_event_channel = nullptr;
            } else if (g_event_fd >= 0) {
                close(g_event_fd);
            }
            g_event_fd = -1;
        }

        void schedule_event_retry() {
            if (g_event_retry_id == 0)
                g_event_retry_id = g_timeout_add_seconds(2, start_event_subscription, nullptr);
        }

        void handle_disconnect() {
            stop_event_subscription();
            schedule_event_retry();
            set_status_main(_("Daemon Offline"));
            set_route_status(Route::WiFi, false, _("The Tether daemon is not running."));
            set_route_status(Route::Bluetooth, false, _("The Tether daemon is not running."));
            if (g_on_disconnect)
                g_on_disconnect();
        }

        gboolean on_event_channel(GIOChannel* source, GIOCondition condition, gpointer) {
            if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
                handle_disconnect();
                return FALSE;
            }
            if (condition & G_IO_IN) {
                gchar* line = nullptr;
                gsize length = 0;
                GError* error = nullptr;
                while (true) {
                    GIOStatus status = g_io_channel_read_line(source, &line, &length, nullptr, &error);
                    if (status == G_IO_STATUS_AGAIN) {
                        if (line)
                            g_free(line);
                        break;
                    }
                    if (status == G_IO_STATUS_EOF || status == G_IO_STATUS_ERROR) {
                        if (error) {
                            debug::log(ERR, "Stream err: {}", error->message);
                            g_error_free(error);
                        }
                        if (line)
                            g_free(line);
                        handle_disconnect();
                        return FALSE;
                    }
                    if (line && length > 0) {
                        std::string_view payload(line, length);
                        while (!payload.empty() && (payload.back() == '\n' || payload.back() == '\r'))
                            payload.remove_suffix(1);

                        if (!payload.empty() && payload != "OK") {
                            try {
                                if (g_on_event)
                                    g_on_event(nlohmann::json::parse(payload));
                            } catch (const std::exception& e) {
                                debug::log(WARN, "daemon: discarding an unreadable event ({})", e.what());
                            }
                        }
                    }
                    if (line)
                        g_free(line);
                }
            }
            return TRUE;
        }

        gboolean start_event_subscription(gpointer) {
            if (g_event_retry_id != 0) {
                g_source_remove(g_event_retry_id);
                g_event_retry_id = 0;
            }
            stop_event_subscription();
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) {
                schedule_event_retry();
                return G_SOURCE_REMOVE;
            }

            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::string path = tether::get_runtime_dir() + "/tetherd.sock";
            std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                close(fd);
                static bool autostart_attempted = false;
                if (!autostart_attempted) {
                    autostart_attempted = true;
                    tether::spawn_daemon();
                }
                schedule_event_retry();
                set_status_main(_("Daemon Offline"));
                set_route_status(Route::WiFi, false, _("The Tether daemon is not running."));
                set_route_status(Route::Bluetooth, false, _("The Tether daemon is not running."));
                return G_SOURCE_REMOVE;
            }

            g_event_fd = fd;
            g_event_channel = g_io_channel_unix_new(fd);
            g_io_channel_set_encoding(g_event_channel, nullptr, nullptr);
            g_io_channel_set_buffered(g_event_channel, TRUE);
            g_io_channel_set_flags(g_event_channel, G_IO_FLAG_NONBLOCK, nullptr);
            g_event_watch_id = g_io_add_watch(g_event_channel,
                                              static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
                                              on_event_channel,
                                              nullptr);
            static const char kSubscribe[] = "{\"command\":\"subscribe\"}\n";
            if (write(fd, kSubscribe, sizeof(kSubscribe) - 1) < 0) {
                debug::log(ERR, "subscribe write error\n");
            }
            // The daemon answers subscribe with the current Bluetooth connection
            // status, but ask anyway: an older daemon will not, and a view whose
            // composer is gated on that status stays shut until it arrives.
            daemon_send({{"command", "bt_connection"}});
            daemon_send({{"command", "bt_status"}});
            daemon_send({{"command", "bt_list_devices"}});
            daemon_send({{"command", "bt_airpods"}});
            // primes the tray unread count
            daemon_send({{"command", "bt_list_threads"}});
            set_status_main(_("Daemon Online"));
            tray_refresh();
            return G_SOURCE_REMOVE;
        }

    } // namespace

    void daemon_client_start(DaemonEventFn on_event) {
        g_on_event = std::move(on_event);
        start_event_subscription(nullptr);
    }

    void daemon_client_stop() {
        stop_event_subscription();
        if (g_event_retry_id != 0) {
            g_source_remove(g_event_retry_id);
            g_event_retry_id = 0;
        }
    }

    bool daemon_send(const nlohmann::json& message) {
        if (g_event_fd < 0)
            return false;

        // The socket is non-blocking, and these are newline-delimited commands:
        // a short write leaves the daemon parsing a fragment and every later
        // command on this socket is misframed. Write it all or report failure.
        const std::string payload = message.dump() + "\n";
        size_t written = 0;
        int stalls = 0;
        while (written < payload.size()) {
            const ssize_t n = ::write(g_event_fd, payload.data() + written, payload.size() - written);
            if (n > 0) {
                written += static_cast<size_t>(n);
                stalls = 0;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                // Blocking the UI thread is the lesser evil against dropping a
                // command the user just asked for, but only briefly.
                if (++stalls > 200) {
                    debug::log(ERR, "daemon send stalled; dropped {} command", message.value("command", "?"));
                    return false;
                }
                g_usleep(1000);
                continue;
            }
            debug::log(ERR, "daemon write error: {}", std::strerror(errno));
            return false;
        }
        return true;
    }

    void daemon_client_on_disconnect(DaemonDisconnectFn on_disconnect) { g_on_disconnect = std::move(on_disconnect); }

    bool daemon_connected() { return g_event_fd >= 0; }

} // namespace tether::ui
