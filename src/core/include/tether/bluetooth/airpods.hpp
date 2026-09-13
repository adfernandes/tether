#pragma once

#include "tether/bluetooth/config.hpp"
#include "tether/bluetooth/objects.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace tether::bluetooth {

    // Apple's Accessory Protocol, the same channel an iPhone uses.
    inline constexpr uint16_t AAP_PSM = 0x1001;

    // Components in a battery notification.
    inline constexpr uint8_t AAP_COMPONENT_RIGHT = 0x02;
    inline constexpr uint8_t AAP_COMPONENT_LEFT = 0x04;
    inline constexpr uint8_t AAP_COMPONENT_CASE = 0x08;
    // Status byte for a component that is not reporting: a bud in the case, or a shut case.
    inline constexpr uint8_t AAP_STATUS_DISCONNECTED = 0x04;

    // Listening mode. The values are the wire bytes, which are one above the
    // order Apple's own UI lists them in.
    enum class AncMode : uint8_t {
        Off = 0x01,
        NoiseCancellation = 0x02,
        Transparency = 0x03,
        Adaptive = 0x04,
    };

    const char* to_string(AncMode mode);
    std::optional<AncMode> anc_mode_from_string(const std::string& name);

    // Ownership state of one host attached to the buds, the `B` field of a connected-devices
    // notification. **Bit 0x02 is the owner**.
    inline constexpr uint8_t AAP_PEER_OWNER = 0x02;
    inline constexpr uint8_t AAP_PEER_ENGAGED = 0x10;
    // Connected and idle. An iPhone sits here for as long as it is paired and doing
    // nothing, so this one value is not a peer taking the buds.
    inline constexpr uint8_t AAP_PEER_PASSIVE = 0x15;

    // One host in a connected-devices notification.
    struct AapPeer {
        std::string address;
        // Role and generation counter. Zero for our own address means the firmware
        // evicted us, but it also occurs when the buds swap primary and secondary.
        uint8_t role = 0;
        uint8_t state = 0;

        bool active() const { return (state & AAP_PEER_OWNER) != 0; }
        // Escalating towards taking the buds, on the models that report 0x10 and up. Claiming
        // against this leaves the host contested and the firmware closes the link. Owning never
        // counts: an owner that has finished is what a reclaim claims from, and some models keep
        // an owning iPhone at 0x17 for as long as it holds the buds.
        bool taking_over() const { return state >= AAP_PEER_ENGAGED && state != AAP_PEER_PASSIVE && !active(); }

        bool operator==(const AapPeer&) const = default;
    };

    // What the buds are carrying for a host.
    enum class AudioSource { None, Call, Media };

    struct AudioSourceEvent {
        std::string address;
        AudioSource source = AudioSource::None;

        bool operator==(const AudioSourceEvent&) const = default;
    };

    // Where a bud is.
    enum class EarStatus { Unknown, InEar, OutOfEar, InCase };

    const char* to_string(EarStatus status);

    struct EarState {
        EarStatus primary = EarStatus::Unknown;
        EarStatus secondary = EarStatus::Unknown;

        int in_ear() const { return (primary == EarStatus::InEar ? 1 : 0) + (secondary == EarStatus::InEar ? 1 : 0); }
        bool known() const { return primary != EarStatus::Unknown || secondary != EarStatus::Unknown; }

        bool operator==(const EarState&) const = default;
    };

    struct AirPodsBattery {
        // -1 when unknown.
        int left = -1;
        int right = -1;
        int case_ = -1;

        bool any() const { return left >= 0 || right >= 0 || case_ >= 0; }

        bool operator==(const AirPodsBattery&) const = default;
    };

    // One notification has only the components that changed.
    struct BatteryUpdate {
        AirPodsBattery levels;
        bool has_left = false;
        bool has_right = false;
        bool has_case = false;

        bool operator==(const BatteryUpdate&) const = default;
    };

    enum class AirPodsStatus {
        // No AirPods connected to this machine.
        Idle,
        // The channel is opening, or open with no battery packet yet.
        Connecting,
        Live,
        // The channel is single-client and something else holds it.
        Busy,
        Failed,
    };

    const char* to_string(AirPodsStatus status);

    struct AirPodsState {
        std::string address;
        std::string name;
        bool peer_taking_over = false;
        bool peer_active = false;
        // Another host reports a call or media playing, from its own smart-routing reports.
        bool peer_audio = false;
        bool peer_call = false;
        bool local_audio = false;
        std::optional<bool> owns;
        AirPodsBattery battery;
        // Unset until the buds report one. Not every model has the feature.
        std::optional<AncMode> anc;
        EarState ear;
        AirPodsStatus status = AirPodsStatus::Idle;
        // Written for display, shown verbatim.
        std::string reason;

        // What handoff yields to: a peer on a call, which always takes the buds, or a peer that owns
        // them and is playing through them. Ownership alone is not enough, because the phone keeps
        // it until another host claims.
        bool peer_busy() const { return peer_call || (peer_active && peer_audio); }

        bool operator==(const AirPodsState&) const = default;
    };

    nlohmann::json to_json(const AirPodsState& state);

    // Decodes an AAP battery notification:
    //   04 00 04 00 04 00 [count] ([component] 01 [level] [status] 01) * count
    // Returns no value for anything that is not one, including a truncated packet.
    std::optional<BatteryUpdate> parse_battery(const uint8_t* data, size_t len);

    void merge_battery(AirPodsBattery& into, const BatteryUpdate& update);

    // Decodes an ear-detection notification, 8 bytes carrying the primary bud at
    // offset 6 and the secondary at 7.
    std::optional<EarState> parse_ear(const uint8_t* data, size_t len);

    // Decodes a connected-devices notification:
    //   04 00 04 00 2E 00 [?] [?] [count] ([mac] * 6 [role] [state]) * count
    // The MAC is in display order here and least-significant byte first in the
    // audio-source notification; both come back in BlueZ's own text order.
    std::optional<std::vector<AapPeer>> parse_connected_devices(const uint8_t* data, size_t len);

    // Decodes an audio-source notification, 13 bytes: prefix, MAC, then the type.
    std::optional<AudioSourceEvent> parse_audio_source(const uint8_t* data, size_t len);

    // Whether a session that has claimed the buds to validate itself should hand ownership straight back.
    bool releases_when_idle(bool local_audio, bool peer_present, bool yielded);

    // `otherDeviceAudioCategory` in a host's media report, as an iPhone sends them.
    inline constexpr int AUDIO_CATEGORY_NONE = 100;
    inline constexpr int AUDIO_CATEGORY_MEDIA = 301;
    inline constexpr int AUDIO_CATEGORY_CALL = 501;

    // A smart-routing message another host sent this one through the buds:
    //   04 00 04 00 11 00 [sender] * 6 [length] * 2 01 [OPACK dictionary]
    // with the sender least-significant byte first. Hosts report what they are playing whenever it
    // changes, and ask the others to give up ownership when they take the buds.
    struct SmartRoutingMessage {
        std::string sender;
        std::string app;
        // Unset in messages that are not media reports.
        std::optional<bool> streaming;
        std::optional<int> category;
        bool set_ownership_to_false = false;

        // A call or media is actually playing; a route with nothing on it reports YES with category 100.
        bool playing() const { return streaming.value_or(false) && category.value_or(0) > AUDIO_CATEGORY_NONE; }
        bool call() const { return streaming.value_or(false) && category == AUDIO_CATEGORY_CALL; }
    };

    // Returns no value for anything that is not one, or whose body does not decode.
    std::optional<SmartRoutingMessage> parse_smart_routing(const uint8_t* data, size_t len);

    // Smart-routing messages this host sends to `target`, opcode 0x10. Empty for an address that will
    // not parse. The first two are what a host sends for every other host in the buds' list, which
    // is what moves it from unvalidated to validated.
    std::vector<uint8_t> tipi_add_device(const std::string& self, const std::string& target);
    std::vector<uint8_t> smart_routing_media_info(const std::string& self, const std::string& target, bool streaming);
    // Tells `target` this host has taken the buds and it should give up ownership.
    std::vector<uint8_t> smart_routing_hijack(const std::string& target);

    // Whether any host other than `local` is engaged with the buds, or actively
    // holding them. A machine's own entry is never a peer.
    struct PeerSummary {
        bool taking_over = false;
        bool active = false;
        bool present = false;

        bool operator==(const PeerSummary&) const = default;
    };

    PeerSummary summarize_peers(const std::vector<AapPeer>& peers, const std::string& local);

    enum class StemPress { Single, Double, Triple, Long };

    // Decodes a stem-press notification, `04 00 04 00 19 00 [type] [bud]`. The buds send one
    // only for the press types the host claimed with the stem config.
    std::optional<StemPress> parse_stem_press(const uint8_t* data, size_t len);

    // Decodes a listening-mode notification, 11 bytes carrying the mode at offset 7.
    // Returns no value for anything else, including an out-of-range mode.
    std::optional<AncMode> parse_anc(const uint8_t* data, size_t len);

    // What an ear-state change should do to local playback.
    enum class MediaAction { None, Pause, Resume };

    // `holding` is whether the caller has a pause outstanding.
    MediaAction ear_media_action(const EarState& before, const EarState& after, PauseMode mode, bool holding);

    // What an iPhone call should do to AirPods that are connected to this machine.
    enum class HandoffAction { None, Release, Reclaim };

    // What the buds' own view of their hosts should do to local audio, for the
    // native handoff: the phone taking them is a peer going active, not a call.
    HandoffAction ownership_action(bool peer_active, bool released, bool enabled);

    // Whether a player starting here takes the buds: not while this machine owns them, never from a
    // call, and not while a handoff is already moving them.
    bool takes_over_for_play(std::optional<bool> owns, bool peer_call, bool busy);

    // `released` is whether this code is what disconnected them: buds the user
    // took away by hand are never reclaimed. A reclaim outlives `enabled` going
    // false, so turning the feature off mid-call does not strand them.
    HandoffAction handoff_action(bool call_active, bool buds_on_linux, bool released, bool enabled);

    // Whether a bt_calls payload describes a call worth handing the buds over for.
    bool call_wants_audio(const nlohmann::json& calls);

    // The connected AirPods worth opening a channel to, or null. Only one is
    // returned: the channel is single-client and so is the watcher.
    const Device* find_airpods(const BluezObjects& objects);

    // Keeps an AAP channel open to one set of AirPods and publishes their battery.
    class AirPodsWatcher {
    public:
        // `on_stem_press` runs on the worker thread for each claimed press.
        explicit AirPodsWatcher(std::function<void(const AirPodsState&)> on_change,
                                std::function<void(StemPress)> on_stem_press = {});
        ~AirPodsWatcher();

        AirPodsWatcher(const AirPodsWatcher&) = delete;
        AirPodsWatcher& operator=(const AirPodsWatcher&) = delete;

        // Points the watcher at a connected device, or stops it when `address` is
        // empty. Cheap to call on every BlueZ snapshot: an unchanged target is a no-op.
        void set_device(const std::string& address, const std::string& name, const std::string& local = {});

        // Stands the watcher down. The channel takes one client per machine, so
        // this is what lets another AirPods program have it.
        void set_enabled(bool enabled);

        // Asks the buds to switch listening mode. Sent on the open channel, and
        // dropped if the channel closes before it goes out; the buds answer with a
        // notification, which is what actually updates the published state.
        void set_anc(AncMode mode);

        // Takes the buds or gives them up over AAP, which is what Apple's own
        // handoff does. A claim is dropped while a peer is engaged: sending one then
        // leaves this host contested and the firmware closes the link. Giving them up
        // also stops the watcher's own claims until they are taken back.
        void set_ownership(bool own);

        // Takes the buds the way another Apple host does: a claim, then a smart-routing report that
        // something plays here and a request that every other host give up ownership.
        void take_over();

        // Reports to the other hosts whether something plays here. Sent when it changes.
        void set_streaming(bool streaming);

        AirPodsState state() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    extern AirPodsWatcher* g_airpods;

} // namespace tether::bluetooth
