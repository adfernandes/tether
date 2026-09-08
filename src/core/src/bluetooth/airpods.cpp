#include "tether/bluetooth/airpods.hpp"

#include <tether/i18n.hpp>
#include <tether/log.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

// The kernel's L2CAP ABI, declared here rather than pulled in from bluez-libs.
// glibc already defines some of them.
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef SOL_BLUETOOTH
#define SOL_BLUETOOTH 274
#endif
#ifndef BTPROTO_L2CAP
#define BTPROTO_L2CAP 0
#endif
#ifndef BT_SECURITY
#define BT_SECURITY 4
#endif
#ifndef BT_SECURITY_MEDIUM
#define BT_SECURITY_MEDIUM 2
#endif
#ifndef BDADDR_BREDR
#define BDADDR_BREDR 0
#endif

namespace {

    struct sockaddr_l2 {
        sa_family_t l2_family;
        uint16_t l2_psm;
        uint8_t l2_bdaddr[6];
        uint16_t l2_cid;
        uint8_t l2_bdaddr_type;
    };

    struct bt_security {
        uint8_t level;
        uint8_t key_size;
    };

} // namespace

namespace tether::bluetooth {

    AirPodsWatcher* g_airpods = nullptr;

    namespace {

        // AAP control packets,
        constexpr uint8_t HANDSHAKE[] = {
            0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        constexpr uint8_t SET_FEATURES[] = {
            0x04, 0x00, 0x04, 0x00, 0x4d, 0x00, 0xd7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        constexpr uint8_t REQUEST_NOTIFICATIONS[] = {0x04, 0x00, 0x04, 0x00, 0x0f, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff};

        constexpr uint8_t BATTERY_PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x04, 0x00};
        constexpr size_t BATTERY_ENTRY_BYTES = 5;

        // Listening mode, sent and received on the same 11-byte:
        // 04 00 04 00 09 00 0D [mode] 00 00 00
        constexpr uint8_t ANC_PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0d};
        constexpr size_t ANC_PACKET_BYTES = 11;
        constexpr size_t ANC_MODE_OFFSET = 7;

        constexpr int CONNECT_TIMEOUT_MS = 8000;
        constexpr int FIRST_PACKET_TIMEOUT_MS = 8000;
        // A channel that has gone quiet this long is recycled.
        constexpr int IDLE_TIMEOUT_MS = 300000;
        constexpr int BACKOFF_START_MS = 1000;
        constexpr int BACKOFF_MAX_MS = 30000;
        // A contended channel blocks with no error.
        constexpr int BUSY_AFTER_ATTEMPTS = 3;

        // "AA:BB:CC:DD:EE:FF" into the kernel's little-endian order.
        bool parse_address(const std::string& text, uint8_t out[6]) {
            if (text.size() != 17)
                return false;
            for (int i = 0; i < 6; ++i) {
                const size_t at = static_cast<size_t>(i) * 3;
                if (i < 5 && text[at + 2] != ':')
                    return false;
                char* end = nullptr;
                const long byte = std::strtol(text.substr(at, 2).c_str(), &end, 16);
                if (end == nullptr || *end != '\0' || byte < 0 || byte > 0xff)
                    return false;
                out[5 - i] = static_cast<uint8_t>(byte);
            }
            return true;
        }

        // Waits for `fd` or the wake eventfd. Returns 1 for fd, 0 for a wake, -1 on
        // error or timeout. A negative timeout waits forever.
        int wait_for(int fd, int wake_fd, short events, int timeout_ms) {
            pollfd fds[2]{};
            nfds_t count = 0;
            int socket_slot = -1;
            if (fd >= 0) {
                socket_slot = static_cast<int>(count);
                fds[count++] = {fd, events, 0};
            }
            const int wake_slot = static_cast<int>(count);
            fds[count++] = {wake_fd, POLLIN, 0};

            if (::poll(fds, count, timeout_ms) <= 0)
                return -1;
            if (socket_slot >= 0 && fds[socket_slot].revents != 0)
                return 1;
            return fds[wake_slot].revents != 0 ? 0 : -1;
        }

        bool write_all(int fd, const uint8_t* data, size_t len) {
            return ::send(fd, data, len, MSG_NOSIGNAL) == static_cast<ssize_t>(len);
        }

    } // namespace

    const char* to_string(AirPodsStatus status) {
        switch (status) {
        case AirPodsStatus::Idle:
            return "idle";
        case AirPodsStatus::Connecting:
            return "connecting";
        case AirPodsStatus::Live:
            return "live";
        case AirPodsStatus::Busy:
            return "busy";
        case AirPodsStatus::Failed:
            return "failed";
        }
        return "idle";
    }

    nlohmann::json to_json(const AirPodsState& s) {
        return {
            {"command", "bt_airpods"},
            {"address", s.address},
            {"name", s.name},
            {"left", s.battery.left},
            {"right", s.battery.right},
            {"case", s.battery.case_},
            {"anc", s.anc ? nlohmann::json(to_string(*s.anc)) : nlohmann::json()},
            {"status", to_string(s.status)},
            {"reason", s.reason},
        };
    }

    const char* to_string(AncMode mode) {
        switch (mode) {
        case AncMode::Off:
            return "off";
        case AncMode::NoiseCancellation:
            return "anc";
        case AncMode::Transparency:
            return "transparency";
        case AncMode::Adaptive:
            return "adaptive";
        }
        return "off";
    }

    std::optional<AncMode> anc_mode_from_string(const std::string& name) {
        for (AncMode mode : {AncMode::Off, AncMode::NoiseCancellation, AncMode::Transparency, AncMode::Adaptive})
            if (name == to_string(mode))
                return mode;
        return std::nullopt;
    }

    std::optional<AncMode> parse_anc(const uint8_t* data, size_t len) {
        if (data == nullptr || len != ANC_PACKET_BYTES)
            return std::nullopt;
        if (std::memcmp(data, ANC_PREFIX, sizeof(ANC_PREFIX)) != 0)
            return std::nullopt;
        const uint8_t mode = data[ANC_MODE_OFFSET];
        if (mode < static_cast<uint8_t>(AncMode::Off) || mode > static_cast<uint8_t>(AncMode::Adaptive))
            return std::nullopt;
        return static_cast<AncMode>(mode);
    }

    std::optional<BatteryUpdate> parse_battery(const uint8_t* data, size_t len) {
        if (data == nullptr || len < sizeof(BATTERY_PREFIX) + 1)
            return std::nullopt;
        if (std::memcmp(data, BATTERY_PREFIX, sizeof(BATTERY_PREFIX)) != 0)
            return std::nullopt;

        const size_t count = data[sizeof(BATTERY_PREFIX)];
        size_t offset = sizeof(BATTERY_PREFIX) + 1;
        if (count == 0 || offset + count * BATTERY_ENTRY_BYTES > len)
            return std::nullopt;

        BatteryUpdate update;
        for (size_t i = 0; i < count; ++i, offset += BATTERY_ENTRY_BYTES) {
            const uint8_t component = data[offset];
            const uint8_t level = data[offset + 2];
            const uint8_t status = data[offset + 3];
            const int percent = (status == AAP_STATUS_DISCONNECTED || level > 100) ? -1 : static_cast<int>(level);
            switch (component) {
            case AAP_COMPONENT_LEFT:
                update.levels.left = percent;
                update.has_left = true;
                break;
            case AAP_COMPONENT_RIGHT:
                update.levels.right = percent;
                update.has_right = true;
                break;
            case AAP_COMPONENT_CASE:
                update.levels.case_ = percent;
                update.has_case = true;
                break;
            default:
                break;
            }
        }
        if (!update.has_left && !update.has_right && !update.has_case)
            return std::nullopt;
        return update;
    }

    const Device* find_airpods(const BluezObjects& objects) {
        for (const auto& device : objects.devices)
            if (device.connected && device.looks_like_airpods())
                return &device;
        return nullptr;
    }

    void merge_battery(AirPodsBattery& into, const BatteryUpdate& update) {
        if (update.has_left)
            into.left = update.levels.left;
        if (update.has_right)
            into.right = update.levels.right;
        if (update.has_case)
            into.case_ = update.levels.case_;
    }

    struct AirPodsWatcher::Impl {
        explicit Impl(std::function<void(const AirPodsState&)> cb) : on_change(std::move(cb)) {}

        std::function<void(const AirPodsState&)> on_change;

        mutable std::mutex mutex;
        AirPodsState state;
        AirPodsState published;
        std::string target_address;
        std::string target_name;
        // Handed to the worker rather than written from the caller's thread: the
        // socket belongs to the session and closing it out from under a write is
        // the one race worth not having.
        std::optional<AncMode> pending_anc;
        bool stopping = false;

        int wake_fd = -1;
        std::thread worker;

        void wake() {
            const uint64_t one = 1;
            [[maybe_unused]] const ssize_t written = ::write(wake_fd, &one, sizeof(one));
        }

        void drain_wake() {
            uint64_t value = 0;
            [[maybe_unused]] const ssize_t got = ::read(wake_fd, &value, sizeof(value));
        }

        // Publishes only when the state actually changed, as the connection status does.
        void publish(AirPodsStatus status, const std::string& reason) {
            AirPodsState copy;
            {
                std::lock_guard<std::mutex> lock(mutex);
                state.status = status;
                state.reason = reason;
                if (state == published)
                    return;
                published = state;
                copy = state;
            }
            if (on_change)
                on_change(copy);
        }

        bool retargeted(const std::string& address) const {
            std::lock_guard<std::mutex> lock(mutex);
            return stopping || target_address != address;
        }

        // Sleeps, returning early if the target changed or the watcher is stopping.
        void backoff(int ms) {
            if (wait_for(-1, wake_fd, 0, ms) == 0)
                drain_wake();
        }

        // Sends whatever the caller queued, on the worker thread. False means the
        // write failed and the session is over.
        bool send_pending(int fd) {
            std::optional<AncMode> mode;
            {
                std::lock_guard<std::mutex> lock(mutex);
                std::swap(mode, pending_anc);
            }
            if (!mode)
                return true;
            const uint8_t packet[] = {
                0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0d, static_cast<uint8_t>(*mode), 0x00, 0x00, 0x00};
            static_assert(sizeof(packet) == ANC_PACKET_BYTES);
            if (write_all(fd, packet, sizeof(packet)))
                return true;
            debug::log(DEBUG, "airpods: listening mode write failed: {}", std::strerror(errno));
            return false;
        }

        // Runs one channel from connect to close. Sets `delivered` when at least one
        // battery packet arrived, which is what separates a contended channel from a
        // healthy but quiet one.
        void session(const std::string& address, bool& delivered) {
            uint8_t addr[6] = {};
            if (!parse_address(address, addr)) {
                publish(AirPodsStatus::Failed, _("The AirPods address could not be read."));
                return;
            }

            const int fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_L2CAP);
            if (fd < 0) {
                debug::log(DEBUG, "airpods: socket failed: {}", std::strerror(errno));
                publish(AirPodsStatus::Failed, _("Bluetooth sockets are unavailable on this system."));
                return;
            }

            sockaddr_l2 sa{};
            sa.l2_family = AF_BLUETOOTH;
            sa.l2_psm = AAP_PSM;
            sa.l2_bdaddr_type = BDADDR_BREDR;
            std::memcpy(sa.l2_bdaddr, addr, sizeof(addr));

            int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
            if (rc < 0 && (errno == EACCES || errno == EPERM)) {
                // Some controllers refuse an unauthenticated channel.
                bt_security sec{BT_SECURITY_MEDIUM, 0};
                ::setsockopt(fd, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec));
                rc = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
            }
            if (rc < 0 && errno != EINPROGRESS) {
                debug::log(DEBUG, "airpods: connect failed: {}", std::strerror(errno));
                ::close(fd);
                return;
            }
            if (rc < 0) {
                if (wait_for(fd, wake_fd, POLLOUT, CONNECT_TIMEOUT_MS) != 1) {
                    debug::log(DEBUG, "airpods: channel did not open within {}ms", CONNECT_TIMEOUT_MS);
                    ::close(fd);
                    return;
                }
                int error = 0;
                socklen_t size = sizeof(error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error != 0) {
                    debug::log(DEBUG, "airpods: connect failed: {}", std::strerror(error));
                    ::close(fd);
                    return;
                }
            }

            if (!write_all(fd, HANDSHAKE, sizeof(HANDSHAKE)) || !write_all(fd, SET_FEATURES, sizeof(SET_FEATURES)) ||
                !write_all(fd, REQUEST_NOTIFICATIONS, sizeof(REQUEST_NOTIFICATIONS))) {
                debug::log(DEBUG, "airpods: handshake failed: {}", std::strerror(errno));
                ::close(fd);
                return;
            }

            std::array<uint8_t, 1024> buffer{};
            while (!retargeted(address)) {
                const int timeout = delivered ? IDLE_TIMEOUT_MS : FIRST_PACKET_TIMEOUT_MS;
                const int ready = wait_for(fd, wake_fd, POLLIN, timeout);
                if (ready == 0) {
                    drain_wake();
                    if (!send_pending(fd))
                        break;
                    continue;
                }
                if (ready < 0)
                    break;

                const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
                if (got <= 0)
                    break;

                const size_t size = static_cast<size_t>(got);
                if (auto mode = parse_anc(buffer.data(), size)) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.anc = *mode;
                    }
                    publish(delivered ? AirPodsStatus::Live : AirPodsStatus::Connecting, "");
                    continue;
                }

                auto update = parse_battery(buffer.data(), size);
                if (!update)
                    continue;

                delivered = true;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    merge_battery(state.battery, *update);
                }
                publish(AirPodsStatus::Live, "");
            }

            {
                // A queued mode change does not outlive the channel it was meant for.
                std::lock_guard<std::mutex> lock(mutex);
                pending_anc.reset();
            }
            ::close(fd);
        }

        void run() {
            int backoff_ms = BACKOFF_START_MS;
            int quiet_attempts = 0;

            while (true) {
                std::string address;
                std::string name;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (stopping)
                        return;
                    address = target_address;
                    name = target_name;
                }

                if (address.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.address.clear();
                        state.name.clear();
                        state.battery = {};
                        state.anc.reset();
                    }
                    publish(AirPodsStatus::Idle, "");
                    backoff_ms = BACKOFF_START_MS;
                    quiet_attempts = 0;
                    if (wait_for(-1, wake_fd, 0, -1) == 0)
                        drain_wake();
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (state.address != address) {
                        state.address = address;
                        state.battery = {};
                        state.anc.reset();
                    }
                    state.name = name;
                }
                if (quiet_attempts < BUSY_AFTER_ATTEMPTS)
                    publish(AirPodsStatus::Connecting, "");

                bool delivered = false;
                session(address, delivered);

                if (retargeted(address))
                    continue;

                if (delivered) {
                    quiet_attempts = 0;
                    backoff_ms = BACKOFF_START_MS;
                } else if (++quiet_attempts == BUSY_AFTER_ATTEMPTS) {
                    debug::log(INFO, "airpods: the AAP channel is not answering; another program probably holds it");
                    publish(AirPodsStatus::Busy, _("Another program is using the AirPods channel."));
                }

                backoff(backoff_ms);
                backoff_ms = std::min(backoff_ms * 2, BACKOFF_MAX_MS);
            }
        }
    };

    AirPodsWatcher::AirPodsWatcher(std::function<void(const AirPodsState&)> on_change)
        : impl_(std::make_unique<Impl>(std::move(on_change))) {
        impl_->wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (impl_->wake_fd < 0) {
            debug::log(WARN, "airpods: eventfd failed: {}", std::strerror(errno));
            return;
        }
        impl_->worker = std::thread([this] { impl_->run(); });
    }

    AirPodsWatcher::~AirPodsWatcher() {
        if (impl_->worker.joinable()) {
            {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->stopping = true;
            }
            impl_->wake();
            impl_->worker.join();
        }
        if (impl_->wake_fd >= 0)
            ::close(impl_->wake_fd);
    }

    void AirPodsWatcher::set_device(const std::string& address, const std::string& name) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->target_address == address && impl_->target_name == name)
                return;
            impl_->target_address = address;
            impl_->target_name = name;
        }
        impl_->wake();
    }

    void AirPodsWatcher::set_anc(AncMode mode) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->pending_anc = mode;
        }
        impl_->wake();
    }

    AirPodsState AirPodsWatcher::state() const {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->state;
    }

} // namespace tether::bluetooth
