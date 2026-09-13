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
#include <variant>

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

        // Stem config: the host takes single press, and the buds send it a stem-press notification
        // instead of running their own play/pause, which moves the audio to another host. The other
        // press types keep their firmware actions. Reset by the firmware on every handoff.
        constexpr uint8_t STEM_CONFIG[] = {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x39, 0x01, 0x00, 0x00, 0x00};
        constexpr uint8_t STEM_PRESS_PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x19, 0x00};
        constexpr size_t STEM_PRESS_PACKET_BYTES = 8;
        // Smart-routing messages another host relays through the buds.
        constexpr uint8_t SMART_ROUTING_RESPONSE_PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x11, 0x00};

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
        // Two bytes of unknown meaning sit between the prefix and the count.
        constexpr size_t CONNECTED_DEVICES_COUNT_OFFSET = 8;
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
        // The stem config is dropped silently until the state broadcast the notification request
        // starts has finished; AirPods Center measured this on Pro 2 and Pro 3.
        constexpr int STEM_CONFIG_AT_MS = 1500;
        // How long the session claim waits after the channel opens.
        // A claim sent while the audio link is still being set up makes the buds drop
        // the link; the AirPods Center reference puts audio setup safe from 5s.
        constexpr int CLAIM_SETTLE_MS = 5000;
        // After that claim, how long this machine has to start playing before ownership goes back.
        constexpr int IDLE_RELEASE_MS = 3000;
        // After a claim, before the session's notification request goes out again. The
        // firmware discards anything sent while it is still broadcasting its own state.
        constexpr int CONFIG_RESEND_MS = 500;

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

        std::string hex(const uint8_t* data, size_t len) {
            static constexpr char HEX[] = "0123456789abcdef";
            std::string out;
            out.reserve(len * 3);
            for (size_t i = 0; i < len; ++i) {
                if (i > 0)
                    out.push_back(' ');
                out.push_back(HEX[data[i] >> 4]);
                out.push_back(HEX[data[i] & 0x0f]);
            }
            return out;
        }

        // Non-printable bytes as '.'.
        std::string printable(const uint8_t* data, size_t len) {
            std::string out(data, data + len);
            for (auto& c : out)
                if (c < 0x20 || c > 0x7e)
                    c = '.';
            return out;
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
            {"peer_audio", s.peer_audio},
            {"peer_call", s.peer_call},
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

    std::optional<StemPress> parse_stem_press(const uint8_t* data, size_t len) {
        if (data == nullptr || len != STEM_PRESS_PACKET_BYTES)
            return std::nullopt;
        if (std::memcmp(data, STEM_PRESS_PREFIX, sizeof(STEM_PRESS_PREFIX)) != 0)
            return std::nullopt;
        switch (data[6]) {
        case 0x05:
            return StemPress::Single;
        case 0x06:
            return StemPress::Double;
        case 0x07:
            return StemPress::Triple;
        case 0x08:
            return StemPress::Long;
        default:
            return std::nullopt;
        }
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
        if (data == nullptr || len < CONNECTED_DEVICES_COUNT_OFFSET + 1)
            return std::nullopt;
        if (std::memcmp(data, CONNECTED_DEVICES_PREFIX, sizeof(CONNECTED_DEVICES_PREFIX)) != 0)
            return std::nullopt;

        const size_t count = data[CONNECTED_DEVICES_COUNT_OFFSET];
        size_t offset = CONNECTED_DEVICES_COUNT_OFFSET + 1;
        if (offset + count * CONNECTED_DEVICES_ENTRY_BYTES > len)
            return std::nullopt;

        std::vector<AapPeer> peers;
        peers.reserve(count);
        for (size_t i = 0; i < count; ++i, offset += CONNECTED_DEVICES_ENTRY_BYTES)
            peers.push_back({format_address(data + offset, false), data[offset + 6], data[offset + 7]});
        return peers;
    }

    std::optional<AudioSourceEvent> parse_audio_source(const uint8_t* data, size_t len) {
        if (data == nullptr || len != AUDIO_SOURCE_PACKET_BYTES)
            return std::nullopt;
        if (std::memcmp(data, AUDIO_SOURCE_PREFIX, sizeof(AUDIO_SOURCE_PREFIX)) != 0)
            return std::nullopt;

        AudioSourceEvent event;
        event.address = format_address(data + sizeof(AUDIO_SOURCE_PREFIX), true);
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

    bool takes_over_for_play(std::optional<bool> owns, bool peer_call, bool busy) {
        return owns != true && !peer_call && !busy;
    }

    bool releases_when_idle(bool local_audio, bool peer_present, bool yielded) {
        return peer_present && !local_audio && !yielded;
    }

    namespace {

        // Apple's OPACK, the subset smart routing uses: one dictionary of strings, integers and booleans.
        using OpackValue = std::variant<bool, int64_t, std::string>;
        using OpackEntries = std::vector<std::pair<std::string, OpackValue>>;

        void opack_append(std::vector<uint8_t>& out, const OpackValue& value) {
            if (const auto* flag = std::get_if<bool>(&value)) {
                out.push_back(*flag ? 0x01 : 0x02);
            } else if (const auto* number = std::get_if<int64_t>(&value)) {
                const auto n = static_cast<uint64_t>(*number);
                if (*number >= 0 && *number <= 0x27) {
                    out.push_back(static_cast<uint8_t>(0x08 + n));
                } else {
                    const int size = *number >= 0 && n <= 0xff ? 1 : *number >= 0 && n <= 0xffff ? 2 : 4;
                    out.push_back(static_cast<uint8_t>(0x30 + (size == 1 ? 0 : size == 2 ? 1 : 2)));
                    for (int i = 0; i < size; ++i)
                        out.push_back(static_cast<uint8_t>(n >> (8 * i)));
                }
            } else {
                const auto& text = std::get<std::string>(value);
                if (text.size() <= 0x20) {
                    out.push_back(static_cast<uint8_t>(0x40 + text.size()));
                } else {
                    out.push_back(0x61);
                    out.push_back(static_cast<uint8_t>(text.size()));
                }
                out.insert(out.end(), text.begin(), text.end());
            }
        }

        // A smart-routing packet: opcode, the target least-significant byte first, the body length,
        // then 01 and the dictionary.
        std::vector<uint8_t> smart_routing_packet(const std::string& target, const OpackEntries& entries) {
            uint8_t addr[6] = {};
            if (!parse_address(target, addr) || entries.size() >= 15)
                return {};
            std::vector<uint8_t> body = {0x01, static_cast<uint8_t>(0xe0 + entries.size())};
            for (const auto& [key, value] : entries) {
                opack_append(body, key);
                opack_append(body, value);
            }
            static constexpr uint8_t PREFIX[] = {0x04, 0x00, 0x04, 0x00, 0x10, 0x00};
            std::vector<uint8_t> packet(std::begin(PREFIX), std::end(PREFIX));
            packet.insert(packet.end(), std::begin(addr), std::end(addr));
            packet.push_back(static_cast<uint8_t>(body.size()));
            packet.push_back(static_cast<uint8_t>(body.size() >> 8));
            packet.insert(packet.end(), body.begin(), body.end());
            return packet;
        }

        // Decodes one value. `seen` is OPACK's back-reference table: every string and wide integer,
        // once each, which 0xA0 + index refers to.
        std::optional<OpackValue>
            opack_read(const uint8_t* data, size_t len, size_t& at, std::vector<OpackValue>& seen) {
            if (at >= len)
                return std::nullopt;
            const uint8_t tag = data[at++];
            const auto remember = [&](OpackValue value) {
                if (std::find(seen.begin(), seen.end(), value) == seen.end())
                    seen.push_back(value);
                return std::optional<OpackValue>(std::move(value));
            };
            const auto little_endian = [&](size_t size) -> std::optional<uint64_t> {
                if (len - at < size)
                    return std::nullopt;
                uint64_t n = 0;
                for (size_t i = 0; i < size; ++i)
                    n |= static_cast<uint64_t>(data[at + i]) << (8 * i);
                at += size;
                return n;
            };
            if (tag == 0x01 || tag == 0x02)
                return OpackValue(tag == 0x01);
            if (tag >= 0x08 && tag <= 0x2f)
                return OpackValue(static_cast<int64_t>(tag - 0x08));
            if (tag >= 0x30 && tag <= 0x33) {
                const auto n = little_endian(size_t{1} << (tag - 0x30));
                return n ? remember(static_cast<int64_t>(*n)) : std::nullopt;
            }
            if ((tag >= 0x40 && tag <= 0x60) || (tag >= 0x61 && tag <= 0x64)) {
                std::optional<uint64_t> size =
                    tag <= 0x60 ? std::optional<uint64_t>(tag - 0x40) : little_endian(size_t{1} << (tag - 0x61));
                if (!size || len - at < *size)
                    return std::nullopt;
                std::string text(data + at, data + at + *size);
                at += *size;
                return remember(std::move(text));
            }
            if (tag >= 0xa0 && tag <= 0xc0 && static_cast<size_t>(tag - 0xa0) < seen.size())
                return seen[tag - 0xa0];
            return std::nullopt;
        }

    } // namespace

    std::optional<SmartRoutingMessage> parse_smart_routing(const uint8_t* data, size_t len) {
        constexpr size_t BODY_AT = 14;
        if (data == nullptr || len < BODY_AT + 2 ||
            !starts_with(data, len, SMART_ROUTING_RESPONSE_PREFIX, sizeof(SMART_ROUTING_RESPONSE_PREFIX)))
            return std::nullopt;
        const size_t body_len = data[12] | (data[13] << 8);
        if (body_len < 2 || BODY_AT + body_len > len || data[BODY_AT] != 0x01)
            return std::nullopt;
        const uint8_t dict = data[BODY_AT + 1];
        if (dict < 0xe0 || dict >= 0xef)
            return std::nullopt;

        SmartRoutingMessage message;
        message.sender = format_address(data + 6, true);
        const size_t end = BODY_AT + body_len;
        size_t at = BODY_AT + 2;
        std::vector<OpackValue> seen;
        for (int i = 0; i < dict - 0xe0; ++i) {
            const auto key = opack_read(data, end, at, seen);
            const auto value = key ? opack_read(data, end, at, seen) : std::nullopt;
            if (!value || !std::holds_alternative<std::string>(*key))
                return std::nullopt;
            const auto& name = std::get<std::string>(*key);
            if (name == "playingApp" && std::holds_alternative<std::string>(*value))
                message.app = std::get<std::string>(*value);
            else if (name == "hostStreamingState" && std::holds_alternative<std::string>(*value))
                message.streaming = std::get<std::string>(*value) == "YES";
            else if (name == "otherDeviceAudioCategory" && std::holds_alternative<int64_t>(*value))
                message.category = static_cast<int>(std::get<int64_t>(*value));
            else if (name == "audioRoutingSetOwnershipToFalse" && std::holds_alternative<bool>(*value))
                message.set_ownership_to_false = std::get<bool>(*value);
        }
        return message;
    }

    std::vector<uint8_t> tipi_add_device(const std::string& self, const std::string& target) {
        if (self.size() != 17)
            return {};
        return smart_routing_packet(target,
                                    {{"idleTime", int64_t{0}},
                                     {"newTipi", true},
                                     {"btAddress", self},
                                     {"btName", std::string("Android")},
                                     {"nearbyAudioScore", int64_t{6}}});
    }

    // The keys and their order are the iPhone's own report.
    std::vector<uint8_t> smart_routing_media_info(const std::string& self, const std::string& target, bool streaming) {
        if (self.size() != 17)
            return {};
        return smart_routing_packet(
            target,
            {{"playingApp", std::string(streaming ? "Unknown" : "NA")},
             {"hostStreamingState", std::string(streaming ? "YES" : "NO")},
             {"btAddress", self},
             {"btName", std::string("Android")},
             {"otherDeviceAudioCategory", int64_t{streaming ? AUDIO_CATEGORY_MEDIA : AUDIO_CATEGORY_NONE}}});
    }

    std::vector<uint8_t> smart_routing_hijack(const std::string& target) {
        return smart_routing_packet(target,
                                    {{"localscore", int64_t{100}},
                                     {"reason", std::string("Hijackv2")},
                                     {"audioRoutingScore", int64_t{301}},
                                     {"audioRoutingSetOwnershipToFalse", true},
                                     {"remotescore", int64_t{301}}});
    }

    PeerSummary summarize_peers(const std::vector<AapPeer>& peers, const std::string& local) {
        PeerSummary summary;
        for (const auto& peer : peers) {
            if (!local.empty() && peer.address == local)
                continue;
            summary.present = summary.present || !local.empty();
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
        enum class Stage { Handshake, Features, Notifications, StemConfig, Ready };

        std::function<void(const AirPodsState&)> on_change;
        std::function<void(StemPress)> on_stem_press;

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
        bool pending_take_over = false;
        // Whether something plays here, as reported to the other hosts. `streaming_sent` is what the
        // current session last told them.
        bool streaming = false;
        std::optional<bool> streaming_sent;
        // This machine gave the buds up: no claim of the watcher's own goes out until it takes them back.
        bool yielded = false;
        // A claim has just gone out, so the session's notification request is due again: the
        // firmware resets its AAP state on every handoff and silently drops what was set before.
        bool config_resend_due = false;
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
        PeerSummary note_peers(const std::vector<AapPeer>& peers,
                               std::map<std::string, uint8_t>& seen,
                               bool& warned_unlisted,
                               bool delivered) {
            std::string local;
            {
                std::lock_guard<std::mutex> lock(mutex);
                local = local_address;
            }
            bool listed = false;
            std::optional<bool> mine;
            for (const auto& peer : peers) {
                if (peer.address == local) {
                    listed = true;
                    mine = peer.active();
                }
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

            // Without its own entry this machine reads as a peer: an eviction, or a misread list.
            if (!local.empty() && !listed && !warned_unlisted) {
                debug::log(DEBUG, "airpods: this machine ({}) is not in the buds' host list", local);
                warned_unlisted = true;
            }

            const auto summary = summarize_peers(peers, local);
            {
                std::lock_guard<std::mutex> lock(mutex);
                state.peer_taking_over = summary.taking_over;
                state.peer_active = summary.active;
                if (mine)
                    state.owns = mine;
            }
            publish(delivered ? AirPodsStatus::Live : AirPodsStatus::Connecting, "");
            return summary;
        }

        // Registers every other host in the buds' list, once each per session. A host the
        // firmware has not been told about stays unvalidated, and an unvalidated host is the
        // one it evicts when the pods move while another host is engaged.
        bool register_hosts(int fd, const std::vector<AapPeer>& peers, std::vector<std::string>& sent) {
            std::string local;
            bool playing = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                local = local_address;
                playing = streaming;
            }
            if (local.empty())
                return true;
            for (const auto& peer : peers) {
                if (peer.address == local || std::find(sent.begin(), sent.end(), peer.address) != sent.end())
                    continue;
                sent.push_back(peer.address);
                for (const auto& packet :
                     {smart_routing_media_info(local, peer.address, playing), tipi_add_device(local, peer.address)}) {
                    if (packet.empty())
                        continue;
                    if (!write_all(fd, packet.data(), packet.size())) {
                        debug::log(DEBUG, "airpods: host registration write failed: {}", std::strerror(errno));
                        return false;
                    }
                }
                debug::log(DEBUG, "airpods: registered host {} with the buds", peer.address);
            }
            return true;
        }

        // Takes or gives up ownership, on the worker thread. A claim sent while a peer
        // is taking the buds is the one packet that reliably kills the link, so the
        // peer state from the last connected-devices notification gates it. An idle
        // iPhone is not that: claiming out from under one is the whole point.
        bool send_ownership(int fd, bool own) {
            if (!own) {
                if (write_all(fd, RELEASE, sizeof(RELEASE))) {
                    debug::log(DEBUG, "airpods: release sent");
                    return true;
                }
                debug::log(DEBUG, "airpods: release write failed: {}", std::strerror(errno));
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (state.peer_taking_over) {
                    debug::log(DEBUG, "airpods: not claiming, a peer is taking the buds");
                    return true;
                }
            }
            if (write_all(fd, CLAIM, sizeof(CLAIM))) {
                debug::log(DEBUG, "airpods: claim sent");
                std::lock_guard<std::mutex> lock(mutex);
                config_resend_due = true;
                return true;
            }
            debug::log(DEBUG, "airpods: claim write failed: {}", std::strerror(errno));
            return false;
        }

        // Sends whatever the caller queued, on the worker thread. False means the
        // write failed and the session is over.
        bool send_pending(int fd, const std::vector<AapPeer>& hosts) {
            std::optional<AncMode> mode;
            std::optional<bool> ownership;
            bool take_over = false;
            bool playing = false;
            std::optional<bool> told;
            std::string local;
            {
                std::lock_guard<std::mutex> lock(mutex);
                std::swap(mode, pending_anc);
                std::swap(ownership, pending_ownership);
                std::swap(take_over, pending_take_over);
                playing = streaming;
                told = streaming_sent;
                local = local_address;
            }
            if (ownership && !send_ownership(fd, *ownership))
                return false;

            // Every other host hears this one's playback state, and on a take-over is asked to let go.
            const auto to_hosts = [&](const std::function<std::vector<uint8_t>(const std::string&)>& build) {
                for (const auto& host : hosts) {
                    if (host.address == local)
                        continue;
                    const auto packet = build(host.address);
                    if (!packet.empty() && !write_all(fd, packet.data(), packet.size())) {
                        debug::log(DEBUG, "airpods: smart routing write failed: {}", std::strerror(errno));
                        return false;
                    }
                }
                return true;
            };
            if (take_over && !send_ownership(fd, true))
                return false;
            // A take-over is for playback, whether or not the player has resumed yet.
            const bool report = take_over || playing;
            if (!local.empty() && told != report) {
                if (!to_hosts([&](const std::string& host) { return smart_routing_media_info(local, host, report); }))
                    return false;
                debug::log(DEBUG, "airpods: told the other hosts {} is playing here", report ? "something" : "nothing");
                std::lock_guard<std::mutex> lock(mutex);
                streaming_sent = report;
            }
            if (take_over) {
                if (!to_hosts([](const std::string& host) { return smart_routing_hijack(host); }))
                    return false;
                debug::log(DEBUG, "airpods: took the buds over from the other hosts");
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

        // Runs one channel from connect to close. Sets `delivered` when the buds sent
        // any notification, which is what separates a contended channel from a
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
                    stage = Stage::StemConfig;
                    due = std::max(opened + std::chrono::milliseconds(STEM_CONFIG_AT_MS), now);
                    return true;
                case Stage::StemConfig:
                    if (!write_all(fd, STEM_CONFIG, sizeof(STEM_CONFIG)))
                        return false;
                    debug::log(DEBUG, "airpods: stem config sent");
                    stage = Stage::Ready;
                    return true;
                case Stage::Ready:
                    return true;
                }
                return true;
            };

            // AirPods Pro 2 validates a host by itself once the session is up; Pro 3
            // never does, and an unvalidated host is dropped at the first pod movement.
            // So a session claims once the buds' host list proves the channel is live,
            // never into a peer taking them, and only after the audio link has settled.
            bool claim_sent = false;
            std::optional<clock::time_point> claim_at;
            // Ownership taken to validate this host goes back unless something here is playing.
            std::optional<clock::time_point> idle_release_at;
            const auto claim = [&] {
                bool peer = false;
                bool yielding = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    peer = state.peer_taking_over;
                    yielding = yielded;
                }
                if (peer || yielding) {
                    debug::log(DEBUG,
                               "airpods: session claim held back, {}",
                               peer ? "a peer is taking the buds" : "the buds are yielded");
                    return true;
                }
                claim_sent = true;
                if (!send_ownership(fd, true))
                    return false;
                idle_release_at = clock::now() + std::chrono::milliseconds(IDLE_RELEASE_MS);
                return true;
            };

            std::map<std::string, uint8_t> peer_states;
            // Hosts this session has registered with the buds, once each.
            std::vector<std::string> tipi_sent;
            // When the session's notification request is due again after a claim.
            std::optional<clock::time_point> config_at;
            bool warned_unlisted = false;
            std::optional<bool> owns;
            // Another host is attached to the buds, this machine is playing through them.
            bool peer_present = false;
            bool local_audio = false;
            // The buds' host list, and each host's latest media report.
            std::vector<AapPeer> hosts;
            std::map<std::string, SmartRoutingMessage> reports;
            // Both buds out moves the stem and volume target to another host; this machine owned
            // them when they came out, so it claims again when one goes back in.
            bool owned_when_removed = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                streaming_sent.reset();
            }
            std::array<uint8_t, 1024> buffer{};
            while (!retargeted(address)) {
                const auto now = clock::now();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (config_resend_due) {
                        config_resend_due = false;
                        config_at = now + std::chrono::milliseconds(CONFIG_RESEND_MS);
                    }
                }
                // Until the buds send a notification, stray bytes do not keep an attempt alive:
                // a channel another client holds stays open and says nothing useful.
                const auto quiet_since = delivered ? last_packet : opened;
                const int quiet_timeout = (delivered ? IDLE_TIMEOUT_MS : FIRST_PACKET_TIMEOUT_MS);
                const auto until = [&](clock::time_point at) {
                    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(at - now).count());
                };
                int timeout = until(quiet_since + std::chrono::milliseconds(quiet_timeout));
                if (stage != Stage::Ready)
                    timeout = std::min(timeout, until(due));
                if (claim_at)
                    timeout = std::min(timeout, until(*claim_at));
                if (idle_release_at)
                    timeout = std::min(timeout, until(*idle_release_at));
                if (config_at)
                    timeout = std::min(timeout, until(*config_at));
                const int ready = wait_for(fd, wake_fd, POLLIN, std::max(timeout, 0));
                if (ready == 0) {
                    drain_wake();
                    if (!send_pending(fd, hosts))
                        break;
                    continue;
                }
                if (ready == -2)
                    break;
                if (ready < 0) {
                    const auto fired = clock::now();
                    if (stage != Stage::Ready && fired >= due) {
                        if (!advance(fired))
                            break;
                        continue;
                    }
                    // The firmware drops what a session set up before a handoff, so the
                    // notification request and stem config go out again after taking the buds back.
                    if (config_at && fired >= *config_at) {
                        config_at.reset();
                        if (!write_all(fd, REQUEST_NOTIFICATIONS, sizeof(REQUEST_NOTIFICATIONS)) ||
                            !write_all(fd, STEM_CONFIG, sizeof(STEM_CONFIG)))
                            break;
                        debug::log(DEBUG, "airpods: notification request and stem config re-sent after claiming");
                        continue;
                    }
                    if (idle_release_at && fired >= *idle_release_at) {
                        idle_release_at.reset();
                        bool yielding = false;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            yielding = yielded;
                        }
                        if (releases_when_idle(local_audio, peer_present, yielding)) {
                            debug::log(DEBUG, "airpods: nothing playing here, handing ownership back");
                            if (!send_ownership(fd, false))
                                break;
                            owns = false;
                        }
                        continue;
                    }
                    if (claim_at && fired >= *claim_at) {
                        claim_at.reset();
                        if (!claim_sent && !claim())
                            break;
                        continue;
                    }
                    if (fired - quiet_since >= std::chrono::milliseconds(quiet_timeout)) {
                        if (!delivered)
                            debug::log(DEBUG, "airpods: no notification within {}ms", FIRST_PACKET_TIMEOUT_MS);
                        break;
                    }
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
                    delivered = true;
                    owns = buffer[sizeof(OWNS_CONNECTION_PREFIX)] == 0x01;
                    debug::log(DEBUG, "airpods: ownership verdict {:#04x}", buffer[sizeof(OWNS_CONNECTION_PREFIX)]);
                    continue;
                }

                // Ownership runs on these two; the raw bytes settle any doubt about the layout.
                if (starts_with(buffer.data(), size, CONNECTED_DEVICES_PREFIX, sizeof(CONNECTED_DEVICES_PREFIX)) ||
                    starts_with(buffer.data(), size, AUDIO_SOURCE_PREFIX, sizeof(AUDIO_SOURCE_PREFIX)))
                    debug::log(DEBUG, "airpods: rx {}", hex(buffer.data(), size));

                if (auto peers = parse_connected_devices(buffer.data(), size)) {
                    delivered = true;
                    peer_present = note_peers(*peers, peer_states, warned_unlisted, delivered).present;
                    hosts = *peers;
                    if (!register_hosts(fd, *peers, tipi_sent))
                        break;
                    // The host list proves the channel is ours; the claim waits for the audio link.
                    if (!claim_sent && !claim_at)
                        claim_at = std::max(last_packet, opened + std::chrono::milliseconds(CLAIM_SETTLE_MS));
                    continue;
                }

                if (auto source = parse_audio_source(buffer.data(), size)) {
                    delivered = true;
                    debug::log(DEBUG,
                               "airpods: audio source {} on {}",
                               source->source == AudioSource::Call    ? "call"
                               : source->source == AudioSource::Media ? "media"
                                                                      : "none",
                               source->address);
                    std::string local;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        local = local_address;
                    }
                    if (!local.empty() && source->address == local &&
                        local_audio != (source->source != AudioSource::None)) {
                        local_audio = source->source != AudioSource::None;
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            state.local_audio = local_audio;
                        }
                        publish(delivered ? AirPodsStatus::Live : AirPodsStatus::Connecting, "");
                    }
                    continue;
                }

                if (auto message = parse_smart_routing(buffer.data(), size)) {
                    delivered = true;
                    debug::log(DEBUG,
                               "airpods: smart routing from {}: app {}, streaming {}, category {}{}",
                               message->sender,
                               message->app.empty() ? "-" : message->app,
                               message->streaming ? (*message->streaming ? "yes" : "no") : "-",
                               message->category ? std::to_string(*message->category) : "-",
                               message->set_ownership_to_false ? ", set ownership to false" : "");
                    if (message->streaming)
                        reports[message->sender] = *message;

                    std::string local;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        local = local_address;
                    }
                    bool audio = false;
                    bool call = false;
                    for (const auto& [host, report] : reports) {
                        if (host == local)
                            continue;
                        audio = audio || report.playing();
                        call = call || report.call();
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.peer_audio = audio;
                        state.peer_call = call;
                    }

                    // The other host has taken the buds and says so; this one lets go without waiting
                    // for the buds' own verdict.
                    if (message->set_ownership_to_false && message->sender != local) {
                        if (!send_ownership(fd, false))
                            break;
                        owns = false;
                        std::lock_guard<std::mutex> lock(mutex);
                        state.owns = false;
                        yielded = true;
                    }
                    publish(delivered ? AirPodsStatus::Live : AirPodsStatus::Connecting, "");
                    continue;
                }

                if (auto mode = parse_anc(buffer.data(), size)) {
                    delivered = true;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        state.anc = *mode;
                    }
                    publish(delivered ? AirPodsStatus::Live : AirPodsStatus::Connecting, "");
                    continue;
                }

                if (auto ear = parse_ear(buffer.data(), size)) {
                    delivered = true;
                    bool reclaim = false;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (state.ear.in_ear() > 0 && ear->in_ear() == 0) {
                            owned_when_removed = state.owns.value_or(false);
                        } else if (state.ear.in_ear() == 0 && ear->in_ear() > 0 && owned_when_removed) {
                            owned_when_removed = false;
                            reclaim = !yielded;
                        }
                        state.ear = *ear;
                    }
                    if (reclaim) {
                        debug::log(DEBUG, "airpods: a bud went back in after both were out; claiming again");
                        if (!send_ownership(fd, true))
                            break;
                    }
                    publish(delivered ? AirPodsStatus::Live : AirPodsStatus::Connecting, "");
                    continue;
                }

                if (auto press = parse_stem_press(buffer.data(), size)) {
                    delivered = true;
                    debug::log(DEBUG, "airpods: stem press {:#04x}", buffer[6]);
                    if (on_stem_press)
                        on_stem_press(*press);
                    continue;
                }

                auto update = parse_battery(buffer.data(), size);
                if (!update) {
                    // A smart-routing message that did not decode is shown as text: its keys are ASCII.
                    if (starts_with(
                            buffer.data(), size, SMART_ROUTING_RESPONSE_PREFIX, sizeof(SMART_ROUTING_RESPONSE_PREFIX)))
                        debug::log(DEBUG, "airpods: smart routing not decoded: {}", printable(buffer.data(), size));
                    debug::log(DEBUG, "airpods: rx unhandled {}", hex(buffer.data(), size));
                    continue;
                }

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
                pending_take_over = false;
                state.peer_audio = false;
                state.peer_call = false;
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
                        state.peer_audio = false;
                        state.peer_call = false;
                        state.local_audio = false;
                        state.owns.reset();
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
                        state.peer_audio = false;
                        state.peer_call = false;
                        state.local_audio = false;
                        state.owns.reset();
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

    AirPodsWatcher::AirPodsWatcher(std::function<void(const AirPodsState&)> on_change,
                                   std::function<void(StemPress)> on_stem_press)
        : impl_(std::make_unique<Impl>(std::move(on_change))) {
        impl_->on_stem_press = std::move(on_stem_press);
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
            impl_->yielded = !own;
        }
        impl_->wake();
    }

    void AirPodsWatcher::take_over() {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->pending_take_over = true;
            impl_->yielded = false;
        }
        impl_->wake();
    }

    void AirPodsWatcher::set_streaming(bool streaming) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (impl_->streaming == streaming)
                return;
            impl_->streaming = streaming;
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
