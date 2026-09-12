#include <gtest/gtest.h>
#include <tether/bluetooth/objects.hpp>
#include <tether/bluetooth/telephony.hpp>

using namespace tether::bluetooth;

namespace {

    // Same shape BlueZ returns from GetManagedObjects, parsed from GVariant text
    // so the tests need no bus.
    struct Payload {
        GVariant* v = nullptr;

        explicit Payload(const char* text) {
            GError* error = nullptr;
            v = g_variant_parse(G_VARIANT_TYPE("a{oa{sa{sv}}}"), text, nullptr, nullptr, &error);
            if (!v) {
                ADD_FAILURE() << "bad fixture: " << (error ? error->message : "unknown");
                g_clear_error(&error);
            }
        }
        ~Payload() {
            if (v)
                g_variant_unref(v);
        }
        Payload(const Payload&) = delete;
        Payload& operator=(const Payload&) = delete;
    };

    // Two phones, each with a gateway; one is ringing, the other is on a call.
    // Recorded from bluetoothd 5.87 --experimental against a live iPhone.
    constexpr const char* TWO_GATEWAYS = R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Address': <'02:00:00:00:00:01'>, 'Alias': <'Zack 15 Pro'> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/telephony0': {
        'org.bluez.Telephony1': {
          'UUID': <'0000111f-0000-1000-8000-00805f9b34fb'>,
          'State': <'connected'>,
          'OperatorName': <'AT&T'>,
          'SupportedURISchemes': <['tel']>,
          'Signal': <byte 1>,
          'BattChg': <byte 4>,
          'Service': <true>,
          'Roaming': <false>,
          'InbandRingtone': <false>
        }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/telephony0/call1': {
        'org.bluez.Call1': {
          'LineIdentification': <'+15555550123'>,
          'Name': <''>,
          'IncomingLine': <''>,
          'State': <'incoming'>,
          'Multiparty': <false>
        }
      },
      '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF': {
        'org.bluez.Device1': { 'Address': <'AA:BB:CC:DD:EE:FF'>, 'Alias': <'Other'> }
      },
      '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/telephony0': {
        'org.bluez.Telephony1': { 'State': <'connected'>, 'Signal': <byte 5> }
      },
      '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/telephony0/call1': {
        'org.bluez.Call1': { 'LineIdentification': <'+15555559999'>, 'State': <'active'> }
      }
    })";

} // namespace

TEST(DialString, KeepsDigitsAndLeadingPlus) {
    EXPECT_EQ(normalize_dial_string("+15555550123"), "+15555550123");
    EXPECT_EQ(normalize_dial_string("5555550123"), "5555550123");
    EXPECT_EQ(normalize_dial_string("+1 (555) 555-0123"), "+15555550123");
    EXPECT_EQ(normalize_dial_string("555.555.0123"), "5555550123");
    EXPECT_EQ(normalize_dial_string("\t555 555 0123 "), "5555550123");
}

// The dial string reaches the cellular network. Anything that is not a number is
// refused here rather than passed to BlueZ to judge.
TEST(DialString, RefusesEverythingElse) {
    EXPECT_TRUE(normalize_dial_string("").empty());
    EXPECT_TRUE(normalize_dial_string("+").empty());
    EXPECT_TRUE(normalize_dial_string("   ").empty());
    EXPECT_TRUE(normalize_dial_string("not a number").empty());
    // Supplementary service and AT syntax, the reason this check exists. BlueZ
    // would accept these characters; nothing in the UI asks for them.
    EXPECT_TRUE(normalize_dial_string("*21*5551234#").empty());
    EXPECT_TRUE(normalize_dial_string("555;ATH").empty());
    EXPECT_TRUE(normalize_dial_string("555\r\nATD666").empty());
    EXPECT_TRUE(normalize_dial_string("555,,123").empty());
    // A '+' anywhere but the front is not a country code.
    EXPECT_TRUE(normalize_dial_string("555+123").empty());
    EXPECT_TRUE(normalize_dial_string("++15555550123").empty());
    // Long enough to be a paste accident.
    EXPECT_TRUE(normalize_dial_string(std::string(33, '5')).empty());
    EXPECT_FALSE(normalize_dial_string(std::string(32, '5')).empty());
}

TEST(Telephony, ParsesTheGateway) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);

    const Telephony* gw = objects.find_telephony("/org/bluez/hci0/dev_02_00_00_00_00_01");
    ASSERT_NE(gw, nullptr);
    EXPECT_EQ(gw->path, "/org/bluez/hci0/dev_02_00_00_00_00_01/telephony0");
    EXPECT_EQ(gw->operator_name, "AT&T");
    EXPECT_EQ(gw->signal, 1);
    EXPECT_EQ(gw->battery, 4);
    EXPECT_TRUE(gw->service);
    EXPECT_FALSE(gw->roaming);
    EXPECT_TRUE(gw->ready());
    EXPECT_EQ(gw->uri_schemes, std::vector<std::string>{"tel"});

    EXPECT_EQ(objects.find_telephony("/org/bluez/hci0/dev_11_22_33_44_55_66"), nullptr);
    EXPECT_EQ(objects.find_telephony(""), nullptr);
}

// A second phone's calls must never appear under the selected one.
TEST(Telephony, ParsesOnlyTheSelectedGatewaysCalls) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);

    const auto calls = objects.calls_for("/org/bluez/hci0/dev_02_00_00_00_00_01/telephony0");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].number, "+15555550123");
    EXPECT_EQ(calls[0].state, "incoming");
    EXPECT_TRUE(calls[0].ringing());
    EXPECT_FALSE(calls[0].connected());

    EXPECT_TRUE(objects.calls_for("").empty());
    EXPECT_TRUE(objects.calls_for("/org/bluez/hci0/dev_02_00_00_00_00_01/telephony9").empty());
}

// The gateway path is a prefix of its calls' paths, and BlueZ numbers both per
// device: telephony1 must not collect telephony10's calls.
TEST(Telephony, GatewayPathIsMatchedWholeNotAsAPrefix) {
    Payload payload(R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01/telephony1/call1': {
        'org.bluez.Call1': { 'State': <'active'> }
      },
      '/org/bluez/hci0/dev_02_00_00_00_00_01/telephony10/call1': {
        'org.bluez.Call1': { 'State': <'active'> }
      }
    })");
    const BluezObjects objects = parse_managed_objects(payload.v);
    EXPECT_EQ(objects.calls_for("/org/bluez/hci0/dev_02_00_00_00_00_01/telephony1").size(), 1u);
    EXPECT_EQ(objects.calls_for("/org/bluez/hci0/dev_02_00_00_00_00_01/telephony10").size(), 1u);
}

// The objects exist only under bluetoothd --experimental and only while HFP is
// connected. Their absence is the normal state, not a parse failure.
TEST(Telephony, AbsentWhenHandsFreeIsNotConnected) {
    Payload payload(R"({
      '/org/bluez/hci0/dev_02_00_00_00_00_01': {
        'org.bluez.Device1': { 'Address': <'02:00:00:00:00:01'> }
      }
    })");
    const BluezObjects objects = parse_managed_objects(payload.v);
    EXPECT_TRUE(objects.telephony.empty());
    EXPECT_TRUE(objects.calls.empty());
    EXPECT_EQ(objects.find_telephony("/org/bluez/hci0/dev_02_00_00_00_00_01"), nullptr);
}

// A gateway that is still bringing up its service level connection is not ready
// to take a call.
TEST(Telephony, IsNotReadyBeforeTheServiceLevelConnection) {
    for (const char* state : {"connecting", "slc_connecting", "disconnecting"}) {
        Telephony gw;
        gw.state = state;
        EXPECT_FALSE(gw.ready()) << state;
    }
    Telephony gw;
    gw.state = "connected";
    EXPECT_TRUE(gw.ready());
}

TEST(Telephony, TracksACallThroughItsStates) {
    Call call;
    call.state = "incoming";
    EXPECT_TRUE(call.ringing());

    call.state = "active";
    EXPECT_FALSE(call.ringing());
    EXPECT_TRUE(call.connected());

    call.state = "held";
    EXPECT_TRUE(call.connected());

    call.state = "disconnected";
    EXPECT_FALSE(call.connected());
    EXPECT_FALSE(call.ringing());
    EXPECT_FALSE(call.outgoing());

    // An outgoing call runs dialing then alerting before it connects.
    Call outgoing;
    outgoing.state = "dialing";
    EXPECT_TRUE(outgoing.outgoing());
    outgoing.state = "alerting";
    EXPECT_TRUE(outgoing.outgoing());
    outgoing.state = "active";
    EXPECT_FALSE(outgoing.outgoing());
}

// A state this build has not seen must still reach the UI rather than being
// dropped or guessed at. response_and_hold is one BlueZ can report.
TEST(Telephony, PassesUnknownStatesThrough) {
    Call call;
    call.state = "response_and_hold";
    EXPECT_EQ(call.state, "response_and_hold");
    EXPECT_FALSE(call.ringing());
    EXPECT_FALSE(call.connected());
}

// "withheld" is the network refusing caller ID, not a number. Offering it to
// redial would dial nothing.
TEST(Telephony, WithheldCallerIdIsNotANumber) {
    Call call;
    call.number = "withheld";
    call.state = "incoming";
    EXPECT_TRUE(call.withheld());

    const nlohmann::json j = to_json(call);
    EXPECT_EQ(j["number"], "");
    EXPECT_TRUE(j["withheld"]);
    EXPECT_TRUE(normalize_dial_string(j["number"].get<std::string>()).empty());
}

TEST(Telephony, SerializesCallsForClients) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);
    const nlohmann::json j = to_json(objects.calls_for("/org/bluez/hci0/dev_02_00_00_00_00_01/telephony0"));

    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["number"], "+15555550123");
    EXPECT_EQ(j[0]["state"], "incoming");
    EXPECT_TRUE(j[0]["ringing"]);
    EXPECT_FALSE(j[0]["connected"]);
    EXPECT_TRUE(to_json(std::vector<Call>{}).empty());
}

TEST(Telephony, SerializesTheGatewayForClients) {
    Payload payload(TWO_GATEWAYS);
    const BluezObjects objects = parse_managed_objects(payload.v);
    const nlohmann::json j = to_json(*objects.find_telephony("/org/bluez/hci0/dev_02_00_00_00_00_01"));

    EXPECT_TRUE(j["available"]);
    EXPECT_EQ(j["operator"], "AT&T");
    EXPECT_EQ(j["signal"], 1);
    EXPECT_EQ(j["battery"], 4);
    EXPECT_FALSE(j["roaming"]);
}

namespace {

    // Recorded from pipewire 1.6.8 during a live call. The root manager reports
    // the gateways alone -- no calls, even with one active.
    constexpr const char* PIPEWIRE_ROOT = R"({
      '/org/pipewire/Telephony/ag1': {
        'org.pipewire.Telephony.AudioGateway1': {
          'Address': <'02:00:00:00:00:01'>,
          'SpeakerVolume': <byte 15>,
          'MicrophoneVolume': <byte 15>
        },
        'org.pipewire.Telephony.AudioGatewayTransport1': {
          'Codec': <byte 2>, 'State': <'active'>, 'RejectSCO': <false>
        }
      },
      '/org/pipewire/Telephony/ag2': {
        'org.pipewire.Telephony.AudioGateway1': { 'Address': <'AA:BB:CC:DD:EE:FF'> }
      }
    })";

    // ...and each gateway keeps its calls in an object manager of its own, whose
    // reply carries no gateway at all. Recorded from the same call.
    constexpr const char* PIPEWIRE_GATEWAY_CALLS = R"({
      '/org/pipewire/Telephony/ag1/call1': {
        'org.pipewire.Telephony.Call1': {
          'LineIdentification': <'18002662278'>,
          'IncomingLine': <''>,
          'Name': <''>,
          'Multiparty': <false>,
          'State': <'active'>
        }
      },
      '/org/pipewire/Telephony/ag2/call1': {
        'org.pipewire.Telephony.Call1': { 'LineIdentification': <'+15555559999'>, 'State': <'incoming'> }
      }
    })";

    // A source under the test's control, so TelephonyClient can be driven with
    // no bus at all.
    class FakeSource : public TelephonySource {
    public:
        TelephonySnapshot snap;
        TelephonyIds identity{"fake", "org.example", "org.example.Gateway1", nullptr, "org.example.Call1", true, true};

        TelephonyIds ids() const override { return identity; }
        // Null stands for "reachable but nothing to talk to", which is what
        // every invoke() in these tests should refuse on.
        GDBusConnection* connection() const override { return nullptr; }
        TelephonySnapshot snapshot() const override { return snap; }
    };

    std::unique_ptr<FakeSource> gateway_source(const char* path, const char* state = "connected") {
        auto source = std::make_unique<FakeSource>();
        source->snap.gateway.path = path;
        source->snap.gateway.state = state;
        return source;
    }

    std::unique_ptr<TelephonyClient> client_of(std::vector<std::unique_ptr<TelephonySource>> sources) {
        return std::make_unique<TelephonyClient>(std::move(sources), "02:00:00:00:00:01");
    }

} // namespace

TEST(PipewireTelephony, MatchesTheGatewayByAddress) {
    Payload payload(PIPEWIRE_ROOT);
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "02:00:00:00:00:01");

    EXPECT_EQ(snap.gateway.path, "/org/pipewire/Telephony/ag1") << "address match must be case-insensitive";
    EXPECT_TRUE(snap.gateway.ready()) << "a registered gateway means the service level connection is up";
    EXPECT_EQ(snap.audio_state, "active");
}

// The reply that carries the gateway carries no calls, so reading the call list
// from it is how the Hang up button goes missing.
TEST(PipewireTelephony, RootManagerCarriesNoCalls) {
    Payload payload(PIPEWIRE_ROOT);
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "02:00:00:00:00:01");
    EXPECT_TRUE(snap.calls.empty());
}

// The gateway's own manager has the calls and no gateway to match an address
// against, so they are collected by parentage instead.
TEST(PipewireTelephony, ScopesCallsToTheirOwnGateway) {
    Payload payload(PIPEWIRE_GATEWAY_CALLS);
    const std::vector<Call> calls = parse_pipewire_calls(payload.v, "/org/pipewire/Telephony/ag1");

    ASSERT_EQ(calls.size(), 1u) << "the other phone's call must not appear here";
    EXPECT_EQ(calls.front().number, "18002662278");
    EXPECT_TRUE(calls.front().connected());
    EXPECT_EQ(calls.front().path, "/org/pipewire/Telephony/ag1/call1");
    EXPECT_EQ(calls.front().telephony_path, "/org/pipewire/Telephony/ag1");
}

TEST(PipewireTelephony, CallsNeedAGatewayToBelongTo) {
    Payload payload(PIPEWIRE_GATEWAY_CALLS);
    EXPECT_TRUE(parse_pipewire_calls(payload.v, "").empty());
    EXPECT_TRUE(parse_pipewire_calls(payload.v, "/org/pipewire/Telephony/ag9").empty());
}

// A phone PipeWire is not serving must read as absent, never as "the only
// gateway there is" -- that would answer calls on someone else's phone.
TEST(PipewireTelephony, UnknownAddressYieldsNoGateway) {
    Payload payload(PIPEWIRE_ROOT);
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "11:22:33:44:55:66");

    EXPECT_TRUE(snap.gateway.path.empty());
    EXPECT_TRUE(snap.calls.empty());
    EXPECT_TRUE(snap.audio_state.empty());
}

// The indicators BlueZ reports have no PipeWire equivalent. They must stay at
// their defaults rather than being invented.
TEST(PipewireTelephony, ReportsNoCellularIndicators) {
    Payload payload(PIPEWIRE_ROOT);
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "02:00:00:00:00:01");

    EXPECT_TRUE(snap.gateway.operator_name.empty());
    EXPECT_EQ(snap.gateway.signal, 0);
    EXPECT_EQ(snap.gateway.battery, 0);
    EXPECT_FALSE(snap.gateway.service);
}

TEST(PipewireTelephony, EmptyPayloadIsNotAFailure) {
    Payload payload("@a{oa{sa{sv}}} {}");
    const TelephonySnapshot snap = parse_pipewire_telephony(payload.v, "02:00:00:00:00:01");
    EXPECT_TRUE(snap.gateway.path.empty());
}

// Order in the source vector is the whole selection policy: BlueZ carries the
// cellular indicators, so it wins wherever it has the profile.
TEST(TelephonyClientSources, PrefersTheFirstSourceWithAGateway) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    sources.push_back(gateway_source("/org/bluez/hci0/dev_X/telephony0"));
    sources.push_back(gateway_source("/org/pipewire/Telephony/ag0"));
    const auto client = client_of(std::move(sources));

    EXPECT_EQ(client->status().value("path", ""), "/org/bluez/hci0/dev_X/telephony0");
    EXPECT_TRUE(client->available());
}

TEST(TelephonyClientSources, FallsThroughToTheNextSource) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    sources.push_back(std::make_unique<FakeSource>());
    auto pipewire = gateway_source("/org/pipewire/Telephony/ag0");
    pipewire->identity = {"pipewire", "org.pipewire.Telephony", "g", "t", "c", false, false};
    sources.push_back(std::move(pipewire));
    const auto client = client_of(std::move(sources));

    const nlohmann::json status = client->status();
    EXPECT_EQ(status.value("backend", ""), "pipewire");
    EXPECT_FALSE(status.value("indicators", true)) << "PipeWire reports no indicators, and must say so";
}

// With no source serving, the payload must read exactly as it did before a
// second stack existed.
TEST(TelephonyClientSources, NoSourceReadsAsUnavailable) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    sources.push_back(std::make_unique<FakeSource>());
    const auto client = client_of(std::move(sources));

    const nlohmann::json status = client->status();
    EXPECT_FALSE(client->available());
    EXPECT_FALSE(status.value("available", true));
    EXPECT_EQ(status.value("backend", "unset"), "");
    EXPECT_TRUE(status.value("indicators", false)) << "an absent gateway must not claim indicators are missing";
    EXPECT_TRUE(client->calls().empty());
}

// Moving the audio is only meaningful where the stack carries it. BlueZ opens
// no voice link at all, and must say that rather than "unknown action".
TEST(TelephonyClientSources, AudioActionIsRefusedWithoutATransport) {
    std::vector<std::unique_ptr<TelephonySource>> sources;
    sources.push_back(gateway_source("/org/bluez/hci0/dev_X/telephony0"));
    const auto client = client_of(std::move(sources));

    std::string err;
    EXPECT_FALSE(client->call_action("", "audio_here", err));
    EXPECT_NE(err.find("iPhone"), std::string::npos);
    EXPECT_EQ(err.find("Unknown"), std::string::npos);
}

// The one place a silent wire-format bug could hide: BlueZ's Dial takes a URI,
// PipeWire's takes the bare number and builds the AT command itself.
TEST(TelephonyClientSources, DialArgumentMatchesTheStack) {
    const TelephonyIds bluez{"bluez", "org.bluez", "g", nullptr, "c", true, true};
    const TelephonyIds pipewire{"pipewire", "org.pipewire.Telephony", "g", "t", "c", false, false};

    EXPECT_EQ(dial_argument(bluez, "+15555550123"), "tel:+15555550123");
    EXPECT_EQ(dial_argument(pipewire, "+15555550123"), "+15555550123");
}
