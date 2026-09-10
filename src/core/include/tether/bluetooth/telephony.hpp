#pragma once

#include "tether/bluetooth/objects.hpp"

#include <gio/gio.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

// Call control over Bluetooth Hands-Free, in the HF role: this machine is the
// car kit and the iPhone is the audio gateway. Two stacks can own that profile.
// BlueZ's own hands-free profile and PipeWire's, so the phone is reached
// through whichever one has it, and only the bus addressing differs.
namespace tether::bluetooth {

    class BluezMonitor;

    // Empty when the input cannot be dialled.
    std::string normalize_dial_string(const std::string& input);

    nlohmann::json to_json(const Call& call);
    nlohmann::json to_json(const std::vector<Call>& calls);
    nlohmann::json to_json(const Telephony& gateway);

    struct TelephonyIds {
        // as in the status payload.
        const char* name;
        const char* bus;
        const char* gateway_iface;
        // null if stack has no call audio
        const char* transport_iface;
        const char* call_iface;
        bool dial_uri;
        bool indicators;
    };

    // What a stack's Dial() takes, built from an already normalized number.
    // BlueZ wants a URI, PipeWire formats the bare number into ATD itself, so
    // handing it a URI would put "ATDtel:+15555550123;" on the RFCOMM link.
    std::string dial_argument(const TelephonyIds& ids, const std::string& number);

    struct TelephonySnapshot {
        Telephony gateway;
        std::vector<Call> calls;
        std::string audio_state;
    };

    // A stack that can serve the configured phone's calls.
    class TelephonySource {
    public:
        virtual ~TelephonySource() = default;

        virtual TelephonyIds ids() const = 0;
        // Null while the stack is unreachable.
        virtual GDBusConnection* connection() const = 0;
        // An empty gateway path means this stack is not serving the address.
        virtual TelephonySnapshot snapshot() const = 0;
    };

    // The iPhone's audio gateway. Sources are consulted in order and the first with a gateway wins.
    class TelephonyClient {
    public:
        TelephonyClient(std::vector<std::unique_ptr<TelephonySource>> sources, std::string address);

        TelephonyClient(const TelephonyClient&) = delete;
        TelephonyClient& operator=(const TelephonyClient&) = delete;

        // Whether the phone has HFP connected and the gateway is usable.
        bool available() const;

        // Whether any stack is serving the phone, including one still bringing
        // the service level connection up. Presence, not readiness.
        bool present() const;

        nlohmann::json status() const;
        nlohmann::json calls() const;

        // The number is normalized here, so callers may pass what the user typed.
        bool dial(const std::string& number, std::string& err);

        // action: answer, hangup, hangup_all, swap, hold_and_answer,
        // release_and_answer, release_and_swap, multiparty, audio_here,
        // audio_phone. `path` is the call for answer and hangup, and is ignored
        // otherwise.
        bool call_action(const std::string& path, const std::string& action, std::string& err);

        bool send_tones(const std::string& tones, std::string& err);

    private:
        // The serving source and its snapshot or null for none.
        std::pair<TelephonySource*, TelephonySnapshot> pick() const;

        bool invoke(TelephonySource& source,
                    const std::string& path,
                    const char* iface,
                    const char* method,
                    GVariant* args,
                    std::string& err);

        // Moves the call audio between this computer and the iPhone. Fails when the
        // stack holding Hands-Free cannot carry audio at all.
        bool route_audio(TelephonySource& source, const TelephonySnapshot& snap, bool to_phone, std::string& err);

        std::vector<std::unique_ptr<TelephonySource>> sources_;
        std::string address_;
    };

    // bluez's hfp on the system bus. No call audio.
    std::unique_ptr<TelephonySource> make_bluez_source(BluezMonitor& monitor, std::string address);

    // pipewire's, on the session bus. Carries the call audio as well.
    std::unique_ptr<TelephonySource> make_pipewire_source(std::string address);

    // Every source this machine could have, in preference order.
    std::unique_ptr<TelephonyClient> make_telephony_client(BluezMonitor& monitor, std::string address);

    // Decodes a GetManagedObjects payload from org.pipewire.Telephony into the
    // gateway matching `address`.
    TelephonySnapshot parse_pipewire_telephony(GVariant* objects, const std::string& address);

    std::vector<Call> parse_pipewire_calls(GVariant* objects, const std::string& gateway_path);

} // namespace tether::bluetooth
