#include "tether/bluetooth/telephony.hpp"
#include "tether/bluetooth/monitor.hpp"
#include "tether/log.hpp"

#include <tether/i18n.hpp>

#include <algorithm>
#include <cctype>

namespace tether::bluetooth {

    namespace {

        constexpr const char* BLUEZ_NAME = "org.bluez";
        constexpr const char* IFACE_TELEPHONY = "org.bluez.Telephony1";
        constexpr const char* IFACE_CALL = "org.bluez.Call1";
        constexpr const char* IFACE_PROPS = "org.freedesktop.DBus.Properties";

        constexpr int CALL_TIMEOUT_MS = 10000;

        constexpr size_t MAX_DIAL_DIGITS = 32;
        constexpr size_t MAX_TONES = 32;

        bool iequals(const std::string& a, const std::string& b) {
            return a.size() == b.size() &&
                   std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
                       return std::tolower(x) == std::tolower(y);
                   });
        }

    } // namespace

    std::string normalize_dial_string(const std::string& input) {
        std::string out;
        bool seen_digit = false;
        for (const char raw : input) {
            const unsigned char c = static_cast<unsigned char>(raw);
            if (c == '+') {
                // Only as the country-code prefix
                if (!out.empty())
                    return {};
                out.push_back('+');
                continue;
            }
            if (std::isdigit(c)) {
                out.push_back(static_cast<char>(c));
                seen_digit = true;
                continue;
            }
            if (c == ' ' || c == '-' || c == '(' || c == ')' || c == '.' || c == '\t')
                continue;
            return {};
        }
        if (!seen_digit)
            return {};
        const size_t digits = out.size() - (out[0] == '+' ? 1 : 0);
        return digits > MAX_DIAL_DIGITS ? std::string{} : out;
    }

    nlohmann::json to_json(const Call& call) {
        nlohmann::json j;
        j["path"] = call.path;
        j["number"] = call.withheld() ? "" : call.number;
        j["withheld"] = call.withheld();
        j["name"] = call.name;
        j["incoming_line"] = call.incoming_line;
        j["state"] = call.state;
        j["multiparty"] = call.multiparty;
        j["ringing"] = call.ringing();
        j["outgoing"] = call.outgoing();
        j["connected"] = call.connected();
        return j;
    }

    nlohmann::json to_json(const std::vector<Call>& calls) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& call : calls)
            out.push_back(to_json(call));
        return out;
    }

    nlohmann::json to_json(const Telephony& gateway) {
        nlohmann::json j;
        j["available"] = gateway.ready();
        j["path"] = gateway.path;
        j["state"] = gateway.state;
        j["operator"] = gateway.operator_name;
        j["signal"] = gateway.signal;
        j["battery"] = gateway.battery;
        j["service"] = gateway.service;
        j["roaming"] = gateway.roaming;
        j["inband_ringtone"] = gateway.inband_ringtone;
        return j;
    }

    namespace {

        class BluezSource : public TelephonySource {
        public:
            BluezSource(BluezMonitor& monitor, std::string address)
                : monitor_(&monitor), address_(std::move(address)) {}

            TelephonyIds ids() const override {
                return {"bluez", BLUEZ_NAME, IFACE_TELEPHONY, nullptr, IFACE_CALL, true, true};
            }

            GDBusConnection* connection() const override { return monitor_->connection(); }

            TelephonySnapshot snapshot() const override {
                const BluezObjects objects = monitor_->snapshot();
                for (const auto& device : objects.devices) {
                    if (!iequals(device.address, address_))
                        continue;
                    if (const Telephony* found = objects.find_telephony(device.path))
                        return {*found, objects.calls_for(found->path), {}};
                }
                return {};
            }

        private:
            BluezMonitor* monitor_;
            std::string address_;
        };

    } // namespace

    std::string dial_argument(const TelephonyIds& ids, const std::string& number) {
        return ids.dial_uri ? "tel:" + number : number;
    }

    std::unique_ptr<TelephonySource> make_bluez_source(BluezMonitor& monitor, std::string address) {
        return std::make_unique<BluezSource>(monitor, std::move(address));
    }

    std::unique_ptr<TelephonyClient> make_telephony_client(BluezMonitor& monitor, std::string address) {
        std::vector<std::unique_ptr<TelephonySource>> sources;
        sources.push_back(make_bluez_source(monitor, address));
        sources.push_back(make_pipewire_source(address));
        return std::make_unique<TelephonyClient>(std::move(sources), std::move(address));
    }

    TelephonyClient::TelephonyClient(std::vector<std::unique_ptr<TelephonySource>> sources, std::string address)
        : sources_(std::move(sources)), address_(std::move(address)) {}

    std::pair<TelephonySource*, TelephonySnapshot> TelephonyClient::pick() const {
        for (const auto& source : sources_) {
            TelephonySnapshot snap = source->snapshot();
            if (!snap.gateway.path.empty())
                return {source.get(), std::move(snap)};
        }
        return {nullptr, {}};
    }

    bool TelephonyClient::available() const { return pick().second.gateway.ready(); }

    bool TelephonyClient::present() const { return pick().first != nullptr; }

    nlohmann::json TelephonyClient::status() const {
        const auto [source, snap] = pick();
        nlohmann::json j = to_json(snap.gateway);
        j["address"] = address_;
        j["calls"] = snap.calls.size();
        j["backend"] = source ? source->ids().name : "";
        j["indicators"] = !source || source->ids().indicators;
        j["audio"] = snap.audio_state;
        if (!source)
            j["reason"] = _("The iPhone has not connected Hands-Free to this computer.");
        else if (!snap.gateway.ready())
            j["reason"] = _("Connecting to the iPhone's Hands-Free service.");
        else if (source->ids().transport_iface)
            j["reason"] = _("PipeWire is handling Hands-Free, so the call audio can play on this computer.");
        else
            j["reason"] = _("Calls are controlled here; the audio plays on the iPhone.");
        return j;
    }

    nlohmann::json TelephonyClient::calls() const { return to_json(pick().second.calls); }

    bool TelephonyClient::invoke(TelephonySource& source,
                                 const std::string& path,
                                 const char* iface,
                                 const char* method,
                                 GVariant* args,
                                 std::string& err) {
        GDBusConnection* conn = source.connection();
        if (!conn || path.empty()) {
            // Consumes the floating reference the caller built.
            if (args)
                g_variant_unref(g_variant_ref_sink(args));
            err = _("The iPhone has not connected Hands-Free to this computer.");
            return false;
        }

        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_sync(conn,
                                                      source.ids().bus,
                                                      path.c_str(),
                                                      iface,
                                                      method,
                                                      args,
                                                      nullptr,
                                                      G_DBUS_CALL_FLAGS_NONE,
                                                      CALL_TIMEOUT_MS,
                                                      nullptr,
                                                      &error);
        if (!reply) {
            err = error ? error->message : "unknown error";
            g_clear_error(&error);
            debug::log(WARN, "telephony: {} failed: {}", method, err);
            return false;
        }
        g_variant_unref(reply);
        return true;
    }

    bool TelephonyClient::route_audio(TelephonySource& source,
                                      const TelephonySnapshot& snap,
                                      bool to_phone,
                                      std::string& err) {
        const char* transport = source.ids().transport_iface;
        if (!transport) {
            err = _("This computer is not carrying the call audio; it plays on the iPhone.");
            return false;
        }
        if (!invoke(source,
                    snap.gateway.path,
                    IFACE_PROPS,
                    "Set",
                    g_variant_new("(ssv)", transport, "RejectSCO", g_variant_new_boolean(to_phone)),
                    err))
            return false;
        if (to_phone)
            return true;

        std::string activate_err;
        if (!invoke(source, snap.gateway.path, transport, "Activate", nullptr, activate_err))
            debug::log(INFO, "telephony: no call audio to take yet: {}", activate_err);
        return true;
    }

    bool TelephonyClient::dial(const std::string& number, std::string& err) {
        const std::string dialable = normalize_dial_string(number);
        if (dialable.empty()) {
            err = _("Not a dialable number.");
            return false;
        }
        const auto [source, snap] = pick();
        if (!source) {
            err = _("The iPhone has not connected Hands-Free to this computer.");
            return false;
        }
        const std::string argument = dial_argument(source->ids(), dialable);
        return invoke(*source,
                      snap.gateway.path,
                      source->ids().gateway_iface,
                      "Dial",
                      g_variant_new("(s)", argument.c_str()),
                      err);
    }

    bool TelephonyClient::call_action(const std::string& path, const std::string& action, std::string& err) {
        const auto [source, snap] = pick();
        if (!source) {
            err = _("The iPhone has not connected Hands-Free to this computer.");
            return false;
        }
        const std::vector<Call>& live = snap.calls;

        if (action == "answer" || action == "hangup") {
            std::string target = path;
            if (target.empty() && live.size() == 1)
                target = live.front().path;
            if (target.empty()) {
                err = _("No call named.");
                return false;
            }
            if (std::none_of(live.begin(), live.end(), [&](const Call& c) { return c.path == target; })) {
                err = _("That call is no longer active.");
                return false;
            }
            return invoke(
                *source, target, source->ids().call_iface, action == "answer" ? "Answer" : "Hangup", nullptr, err);
        }

        if (action == "audio_here" || action == "audio_phone")
            return route_audio(*source, snap, action == "audio_phone", err);

        static const std::pair<const char*, const char*> gateway_actions[] = {
            {"hangup_all", "HangupAll"},
            {"swap", "SwapCalls"},
            {"hold_and_answer", "HoldAndAnswer"},
            {"release_and_answer", "ReleaseAndAnswer"},
            {"release_and_swap", "ReleaseAndSwap"},
            {"multiparty", "CreateMultiparty"},
        };
        for (const auto& [name, method] : gateway_actions)
            if (action == name)
                return invoke(*source, snap.gateway.path, source->ids().gateway_iface, method, nullptr, err);

        err = _("Unknown call action.");
        return false;
    }

    bool TelephonyClient::send_tones(const std::string& tones, std::string& err) {
        if (tones.empty() || tones.size() > MAX_TONES || !std::all_of(tones.begin(), tones.end(), [](unsigned char c) {
                return std::isdigit(c) || c == '*' || c == '#' || (c >= 'A' && c <= 'D');
            })) {
            err = _("Not a DTMF sequence.");
            return false;
        }
        const auto [source, snap] = pick();
        if (!source) {
            err = _("The iPhone has not connected Hands-Free to this computer.");
            return false;
        }
        return invoke(*source,
                      snap.gateway.path,
                      source->ids().gateway_iface,
                      "SendTones",
                      g_variant_new("(s)", tones.c_str()),
                      err);
    }

} // namespace tether::bluetooth
