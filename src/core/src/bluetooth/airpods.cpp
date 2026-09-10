#include "tether/bluetooth/airpods.hpp"

#include <tether/i18n.hpp>
#include <tether/log.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
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

        // Ownership, the two packets Apple's own handoff turns on.
        constexpr uint8_t CLAIM[] = {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x01, 0x00, 0x00, 0x00};
        constexpr uint8_t RELEASE[] = {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00};
        // The firmware's own ownership verdict arrives on the same shape.
        constexpr uint8_t OWNS_CONNECTION_PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x06};

        // Claims are answered slowly and the firmware saturates on a burst of them.
        constexpr int CLAIM_COOLDOWN_MS = 1000;

        constexpr uint8_t HANDSHAKE_ACK[] = {0x01, 0x00, 0x04, 0x00};
        constexpr uint8_t FEATURES_ACK[] = {0x04, 0x00, 0x04, 0x00, 0x2b, 0x00};

        constexpr uint8_t BATTERY_PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x04, 0x00};
        constexpr size_t BATTERY_ENTRY_BYTES = 5;

        // Listening mode, sent and received on the same 11-byte:
        // 04 00 04 00 09 00 0D [mode] 00 00 00
        // Ear detection: 04 00 04 00 06 00 [primary] [secondary]
        constexpr uint8_t EAR_PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x06, 0x00};
        constexpr size_t EAR_PACKET_BYTES = 8;

        constexpr uint8_t CONNECTED_DEVICES_PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x2e, 0x00};
        constexpr size_t CONNECTED_DEVICES_ENTRY_BYTES = 8;

        constexpr uint8_t AUDIO_SOURCE_PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x0e, 0x00};
        constexpr size_t AUDIO_SOURCE_PACKET_BYTES = 13;

        constexpr uint8_t ANC_PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0d};
        constexpr size_t ANC_PACKET_BYTES = 11;
        constexpr size_t ANC_MODE_OFFSET = 7;

        // The firmware ignores anything sent before it has answered the step before,
        // and paces its own state broadcast at roughly 800ms.
        constexpr int HANDSHAKE_DELAY_MS = 200;
        // Sent anyway when the buds do not acknowledge; some models never do.
        constexpr int FEATURES_FALLBACK_MS = 500;
        constexpr int NOTIFICATIONS_AT_MS = 600;
        // The buds broadcast their host list unprompted at roughly 800ms. The opening
        // claim waits for it so it can be skipped when the phone already has them, and
        // goes out anyway at this deadline when no list arrives.
        constexpr int OPENING_CLAIM_BY_MS = 1500;

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
        // timeout and -2 on error. A negative timeout waits forever.
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

            const int rc = ::poll(fds, count, timeout_ms);
            if (rc == 0)
                return -1;
            if (rc < 0)
                return -2;
            if (socket_slot >= 0 && fds[socket_slot].revents != 0)
                return 1;
            return fds[wake_slot].revents != 0 ? 0 : -1;
        }

        // Six MAC bytes as BlueZ writes them. `reversed` for the notifications that
        // carry the address least-significant byte first.
        std::string format_address(const uint8_t* mac, bool reversed) {
            std::string out(17, ':');
            static constexpr char HEX[] = "0123456789ABCDEF";
            for (int i = 0; i < 6; ++i) {
                const uint8_t byte = mac[reversed ? 5 - i : i];
                out[static_cast<size_t>(i) * 3] = HEX[byte >> 4];
                out[static_cast<size_t>(i) * 3 + 1] = HEX[byte & 0x0f];
            }
            return out;
        }

        bool starts_with(const uint8_t* data, size_t len, const uint8_t* prefix, size_t prefix_len) {
            return len >= prefix_len && std::memcmp(data, prefix, prefix_len) == 0;
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
        nlohmann::json j = {
            {"command", "bt_airpods"},
            {"address", s.address},
            {"name", s.name},
            {"left", s.battery.left},
            {"right", s.battery.right},
            {"case", s.battery.case_},
            {"ear", {{"primary", to_string(s.ear.primary)}, {"secondary", to_string(s.ear.secondary)}}},
            {"in_ear", s.ear.in_ear()},
            {"peer_taking_over", s.peer_taking_over},
            {"peer_active", s.peer_active},
            {"status", to_string(s.status)},
            {"reason", s.reason},
        };
        if (s.anc)
            j["anc"] = to_string(*s.anc);
        return j;
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

    const char* to_string(EarStatus status) {
        switch (status) {
        case EarStatus::Unknown:
            return "unknown";
        case EarStatus::InEar:
            return "in_ear";
        case EarStatus::OutOfEar:
            return "out_of_ear";
        case EarStatus::InCase:
            return "in_case";
        }
        return "unknown";
    }

    std::optional<EarState> parse_ear(const uint8_t* data, size_t len) {
        if (data == nullptr || len != EAR_PACKET_BYTES)
            return std::nullopt;
        if (std::memcmp(data, EAR_PREFIX, sizeof(EAR_PREFIX)) != 0)
            return std::nullopt;

        const auto decode = [](uint8_t byte) {
            switch (byte) {
            case 0x00:
                return EarStatus::InEar;
            case 0x01:
                return EarStatus::OutOfEar;
            case 0x02:
                return EarStatus::InCase;
            default:
                return EarStatus::Unknown;
            }
        };
        return EarState{decode(data[6]), decode(data[7])};
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

    std::optional<std::vector<AapPeer>> parse_connected_devices(const uint8_t* data, size_t len) {
        if (data == nullptr || len < sizeof(CONNECTED_DEVICES_PREFIX) + 1)
            return std::nullopt;
        if (std::memcmp(data, CONNECTED_DEVICES_PREFIX, sizeof(CONNECTED_DEVICES_PREFIX)) != 0)
            return std::nullopt;

        const size_t count = data[sizeof(CONNECTED_DEVICES_PREFIX)];
        size_t offset = sizeof(CONNECTED_DEVICES_PREFIX) + 1;
        if (offset + count * CONNECTED_DEVICES_ENTRY_BYTES > len)
            return std::nullopt;

        std::vector<AapPeer> peers;
        peers.reserve(count);
        for (size_t i = 0; i < count; ++i, offset += CONNECTED_DEVICES_ENTRY_BYTES)
            peers.push_back({format_address(data + offset, true), data[offset + 6], data[offset + 7]});
        return peers;
    }

    std::optional<AudioSourceEvent> parse_audio_source(const uint8_t* data, size_t len) {
        if (data == nullptr || len != AUDIO_SOURCE_PACKET_BYTES)
            return std::nullopt;
        if (std::memcmp(data, AUDIO_SOURCE_PREFIX, sizeof(AUDIO_SOURCE_PREFIX)) != 0)
            return std::nullopt;

        AudioSourceEvent event;
        event.address = format_address(data + sizeof(AUDIO_SOURCE_PREFIX), false);
        switch (data[AUDIO_SOURCE_PACKET_BYTES - 1]) {
        case 0x01:
            event.source = AudioSource::Call;
            break;
        case 0x02:
            event.source = AudioSource::Media;
            break;
        default:
            event.source = AudioSource::None;
            break;
        }
        return event;
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
            // A bitmask: a pod charging in an open case reports charging|disconnected.
            const int percent = ((status & AAP_STATUS_DISCONNECTED) || level > 100) ? -1 : static_cast<int>(level);
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

    MediaAction ear_media_action(const EarState& before, const EarState& after, PauseMode mode, bool holding) {
        if (mode == PauseMode::Never)
            return MediaAction::None;
        if (!before.known() || !after.known() || before == after)
            return MediaAction::None;

        const int needed = mode == PauseMode::OneRemoved ? 2 : 1;
        const bool worn_before = before.in_ear() >= needed;
        const bool worn_after = after.in_ear() >= needed;

        if (worn_before && !worn_after)
            return MediaAction::Pause;
        if (!worn_before && worn_after && holding)
            return MediaAction::Resume;
        return MediaAction::None;
    }

    HandoffAction ownership_action(bool peer_active, bool released, bool enabled) {
        // Buds already given up come back when the peer lets go, whatever the
        // setting says now: the alternative is audio stranded on the phone.
        if (released && !peer_active)
            return HandoffAction::Reclaim;
        if (!enabled)
            return HandoffAction::None;
        if (peer_active && !released)
            return HandoffAction::Release;
        return HandoffAction::None;
    }

    PeerSummary summarize_peers(const std::vector<AapPeer>& peers, const std::string& local) {
        PeerSummary summary;
        for (const auto& peer : peers) {
            if (!local.empty() && peer.address == local)
                continue;
            summary.taking_over = summary.taking_over || peer.taking_over();
            summary.active = summary.active || peer.active();
        }
        return summary;
    }

    bool call_wants_audio(const nlohmann::json& calls) {
        if (!calls.is_array())
            return false;
        for (const auto& call : calls) {
            // Ringing counts: the buds have to be on the phone before it is answered.
            if (call.value("ringing", false) || call.value("outgoing", false) || call.value("connected", false))
                return true;
        }
        return false;
    }

    HandoffAction handoff_action(bool call_active, bool buds_on_linux, bool released, bool enabled) {
        // Only after the call, and only buds this code took away. Ahead of the
        // enabled check: buds given to the phone come back even if the setting
        // was turned off while they were away.
        if (!call_active && released)
            return HandoffAction::Reclaim;
        if (!enabled)
            return HandoffAction::None;
        if (call_active && buds_on_linux && !released)
            return HandoffAction::Release;
        return HandoffAction::None;
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

        // Session setup, paced by the buds' own acknowledgements.
        enum class Stage { Handshake, Features, Notifications, Ready };

        std::function<void(const AirPodsState&)> on_change;

        mutable std::mutex mutex;
        AirPodsState state;
        AirPodsState published;
        std::string target_address;
        std::string target_name;
        std::string local_address;
        // Handed to the worker rather than written from the caller's thread: the
        // socket belongs to the session and closing it out from under a write is
        // the one race worth not having.
        std::optional<AncMode> pending_anc;
        std::optional<bool> pending_ownership;
        std::chrono::steady_clock::time_point last_claim{};
        bool stopping = false;
        bool enabled = true;

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
            return stopping || !enabled || target_address != address;
        }

        // Sleeps, returning early if the target changed or the watcher is stopping.
        void backoff(int ms) {
            if (wait_for(-1, wake_fd, 0, ms) == 0)
                drain_wake();
        }

        // Records a peer list, logging every ownership transition: almost every
        // diagnosis of a dropped link starts from one.
        void note_peers(const std::vector<AapPeer>& peers, std::map<std::string, uint8_t>& seen, bool delivered) {
            std::string local;
            {
                std::lock_guard<std::mutex> lock(mutex);
                local = local_address;
            }
            for (const auto& peer : peers) {
                auto [it, inserted] = seen.insert({peer.address, peer.state});
                if (inserted || it->second != peer.state) {
                    debug::log(DEBUG,
                               "airpods: {} {} {:#04x} -> {:#04x}",
                               peer.address,
                               peer.address == local ? "us" : "peer",
                               inserted ? peer.state : it->second,
                               peer.state);
                    it->second = peer.state;
                }
            }

            const auto summary = summarize_peers(peers, local);
            {
                std::lock_guard<std::mutex> lock(mutex);
                state.peer_taking_over = summary.taking_over;
                state.peer_active = summary.active;
            }
            publish(delivered ? AirPodsStatus::Live : AirPodsStatus::Connecting, "");
        }

        // Takes or gives up ownership, on the worker thread. A claim sent while a peer
        // is taking the buds is the one packet that reliably kills the link, so the
        // peer state from the last connected-devices notification gates it. An idle
        // iPhone is not that: claiming out from under one is the whole point.
        bool send_ownership(int fd, bool own) {
            if (!own) {
                if (write_all(fd, RELEASE, sizeof(RELEASE)))
                    return true;
                debug::log(DEBUG, "airpods: release write failed: {}", std::strerror(errno));
                return false;
            }

            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (state.peer_taking_over) {
                    debug::log(DEBUG, "airpods: not claiming, a peer is taking the buds");
                    return true;
                }
                // ponytail: a flat cooldown. Claims come from call transitions here, not
                // from every play event; measure the pause length if that ever changes.
                if (now - last_claim < std::chrono::milliseconds(CLAIM_COOLDOWN_MS))
                    return true;
                last_claim = now;
            }
            if (write_all(fd, CLAIM, sizeof(CLAIM)))
                return true;
            debug::log(DEBUG, "airpods: claim write failed: {}", std::strerror(errno));
            return false;
        }

        // Sends whatever the caller queued, on the worker thread. False means the
        // write failed and the session is over.
        bool send_pending(int fd) {
            std::optional<AncMode> mode;
            std::optional<bool> ownership;
            {
                std::lock_guard<std::mutex> lock(mutex);
                std::swap(mode, pending_anc);
                std::swap(ownership, pending_ownership);
            }
            if (ownership && !send_ownership(fd, *ownership))
                return false;
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

            using clock = std::chrono::steady_clock;
            const auto opened = clock::now();
            Stage stage = Stage::Handshake;
            auto due = opened + std::chrono::milliseconds(HANDSHAKE_DELAY_MS);
            auto last_packet = opened;

            // Sends the next step of the session setup. Each one goes out on the
            // previous step's acknowledgement, or at its deadline when the buds stay
            // quiet, because the firmware discards anything sent ahead of its own pace.
            const auto advance = [&](clock::time_point now) {
                switch (stage) {
                case Stage::Handshake:
                    if (!write_all(fd, HANDSHAKE, sizeof(HANDSHAKE)))
                        return false;
                    stage = Stage::Features;
                    due = now + std::chrono::milliseconds(FEATURES_FALLBACK_MS);
                    return true;
                case Stage::Features:
                    if (!write_all(fd, SET_FEATURES, sizeof(SET_FEATURES)))
                        return false;
                    stage = Stage::Notifications;
                    due = std::max(opened + std::chrono::milliseconds(NOTIFICATIONS_AT_MS), now);
                    return true;
                case Stage::Notifications:
                    if (!write_all(fd, REQUEST_NOTIFICATIONS, sizeof(REQUEST_NOTIFICATIONS)))
                        return false;
                    stage = Stage::Ready;
                    due = opened + std::chrono::milliseconds(OPENING_CLAIM_BY_MS);
                    return true;
                case Stage::Ready:
                    return true;
                }
                return true;
            };

            // AirPods Pro 2 validates a host by itself once the session is up; Pro 3
            // never does, and an unvalidated host is dropped at the first pod movement.
            // So a session claims once it knows the phone does not have them, and keeps
            // waiting for that to be true rather than claiming into a peer's call.
            bool claim_sent = false;
            bool claim_deadline_passed = false;
            const auto claim = [&] {
                bool blocked = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    blocked = state.peer_taking_over;
                }
                if (blocked)
                    return true;
                claim_sent = true;
                return send_ownership(fd, true);
            };

            std::map<std::string, uint8_t> peer_states;
            std::array<uint8_t, 1024> buffer{};
            while (!retargeted(address)) {
                const auto now = clock::now();
                const auto quiet_for = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_packet);
                const int quiet_timeout = (delivered ? IDLE_TIMEOUT_MS : FIRST_PACKET_TIMEOUT_MS);
                int timeout = quiet_timeout - static_cast<int>(quiet_for.count());
                if (stage != Stage::Ready || !claim_deadline_passed) {
                    const auto until_due = std::chrono::duration_cast<std::chrono::milliseconds>(due - now);
                    timeout = std::min(timeout, static_cast<int>(until_due.count()));
                }
                const int ready = wait_for(fd, wake_fd, POLLIN, std::max(timeout, 0));
                if (ready == 0) {
                    drain_wake();
                    if (!send_pending(fd))
                        break;
                    continue;
                }
                if (ready == -2)
                    break;
                if (ready < 0) {
                    if (clock::now() >= due) {
                        if (stage != Stage::Ready) {
                            if (!advance(clock::now()))
                                break;
                            continue;
                        }
                        // No host list arrived in time, so claim on the reference's own
                        // schedule and let the peer check catch the rest.
                        claim_deadline_passed = true;
                        if (!claim_sent && !claim())
                            break;
                        continue;
                    }
                    if (clock::now() - last_packet >= std::chrono::milliseconds(quiet_timeout))
                        break;
                    continue;
                }

                const ssize_t got = ::recv(fd, buffer.data(), buffer.size(), 0);
                if (got <= 0)
                    break;

                const size_t size = static_cast<size_t>(got);
                last_packet = clock::now();

                if (stage == Stage::Features &&
                    starts_with(buffer.data(), size, HANDSHAKE_ACK, sizeof(HANDSHAKE_ACK))) {
                    if (!advance(last_packet))
                        break;
                    continue;
                }
                if (stage == Stage::Notifications &&
                    starts_with(buffer.data(), size, FEATURES_ACK, sizeof(FEATURES_ACK))) {
                    if (!advance(last_packet))
                        break;
                    continue;
                }

                if (starts_with(buffer.data(), size, OWNS_CONNECTION_PREFIX, sizeof(OWNS_CONNECTION_PREFIX)) &&
                    size > sizeof(OWNS_CONNECTION_PREFIX)) {
                    debug::log(DEBUG, "airpods: ownership verdict {:#04x}", buffer[sizeof(OWNS_CONNECTION_PREFIX)]);
                    continue;
                }

                if (auto peers = parse_connected_devices(buffer.data(), size)) {
                    note_peers(*peers, peer_states, delivered);
                    // The host list is what says whether claiming is safe.
                    if (stage == Stage::Ready && !claim_sent && !claim())
                        break;
                    continue;
                }

                if (auto source = parse_audio_source(buffer.data(), size)) {
                    debug::log(DEBUG,
                               "airpods: audio source {} on {}",
                               source->source == AudioSource::Call    ? "call"
                               : source->source == AudioSource::Media ? "media"
                                                                      : "none",
                               source->address);
                    continue;
                }

                if (auto mode = parse_anc(buffer.data(), size)) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.anc = *mode;
                    }
                    publish(delivered ? AirPodsStatus::Live : AirPodsStatus::Connecting, "");
                    continue;
                }

                if (auto ear = parse_ear(buffer.data(), size)) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.ear = *ear;
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
                // A queued write does not outlive the channel it was meant for.
                std::lock_guard<std::mutex> lock(mutex);
                pending_anc.reset();
                pending_ownership.reset();
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
                    address = enabled ? target_address : std::string{};
                    name = target_name;
                }

                if (address.empty()) {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.address.clear();
                        state.name.clear();
                        state.battery = {};
                        state.anc.reset();
                        state.ear = {};
                        state.peer_taking_over = false;
                        state.peer_active = false;
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
                        state.ear = {};
                        state.peer_taking_over = false;
                        state.peer_active = false;
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

    void AirPodsWatcher::set_device(const std::string& address, const std::string& name, const std::string& local) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->local_address = local;
            if (impl_->target_address == address && impl_->target_name == name)
                return;
            impl_->target_address = address;
            impl_->target_name = name;
        }
        impl_->wake();
    }

    void AirPodsWatcher::set_enabled(bool enabled) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->enabled == enabled)
                return;
            impl_->enabled = enabled;
        }
        impl_->wake();
    }

    void AirPodsWatcher::set_ownership(bool own) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->pending_ownership = own;
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
