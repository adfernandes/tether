#pragma once

#include "tether/bluetooth/objects.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace tether::bluetooth {

    // Apple's Accessory Protocol, the same channel an iPhone uses.
    inline constexpr uint16_t AAP_PSM = 0x1001;

    // Components in a battery notification.
    inline constexpr uint8_t AAP_COMPONENT_RIGHT = 0x02;
    inline constexpr uint8_t AAP_COMPONENT_LEFT = 0x04;
    inline constexpr uint8_t AAP_COMPONENT_CASE = 0x08;
    // Status byte for a component that is not reporting: a bud in the case, or a shut case.
    inline constexpr uint8_t AAP_STATUS_DISCONNECTED = 0x04;

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
        AirPodsBattery battery;
        AirPodsStatus status = AirPodsStatus::Idle;
        // Written for display, shown verbatim.
        std::string reason;

        bool operator==(const AirPodsState&) const = default;
    };

    nlohmann::json to_json(const AirPodsState& state);

    // Decodes an AAP battery notification:
    //   04 00 04 00 04 00 [count] ([component] 01 [level] [status] 01) * count
    // Returns no value for anything that is not one, including a truncated packet.
    std::optional<BatteryUpdate> parse_battery(const uint8_t* data, size_t len);

    void merge_battery(AirPodsBattery& into, const BatteryUpdate& update);

    // The connected AirPods worth opening a channel to, or null. Only one is
    // returned: the channel is single-client and so is the watcher.
    const Device* find_airpods(const BluezObjects& objects);

    // Keeps an AAP channel open to one set of AirPods and publishes their battery.
    class AirPodsWatcher {
    public:
        explicit AirPodsWatcher(std::function<void(const AirPodsState&)> on_change);
        ~AirPodsWatcher();

        AirPodsWatcher(const AirPodsWatcher&) = delete;
        AirPodsWatcher& operator=(const AirPodsWatcher&) = delete;

        // Points the watcher at a connected device, or stops it when `address` is
        // empty. Cheap to call on every BlueZ snapshot: an unchanged target is a no-op.
        void set_device(const std::string& address, const std::string& name);

        AirPodsState state() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    extern AirPodsWatcher* g_airpods;

} // namespace tether::bluetooth
