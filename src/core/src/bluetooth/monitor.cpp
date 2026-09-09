#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gio/gio.h>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace tether::bluetooth {

    BluezMonitor* g_bluez = nullptr;

    namespace {
        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* OBJECT_MANAGER = "org.freedesktop.DBus.ObjectManager";

        // BlueZ emits PropertiesChanged continuously during discovery (RSSI churn
        // above all). Coalesce bursts into one refresh instead of re-reading the
        // whole object tree per signal.
        constexpr guint REFRESH_DEBOUNCE_MS = 250;
    } // namespace

    struct MonitorState {
        std::thread thread;
        GMainContext* context = nullptr;
        GMainLoop* loop = nullptr;
        GDBusConnection* conn = nullptr;
        std::atomic<bool> running{false};

        int efd = -1;
        guint pending_refresh = 0;
        std::vector<guint> subscriptions;

        mutable std::mutex mutex;
        BluezObjects objects;
        Capability cap;
        std::map<std::string, std::string> disconnect_reasons;
        std::string preferred_adapter_id;
        std::string secure_adapter;
        std::optional<bool> secure_connections;
        std::chrono::steady_clock::time_point secure_checked_at{};

        void run();
        void refresh();
        void schedule_refresh();
        void notify();
        std::optional<bool> read_secure_connections(const std::string& adapter_id);
    };

    namespace {

        gboolean on_refresh_timeout(gpointer user_data) {
            auto* impl = static_cast<MonitorState*>(user_data);
            impl->pending_refresh = 0;
            impl->refresh();
            return G_SOURCE_REMOVE;
        }

        void on_bluez_signal(
            GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer user_data) {
            static_cast<MonitorState*>(user_data)->schedule_refresh();
        }

        void on_disconnected(GDBusConnection*,
                             const gchar*,
                             const gchar* object_path,
                             const gchar* interface_name,
                             const gchar*,
                             GVariant* parameters,
                             gpointer user_data) {
            const gchar* reason = nullptr;
            const gchar* message = nullptr;
            g_variant_get(parameters, "(&s&s)", &reason, &message);
            if (!reason || !object_path)
                return;

            auto* impl = static_cast<MonitorState*>(user_data);
            {
                std::lock_guard<std::mutex> lock(impl->mutex);
                impl->disconnect_reasons[object_path] = reason;
            }
            debug::log(INFO,
                       "bluetooth: {} disconnected from {}: {} ({})",
                       interface_name ? interface_name : "?",
                       object_path,
                       reason,
                       message ? message : "");
            impl->schedule_refresh();
        }

        // on watcher thread once it iterates its context
        gboolean quit_loop(gpointer user_data) {
            auto* state = static_cast<MonitorState*>(user_data);
            if (state->loop)
                g_main_loop_quit(state->loop);
            return G_SOURCE_REMOVE;
        }

    } // namespace

    void MonitorState::notify() {
        if (efd < 0)
            return;
        uint64_t one = 1;
        if (write(efd, &one, sizeof(one)) < 0)
            debug::log(WARN, "bluetooth: failed to signal event fd");
    }

    void MonitorState::schedule_refresh() {
        if (pending_refresh != 0)
            return;
        GSource* source = g_timeout_source_new(REFRESH_DEBOUNCE_MS);
        g_source_set_callback(source, on_refresh_timeout, this, nullptr);
        pending_refresh = g_source_attach(source, context);
        g_source_unref(source);
    }

    // bluetoothd exposes no property for its own flags, so the argument vector is
    // the only place to read it. Accepts both spellings; Arch's unit uses -E.
    static bool scan_bluetoothd_cmdline() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
            std::ifstream comm(entry.path() / "comm");
            std::string name;
            if (!comm || !std::getline(comm, name) || name != "bluetoothd")
                continue;

            std::ifstream cmdline(entry.path() / "cmdline", std::ios::binary);
            if (!cmdline)
                continue;
            // Arguments are NUL-separated, so each one compares exactly.
            std::string arg;
            while (std::getline(cmdline, arg, '\0')) {
                if (arg == "-E" || arg == "--experimental")
                    return true;
            }
            return false;
        }
        return false;
    }

    // Walking /proc costs about 2ms and refresh() runs on every debounced BlueZ
    // signal, so the answer is cached. It cannot change without bluetoothd
    // restarting, but that is exactly what applying the drop-in does, and
    // bluetoothd routinely restarts after tetherd is already up.
    bool bluetoothd_has_experimental() {
        static constexpr auto kRecheckAfter = std::chrono::seconds(3);
        static std::chrono::steady_clock::time_point checked_at{};
        static bool value = false;

        const auto now = std::chrono::steady_clock::now();
        if (checked_at == std::chrono::steady_clock::time_point{} || now - checked_at >= kRecheckAfter) {
            value = scan_bluetoothd_cmdline();
            checked_at = now;
        }
        return value;
    }

    std::optional<bool> probe_secure_connections(const std::string& adapter_id, std::chrono::milliseconds timeout) {
        if (adapter_id.empty())
            return std::nullopt;

        std::array<gchar*, 5> argv = {
            const_cast<gchar*>("btmgmt"),
            const_cast<gchar*>("--index"),
            const_cast<gchar*>(adapter_id.c_str()),
            const_cast<gchar*>("info"),
            nullptr,
        };

        GPid child = 0;
        gint input_fd = -1;
        gint output_fd = -1;
        GError* error = nullptr;
        const auto flags = static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                                    G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_CLOEXEC_PIPES);
        // btmgmt epolls its stdin before it runs the command, rejects /dev/null with EPERM.
        if (!g_spawn_async_with_pipes(nullptr,
                                      argv.data(),
                                      nullptr,
                                      flags,
                                      nullptr,
                                      nullptr,
                                      &child,
                                      &input_fd,
                                      &output_fd,
                                      nullptr,
                                      &error)) {
            debug::log(WARN,
                       "bluetooth: cannot start btmgmt for {}: {}",
                       adapter_id,
                       error ? error->message : "unknown error");
            g_clear_error(&error);
            return std::nullopt;
        }
        if (input_fd >= 0)
            close(input_fd);

        const int old_flags = fcntl(output_fd, F_GETFL, 0);
        if (old_flags >= 0)
            fcntl(output_fd, F_SETFL, old_flags | O_NONBLOCK);

        std::string output;
        output.reserve(2048);
        int status = 0;
        bool exited = false;
        const auto started = std::chrono::steady_clock::now();
        const auto deadline = started + std::max(timeout, std::chrono::milliseconds::zero());

        auto drain_output = [&] {
            char buffer[1024];
            for (;;) {
                const ssize_t count = read(output_fd, buffer, sizeof(buffer));
                if (count > 0) {
                    if (output.size() < 64 * 1024)
                        output.append(buffer, std::min<size_t>(static_cast<size_t>(count), 64 * 1024 - output.size()));
                    continue;
                }
                if (count < 0 && errno == EINTR)
                    continue;
                break;
            }
        };

        while (std::chrono::steady_clock::now() < deadline) {
            drain_output();
            const pid_t waited = waitpid(child, &status, WNOHANG);
            if (waited == child) {
                exited = true;
                break;
            }
            if (waited < 0 && errno != EINTR)
                break;

            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            pollfd watched{output_fd, POLLIN | POLLHUP, 0};
            poll(&watched, 1, static_cast<int>(std::clamp<int64_t>(remaining.count(), 1, 50)));
        }

        if (!exited) {
            kill(child, SIGTERM);
            const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (std::chrono::steady_clock::now() < grace_deadline) {
                const pid_t waited = waitpid(child, &status, WNOHANG);
                if (waited == child) {
                    exited = true;
                    break;
                }
                if (waited < 0 && errno != EINTR)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (!exited) {
                kill(child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
                }
            }

            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            debug::log(WARN, "bluetooth: btmgmt info for {} timed out after {}ms", adapter_id, elapsed.count());
            close(output_fd);
            g_spawn_close_pid(child);
            return std::nullopt;
        }

        drain_output();
        close(output_fd);
        g_spawn_close_pid(child);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return std::nullopt;

        const size_t settings = output.find("current settings:");
        if (settings == std::string::npos)
            return std::nullopt;
        const size_t line_end = output.find('\n', settings);
        const std::string line = output.substr(settings, line_end - settings);
        return line.find("secure-conn") != std::string::npos;
    }

    std::optional<bool> MonitorState::read_secure_connections(const std::string& adapter_id) {
        constexpr auto RECHECK_AFTER = std::chrono::seconds(30);
        if (adapter_id.empty()) {
            secure_adapter.clear();
            secure_connections.reset();
            secure_checked_at = {};
            return std::nullopt;
        }

        const auto now = std::chrono::steady_clock::now();
        if (adapter_id != secure_adapter || secure_checked_at == std::chrono::steady_clock::time_point{} ||
            now - secure_checked_at >= RECHECK_AFTER) {
            secure_adapter = adapter_id;
            secure_connections = probe_secure_connections(adapter_id);
            secure_checked_at = std::chrono::steady_clock::now();
        }
        return secure_connections;
    }

    void MonitorState::refresh() {
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn,
                                                      BLUEZ_NAME,
                                                      "/",
                                                      OBJECT_MANAGER,
                                                      "GetManagedObjects",
                                                      nullptr,
                                                      G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      5000,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            debug::log(WARN, "bluetooth: GetManagedObjects failed: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return;
        }

        BluezObjects parsed = parse_managed_objects(reply);
        g_variant_unref(reply);
        parsed.experimental_api = bluetoothd_has_experimental();
        std::string preferred;
        {
            std::lock_guard<std::mutex> lock(mutex);
            preferred = preferred_adapter_id;
        }
        Capability resolved = resolve_capability(parsed, std::nullopt, preferred);
        resolved = resolve_capability(parsed, read_secure_connections(resolved.adapter_id), preferred);

        {
            std::lock_guard<std::mutex> lock(mutex);
            if (parsed == objects && resolved == cap)
                return;
            objects = std::move(parsed);
            cap = std::move(resolved);
        }
        notify();
    }

    void MonitorState::run() {
        g_main_context_push_thread_default(context);

        for (const char* signal : {"InterfacesAdded", "InterfacesRemoved"}) {
            subscriptions.push_back(g_dbus_connection_signal_subscribe(conn,
                                                                       BLUEZ_NAME,
                                                                       OBJECT_MANAGER,
                                                                       signal,
                                                                       "/",
                                                                       nullptr,
                                                                       G_DBUS_SIGNAL_FLAGS_NONE,
                                                                       on_bluez_signal,
                                                                       this,
                                                                       nullptr));
        }
        subscriptions.push_back(g_dbus_connection_signal_subscribe(conn,
                                                                   BLUEZ_NAME,
                                                                   "org.freedesktop.DBus.Properties",
                                                                   "PropertiesChanged",
                                                                   nullptr,
                                                                   nullptr,
                                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                                   on_bluez_signal,
                                                                   this,
                                                                   nullptr));
        subscriptions.push_back(g_dbus_connection_signal_subscribe(conn,
                                                                   BLUEZ_NAME,
                                                                   nullptr,
                                                                   "Disconnected",
                                                                   nullptr,
                                                                   nullptr,
                                                                   G_DBUS_SIGNAL_FLAGS_NONE,
                                                                   on_disconnected,
                                                                   this,
                                                                   nullptr));

        refresh();

        g_main_loop_run(loop);
        running = false;

        for (guint id : subscriptions)
            g_dbus_connection_signal_unsubscribe(conn, id);
        subscriptions.clear();

        if (pending_refresh != 0) {
            if (GSource* s = g_main_context_find_source_by_id(context, pending_refresh))
                g_source_destroy(s);
            pending_refresh = 0;
        }

        g_main_context_pop_thread_default(context);
    }

    BluezMonitor::BluezMonitor() : impl_(std::make_unique<MonitorState>()) {}

    BluezMonitor::~BluezMonitor() { stop(); }

    bool BluezMonitor::start() {
        if (impl_->running)
            return true;

        GError* error = nullptr;
        impl_->conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
        if (!impl_->conn) {
            debug::log(WARN, "bluetooth: cannot reach the system bus: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            return false;
        }

        // fail fast when bluetoothd is not present
        GVariant* probe = g_dbus_connection_call_sync(impl_->conn,
                                                      BLUEZ_NAME,
                                                      "/",
                                                      OBJECT_MANAGER,
                                                      "GetManagedObjects",
                                                      nullptr,
                                                      G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      5000,
                                                      nullptr,
                                                      &error);
        if (!probe) {
            debug::log(WARN, "bluetooth: BlueZ unavailable: {}", error ? error->message : "unknown");
            g_clear_error(&error);
            g_clear_object(&impl_->conn);
            return false;
        }
        impl_->objects = parse_managed_objects(probe);
        impl_->objects.experimental_api = bluetoothd_has_experimental();
        // Secure Connections stays unknown until the watcher thread's first
        // refresh: probing it here would fork btmgmt on the startup path.
        impl_->cap = resolve_capability(impl_->objects);
        g_variant_unref(probe);

        impl_->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (impl_->efd < 0) {
            debug::log(ERR, "bluetooth: eventfd() failed");
            g_clear_object(&impl_->conn);
            return false;
        }

        impl_->context = g_main_context_new();
        // Created here, not on the watcher thread: a stop() that lands before the
        // thread reaches g_main_loop_new would otherwise quit nothing, and the
        // join() below it would block forever.
        impl_->loop = g_main_loop_new(impl_->context, FALSE);
        impl_->running = true;
        impl_->thread = std::thread([this] { impl_->run(); });
        return true;
    }

    void BluezMonitor::stop() {
        if (impl_->thread.joinable()) {
            g_main_context_invoke(impl_->context, quit_loop, impl_.get());
            impl_->thread.join();
        }
        if (impl_->loop) {
            g_main_loop_unref(impl_->loop);
            impl_->loop = nullptr;
        }
        if (impl_->context) {
            g_main_context_unref(impl_->context);
            impl_->context = nullptr;
        }
        g_clear_object(&impl_->conn);
        if (impl_->efd >= 0) {
            close(impl_->efd);
            impl_->efd = -1;
        }
    }

    bool BluezMonitor::running() const { return impl_->running; }

    int BluezMonitor::event_fd() const { return impl_->efd; }

    void BluezMonitor::drain() {
        if (impl_->efd < 0)
            return;
        uint64_t counter = 0;
        while (read(impl_->efd, &counter, sizeof(counter)) > 0) {
            // Drain every pending wakeup; the snapshot is read separately.
        }
    }

    GDBusConnection* BluezMonitor::connection() const { return impl_->conn; }

    void BluezMonitor::invoke_sync(const std::function<void()>& fn) {
        if (!impl_->context) {
            fn();
            return;
        }

        struct Job {
            const std::function<void()>* fn;
            std::mutex mutex;
            std::condition_variable done;
            bool finished = false;
        } job{&fn, {}, {}, false};

        g_main_context_invoke_full(
            impl_->context,
            G_PRIORITY_DEFAULT,
            [](gpointer data) -> gboolean {
                auto* j = static_cast<Job*>(data);
                (*j->fn)();
                {
                    std::lock_guard<std::mutex> lock(j->mutex);
                    j->finished = true;
                }
                j->done.notify_all();
                return G_SOURCE_REMOVE;
            },
            &job,
            nullptr);

        std::unique_lock<std::mutex> lock(job.mutex);
        job.done.wait(lock, [&job] { return job.finished; });
    }

    BluezObjects BluezMonitor::snapshot() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->objects;
    }

    Capability BluezMonitor::capability() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->cap;
    }

    std::string BluezMonitor::last_disconnect_reason(const std::string& address) const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& device : impl_->objects.devices) {
            if (device.address != address)
                continue;
            auto it = impl_->disconnect_reasons.find(device.path);
            return it == impl_->disconnect_reasons.end() ? std::string{} : it->second;
        }
        return {};
    }

    void BluezMonitor::clear_disconnect_reason(const std::string& address) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        for (const auto& device : impl_->objects.devices) {
            if (device.address == address)
                impl_->disconnect_reasons.erase(device.path);
        }
    }

    void BluezMonitor::set_preferred_adapter(std::string id) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->preferred_adapter_id == id)
                return;
            impl_->preferred_adapter_id = std::move(id);
        }

        if (impl_->running)
            invoke_sync([this] { impl_->refresh(); });
    }

    std::string BluezMonitor::preferred_adapter_id() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->preferred_adapter_id;
    }

} // namespace tether::bluetooth
