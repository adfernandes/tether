#pragma once

#include "tether/bluetooth/objects.hpp"

#include <chrono>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <optional>

namespace tether::bluetooth {

    struct MonitorState;

    // Watches BlueZ on the system bus and keeps a current snapshot of its object tree.
    class BluezMonitor {
    public:
        BluezMonitor();
        ~BluezMonitor();

        BluezMonitor(const BluezMonitor&) = delete;
        BluezMonitor& operator=(const BluezMonitor&) = delete;

        // Connects to the system bus and starts the watcher thread. BlueZ object
        // and controller capability reads happen on that thread so daemon startup
        // and the network event loop never wait for Bluetooth tooling.
        bool start();
        void stop();
        bool running() const;

        // Level-triggered eventfd, readable whenever the snapshot has changed.
        // Register with EpollEventLoop::addFd and call drain() from the callback.
        int event_fd() const;
        void drain();

        BluezObjects snapshot() const;
        Capability capability() const;

        // Reason from the last org.bluez Disconnected signal for the device
        std::string last_disconnect_reason(const std::string& address) const;

        // Clears the last disconnect reason for the device, so that the next disconnect will be reported.
        void clear_disconnect_reason(const std::string& address);

        // Which controller to use: "hciN" or an address, empty for the first powered one.
        void set_preferred_adapter(std::string id);
        std::string preferred_adapter_id() const;

        // Shared system-bus connection. GDBusConnection is thread-safe for call.
        GDBusConnection* connection() const;

        // Runs fn on the watcher thread and waits for it to finish.
        void invoke_sync(const std::function<void()>& fn);

    private:
        std::unique_ptr<MonitorState> impl_;
    };

    extern BluezMonitor* g_bluez;

    // BlueZ exposes Secure Connections only through the management interface.
    // Runs btmgmt without a shell and returns no value if it fails or exceeds the
    // deadline. Public so the process boundary and timeout can be tested directly.
    std::optional<bool> probe_secure_connections(const std::string& adapter_id,
                                                 std::chrono::milliseconds timeout = std::chrono::seconds(1));

} // namespace tether::bluetooth
