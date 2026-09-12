#include <gtest/gtest.h>
#include <tether/bluetooth/airpods.hpp>

using namespace tether::bluetooth;

namespace {

    std::optional<BatteryUpdate> parse(const std::vector<uint8_t>& packet) {
        return parse_battery(packet.data(), packet.size());
    }

    // 04 00 04 00 04 00 [count] ([component] 01 [level] [status] 01) * count
    const std::vector<uint8_t> ALL_THREE = {0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x03, 0x04, 0x01, 0x52, 0x01,
                                            0x01, 0x02, 0x01, 0x4f, 0x01, 0x01, 0x08, 0x01, 0x2d, 0x01, 0x01};

    const std::vector<uint8_t> RIGHT_ONLY = {0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x02, 0x01, 0x50, 0x01, 0x01};

    // A bud back in the case: present in the packet, reporting nothing.
    const std::vector<uint8_t> LEFT_DISCONNECTED = {
        0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x00, 0x04, 0x01};

} // namespace

TEST(AirPods, ParsesEveryComponent) {
    auto update = parse(ALL_THREE);
    ASSERT_TRUE(update.has_value());
    EXPECT_TRUE(update->has_left);
    EXPECT_TRUE(update->has_right);
    EXPECT_TRUE(update->has_case);
    EXPECT_EQ(update->levels.left, 82);
    EXPECT_EQ(update->levels.right, 79);
    EXPECT_EQ(update->levels.case_, 45);
}

// A component the packet leaves out must keep whatever it had. Reporting it as 0
// or unknown would make one bud's update blank the other.
TEST(AirPods, PartialUpdateLeavesOtherComponentsAlone) {
    AirPodsBattery battery;
    merge_battery(battery, *parse(ALL_THREE));
    merge_battery(battery, *parse(RIGHT_ONLY));

    EXPECT_EQ(battery.left, 82);
    EXPECT_EQ(battery.right, 80);
    EXPECT_EQ(battery.case_, 45);
}

// Status 0x04 is a bud in the case or a shut case, not a flat battery.
TEST(AirPods, DisconnectedComponentReadsUnknownNotZero) {
    auto update = parse(LEFT_DISCONNECTED);
    ASSERT_TRUE(update.has_value());
    EXPECT_TRUE(update->has_left);
    EXPECT_EQ(update->levels.left, -1);

    AirPodsBattery battery;
    merge_battery(battery, *parse(ALL_THREE));
    merge_battery(battery, *update);
    EXPECT_EQ(battery.left, -1);
    EXPECT_EQ(battery.right, 79);
}

TEST(AirPods, RejectsMalformedPackets) {
    // Count says two entries, only one follows.
    EXPECT_FALSE(parse({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x02, 0x04, 0x01, 0x52, 0x01, 0x01}).has_value());
    // Some other AAP notification on the same channel.
    EXPECT_FALSE(parse({0x04, 0x00, 0x04, 0x00, 0x0f, 0x00, 0x01, 0x04, 0x01, 0x52, 0x01, 0x01}).has_value());
    EXPECT_FALSE(parse({0x04, 0x00, 0x04}).has_value());
    EXPECT_FALSE(parse({}).has_value());
    // Well-formed but carrying only components we have no field for.
    EXPECT_FALSE(parse({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x01, 0x01, 0x52, 0x01, 0x01}).has_value());
}

TEST(AirPods, ParsesEveryListeningMode) {
    const std::pair<uint8_t, AncMode> cases[] = {
        {0x01, AncMode::Off},
        {0x02, AncMode::NoiseCancellation},
        {0x03, AncMode::Transparency},
        {0x04, AncMode::Adaptive},
    };
    for (const auto& [wire, expected] : cases) {
        std::vector<uint8_t> packet = {0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0d, wire, 0x00, 0x00, 0x00};
        auto mode = parse_anc(packet.data(), packet.size());
        ASSERT_TRUE(mode.has_value()) << "wire value " << int(wire);
        EXPECT_EQ(*mode, expected);
        // Round trip through the names the IPC and CLI use.
        EXPECT_EQ(anc_mode_from_string(to_string(*mode)), expected);
    }
}

TEST(AirPods, RejectsMalformedListeningModes) {
    const auto packet = [](std::initializer_list<uint8_t> bytes) {
        std::vector<uint8_t> v(bytes);
        return parse_anc(v.data(), v.size());
    };
    // Mode 0 and 5 are outside the range; the wire values start at 1.
    EXPECT_FALSE(packet({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00}).has_value());
    EXPECT_FALSE(packet({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0d, 0x05, 0x00, 0x00, 0x00}).has_value());
    // Right prefix, wrong length.
    EXPECT_FALSE(packet({0x04, 0x00, 0x04, 0x00, 0x09, 0x00, 0x0d, 0x02}).has_value());
    // A battery notification must not be read as a listening mode.
    EXPECT_FALSE(packet({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x02, 0x01, 0x50, 0x01}).has_value());
    EXPECT_FALSE(parse_anc(nullptr, 0).has_value());

    EXPECT_FALSE(anc_mode_from_string("").has_value());
    EXPECT_FALSE(anc_mode_from_string("ancs").has_value());
}

namespace {
    std::optional<EarState> ear(std::initializer_list<uint8_t> bytes) {
        std::vector<uint8_t> v(bytes);
        return parse_ear(v.data(), v.size());
    }
    constexpr EarState WORN{EarStatus::InEar, EarStatus::InEar};
    constexpr EarState ONE_OUT{EarStatus::InEar, EarStatus::OutOfEar};
    constexpr EarState BOTH_OUT{EarStatus::OutOfEar, EarStatus::OutOfEar};
    constexpr EarState UNKNOWN{};
} // namespace

TEST(AirPods, ParsesEarDetection) {
    auto worn = ear({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00});
    ASSERT_TRUE(worn.has_value());
    EXPECT_EQ(worn->primary, EarStatus::InEar);
    EXPECT_EQ(worn->secondary, EarStatus::InEar);
    EXPECT_EQ(worn->in_ear(), 2);

    auto mixed = ear({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x01, 0x02});
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(mixed->primary, EarStatus::OutOfEar);
    EXPECT_EQ(mixed->secondary, EarStatus::InCase);
    EXPECT_EQ(mixed->in_ear(), 0);
    EXPECT_TRUE(mixed->known());

    // An unrecognised status byte is unknown, not in-ear.
    auto odd = ear({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x7f});
    ASSERT_TRUE(odd.has_value());
    EXPECT_EQ(odd->secondary, EarStatus::Unknown);
    EXPECT_EQ(odd->in_ear(), 1);
}

TEST(AirPods, RejectsMalformedEarDetection) {
    // Right prefix, wrong length.
    EXPECT_FALSE(ear({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00}).has_value());
    EXPECT_FALSE(ear({0x04, 0x00, 0x04, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}).has_value());
    // A battery notification is 12 bytes, but check the prefix is what rejects it.
    EXPECT_FALSE(ear({0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x02}).has_value());
    EXPECT_FALSE(parse_ear(nullptr, 0).has_value());
}

TEST(AirPods, PauseModeNeverDoesNothing) {
    EXPECT_EQ(ear_media_action(WORN, BOTH_OUT, PauseMode::Never, false), MediaAction::None);
    EXPECT_EQ(ear_media_action(BOTH_OUT, WORN, PauseMode::Never, true), MediaAction::None);
}

// The first report after connecting says where the buds already are. Reading it
// as a transition would pause music the moment a case is opened nearby.
TEST(AirPods, FirstEarReportIsNotATransition) {
    EXPECT_EQ(ear_media_action(UNKNOWN, BOTH_OUT, PauseMode::OneRemoved, false), MediaAction::None);
    EXPECT_EQ(ear_media_action(UNKNOWN, WORN, PauseMode::OneRemoved, true), MediaAction::None);
    EXPECT_EQ(ear_media_action(WORN, UNKNOWN, PauseMode::OneRemoved, false), MediaAction::None);
}

TEST(AirPods, PausesWhenOneBudComesOut) {
    EXPECT_EQ(ear_media_action(WORN, ONE_OUT, PauseMode::OneRemoved, false), MediaAction::Pause);
    EXPECT_EQ(ear_media_action(ONE_OUT, WORN, PauseMode::OneRemoved, true), MediaAction::Resume);

    // Both-removed is the looser policy: one bud out is still "worn".
    EXPECT_EQ(ear_media_action(WORN, ONE_OUT, PauseMode::BothRemoved, false), MediaAction::None);
    EXPECT_EQ(ear_media_action(ONE_OUT, BOTH_OUT, PauseMode::BothRemoved, false), MediaAction::Pause);
    EXPECT_EQ(ear_media_action(BOTH_OUT, ONE_OUT, PauseMode::BothRemoved, true), MediaAction::Resume);
}

// Only a pause this code made is undone. Music the user stopped by hand stays stopped.
TEST(AirPods, DoesNotResumeWhatItDidNotPause) {
    EXPECT_EQ(ear_media_action(BOTH_OUT, WORN, PauseMode::OneRemoved, false), MediaAction::None);
    EXPECT_EQ(ear_media_action(BOTH_OUT, WORN, PauseMode::OneRemoved, true), MediaAction::Resume);
}

TEST(AirPods, PutsABudInTheCaseTheSameAsOutOfTheEar) {
    const EarState in_case{EarStatus::InEar, EarStatus::InCase};
    EXPECT_EQ(ear_media_action(WORN, in_case, PauseMode::OneRemoved, false), MediaAction::Pause);
    // An unchanged state is never a trigger, however it is reported.
    EXPECT_EQ(ear_media_action(in_case, in_case, PauseMode::OneRemoved, false), MediaAction::None);
}

// A pod charging inside an open case reports charging|disconnected, so the status
// byte only ever means anything as a bitmask.
TEST(AirPods, TreatsTheBatteryStatusAsABitmask) {
    const std::vector<uint8_t> charging_in_case = {
        0x04, 0x00, 0x04, 0x00, 0x04, 0x00, 0x01, 0x04, 0x01, 0x63, 0x05, 0x01};
    auto update = parse(charging_in_case);
    ASSERT_TRUE(update.has_value());
    EXPECT_TRUE(update->has_left);
    EXPECT_EQ(update->levels.left, -1);
}

TEST(AirPods, ParsesTheConnectedDeviceList) {
    // Two hosts: this machine, then an iPhone sitting idle. The count is at offset 8
    // and addresses are in display order.
    const std::vector<uint8_t> packet = {0x04, 0x00, 0x04, 0x00, 0x2e, 0x00, 0x00, 0x00, 0x02,
                                         0xac, 0xf2, 0x3c, 0xaf, 0x52, 0x9c, 0x00, 0x03,
                                         0x60, 0x57, 0xc8, 0x30, 0x6a, 0xf7, 0x01, 0x15};
    auto peers = parse_connected_devices(packet.data(), packet.size());
    ASSERT_TRUE(peers.has_value());
    ASSERT_EQ(peers->size(), 2u);
    EXPECT_EQ((*peers)[0].address, "AC:F2:3C:AF:52:9C");
    EXPECT_EQ((*peers)[0].state, 0x03);
    EXPECT_EQ((*peers)[1].address, "60:57:C8:30:6A:F7");
    // 0x15 is an iPhone sitting there doing nothing, which is the state this machine
    // claims the buds out of. Reading it as "taken" means never claiming at all.
    EXPECT_FALSE((*peers)[1].taking_over());
    EXPECT_FALSE((*peers)[1].active());

    // Misreading the layout makes this machine's own entry a peer holding the buds.
    const auto summary = summarize_peers(*peers, "AC:F2:3C:AF:52:9C");
    EXPECT_FALSE(summary.taking_over);
    EXPECT_FALSE(summary.active);

    // Captured 2026-09-10 from AirPods Pro 3 with only this machine attached.
    const std::vector<uint8_t> alone = {
        0x04, 0x00, 0x04, 0x00, 0x2e, 0x00, 0x01, 0x00, 0x01, 0xac, 0xf2, 0x3c, 0xaf, 0x52, 0x9c, 0x02, 0x02};
    auto only_us = parse_connected_devices(alone.data(), alone.size());
    ASSERT_TRUE(only_us.has_value());
    ASSERT_EQ(only_us->size(), 1u);
    EXPECT_EQ((*only_us)[0].address, "AC:F2:3C:AF:52:9C");
    EXPECT_EQ((*only_us)[0].state, 0x02);

    // A truncated list is not a list.
    EXPECT_FALSE(parse_connected_devices(packet.data(), packet.size() - 1).has_value());
    EXPECT_FALSE(parse_connected_devices(packet.data(), 4).has_value());
}

// A peer on its way to the buds blocks a claim; one already holding them also ends
// local playback. An idle peer does neither.
TEST(AirPods, SeparatesAnIdlePeerFromOneTakingTheBuds) {
    const auto peer = [](uint8_t state) { return AapPeer{"81:71:C8:30:6A:F3", 0x02, state}; };
    EXPECT_FALSE(peer(0x15).taking_over());
    EXPECT_TRUE(peer(0x10).taking_over());
    EXPECT_FALSE(peer(0x10).active());
    EXPECT_TRUE(peer(0x12).taking_over());
    EXPECT_TRUE(peer(0x17).taking_over());
    EXPECT_TRUE(peer(0x17).active());
    EXPECT_FALSE(peer(0x15).active());
    // AirPods Pro 1 leaves a healthy iPhone at 0x01 forever.
    EXPECT_FALSE(peer(0x01).taking_over());
}

// Ownership moves with bit 0x02. Captured 2026-09-11 over five handoffs, each confirmed by the
// firmware's own verdict packet: the iPhone idles at 0x05 and owns at 0x07, this machine 0x00 and
// 0x02, and neither ever reaches 0x10. An owner that has finished is what a reclaim claims from,
// so owning must not read as taking over.
TEST(AirPods, ReadsOwnershipFromTheOwnerBit) {
    const auto peer = [](uint8_t state) { return AapPeer{"81:71:C8:30:6A:F3", 0x02, state}; };
    EXPECT_FALSE(peer(0x05).active());
    EXPECT_TRUE(peer(0x07).active());
    EXPECT_FALSE(peer(0x00).active());
    EXPECT_TRUE(peer(0x02).active());
    EXPECT_FALSE(peer(0x07).taking_over());
    EXPECT_FALSE(peer(0x02).taking_over());
    // The same bit separates the pair reported by the models the reference was written against.
    EXPECT_FALSE(peer(0x15).active());
    EXPECT_TRUE(peer(0x17).active());
}

// The registration packets the buds expect for every other host, ported from a working
// implementation. The layout is fixed: opcode, the target address least-significant byte first,
// then ASCII keys with this machine's address in the middle.
TEST(AirPods, BuildsTheHostRegistrationPackets) {
    const std::string self = "AC:F2:3C:AF:52:9C";
    const auto add = tipi_add_device(self, "60:57:C8:30:6A:F7");
    ASSERT_EQ(add.size(), 96u);
    EXPECT_EQ(std::vector<uint8_t>(add.begin(), add.begin() + 6),
              (std::vector<uint8_t>{0x04, 0x00, 0x04, 0x00, 0x10, 0x00}));
    EXPECT_EQ(std::vector<uint8_t>(add.begin() + 6, add.begin() + 12),
              (std::vector<uint8_t>{0xf7, 0x6a, 0x30, 0xc8, 0x57, 0x60}));
    // Body offsets counted from the end of the prefix.
    EXPECT_EQ(std::string(add.begin() + 6 + 11, add.begin() + 6 + 19), "idleTime");
    EXPECT_EQ(std::string(add.begin() + 6 + 40, add.begin() + 6 + 57), self);
    EXPECT_EQ(std::string(add.begin() + 6 + 65, add.begin() + 6 + 72), "Android");
    EXPECT_EQ(add.back(), 0x0e);

    const auto media = tipi_media_info(self, "60:57:C8:30:6A:F7");
    ASSERT_EQ(media.size(), 122u);
    EXPECT_EQ(std::string(media.begin() + 6 + 11, media.begin() + 6 + 21), "playingApp");
    EXPECT_EQ(std::string(media.begin() + 6 + 25, media.begin() + 6 + 43), "hostStreamingState");
    EXPECT_EQ(std::string(media.begin() + 6 + 57, media.begin() + 6 + 74), self);
    EXPECT_EQ(media.back(), 0x64);

    // An address that will not parse is no packet at all.
    EXPECT_TRUE(tipi_add_device(self, "nonsense").empty());
    EXPECT_TRUE(tipi_media_info("", "60:57:C8:30:6A:F7").empty());
}

// Handoff yields to a phone that owns the buds and is playing through them. Ownership alone is a
// phone that finished its call and kept the bit; audio alone is a host that owns nothing.
TEST(AirPods, YieldsOnlyToAnOwnerWithAudio) {
    AirPodsState state;
    state.peer_active = true;
    EXPECT_FALSE(state.peer_busy());
    state.peer_audio = true;
    EXPECT_TRUE(state.peer_busy());
    state.peer_active = false;
    EXPECT_FALSE(state.peer_busy());
}

// This machine's own entry rises with its own ownership and is never a peer.
TEST(AirPods, SummarisesPeersWithoutCountingItself) {
    const std::vector<AapPeer> peers = {{"AC:F2:3C:AF:52:9C", 0x01, 0x17}, {"81:71:C8:30:6A:F3", 0x02, 0x10}};
    const auto summary = summarize_peers(peers, "AC:F2:3C:AF:52:9C");
    EXPECT_TRUE(summary.taking_over);
    EXPECT_FALSE(summary.active);

    // A phone on a call is the case handoff exists for.
    const std::vector<AapPeer> on_a_call = {{"81:71:C8:30:6A:F3", 0x02, 0x17}};
    EXPECT_TRUE(summarize_peers(on_a_call, "AC:F2:3C:AF:52:9C").active);

    // Without a local address every entry counts, which is the safe way round.
    EXPECT_TRUE(summarize_peers(peers, "").active);
}

TEST(AirPods, ParsesTheAudioSource) {
    // Here the address is least-significant byte first.
    const std::vector<uint8_t> packet = {0x04, 0x00, 0x04, 0x00, 0x0e, 0x00, 0xf7, 0x6a, 0x30, 0xc8, 0x57, 0x60, 0x01};
    auto event = parse_audio_source(packet.data(), packet.size());
    ASSERT_TRUE(event.has_value());
    EXPECT_EQ(event->address, "60:57:C8:30:6A:F7");
    EXPECT_EQ(event->source, AudioSource::Call);

    EXPECT_FALSE(parse_audio_source(packet.data(), packet.size() - 1).has_value());
}

// Measured on AirPods Pro 3: the buds route this machine's stream while the phone still owns
// them, then hand the route back to the phone within seconds.
TEST(AirPods, ClaimsWhenPlayingHereWithoutOwningTheBuds) {
    const std::string local = "AC:F2:3C:AF:52:9C";
    const AudioSourceEvent here{local, AudioSource::Media};
    EXPECT_TRUE(claims_for_playback(here, local, false, false));
    EXPECT_FALSE(claims_for_playback(here, local, true, false));
    // No verdict yet: the opening claim covers a new session.
    EXPECT_FALSE(claims_for_playback(here, local, std::nullopt, false));
    // Taking the buds off a phone call is the one thing this must never do.
    EXPECT_FALSE(claims_for_playback(here, local, false, true));
    EXPECT_FALSE(claims_for_playback({"60:57:C8:30:6A:F7", AudioSource::Media}, local, false, false));
    EXPECT_FALSE(claims_for_playback({local, AudioSource::None}, local, false, false));
    EXPECT_FALSE(claims_for_playback(here, "", false, false));
}

TEST(AirPods, OwnershipFollowsThePeerTakingTheBuds) {
    EXPECT_EQ(ownership_action(true, false, true), HandoffAction::Release);
    // Given up already: nothing to give up twice.
    EXPECT_EQ(ownership_action(true, true, true), HandoffAction::None);
    EXPECT_EQ(ownership_action(false, true, true), HandoffAction::Reclaim);
    EXPECT_EQ(ownership_action(false, false, true), HandoffAction::None);
}

TEST(AirPods, OwnershipIsOffUnlessEnabledButStillGivesTheBudsBack) {
    EXPECT_EQ(ownership_action(true, false, false), HandoffAction::None);
    EXPECT_EQ(ownership_action(false, true, false), HandoffAction::Reclaim);
}

TEST(AirPods, HandoffIsOffUnlessEnabled) { EXPECT_EQ(handoff_action(true, true, false, false), HandoffAction::None); }

// Turning the setting off while the phone has the buds must not strand them there.
TEST(AirPods, ReclaimsReleasedBudsEvenWhenTurnedOff) {
    EXPECT_EQ(handoff_action(false, false, true, false), HandoffAction::Reclaim);
}

TEST(AirPods, HandsBudsOverForACallAndTakesThemBack) {
    EXPECT_EQ(handoff_action(true, true, false, true), HandoffAction::Release);
    // Released, so the buds are no longer on this machine; the call ending is the trigger.
    EXPECT_EQ(handoff_action(true, false, true, true), HandoffAction::None);
    EXPECT_EQ(handoff_action(false, false, true, true), HandoffAction::Reclaim);
}

// Buds the user disconnected themselves are not ours to take back.
TEST(AirPods, DoesNotReclaimBudsItDidNotRelease) {
    EXPECT_EQ(handoff_action(false, false, false, true), HandoffAction::None);
    EXPECT_EQ(handoff_action(false, true, false, true), HandoffAction::None);
}

// A call while the buds are already on the phone has nothing to hand over.
TEST(AirPods, DoesNothingWhenTheBudsAreNotOnThisMachine) {
    EXPECT_EQ(handoff_action(true, false, false, true), HandoffAction::None);
}

// Releasing twice would lose track of what to give back.
TEST(AirPods, DoesNotReleaseTwice) { EXPECT_EQ(handoff_action(true, true, true, true), HandoffAction::None); }

TEST(AirPods, RecognisesCallsThatWantTheBuds) {
    const auto calls = [](const char* text) { return nlohmann::json::parse(text); };
    EXPECT_TRUE(call_wants_audio(calls(R"([{"state":"incoming","ringing":true}])")));
    EXPECT_TRUE(call_wants_audio(calls(R"([{"state":"active","connected":true}])")));
    EXPECT_TRUE(call_wants_audio(calls(R"([{"state":"dialing","outgoing":true}])")));
    // A held call still has the buds' attention.
    EXPECT_TRUE(call_wants_audio(calls(R"([{"state":"held","connected":true}])")));

    EXPECT_FALSE(call_wants_audio(calls("[]")));
    EXPECT_FALSE(call_wants_audio(calls(R"([{"state":"disconnected"}])")));
    EXPECT_FALSE(call_wants_audio(nlohmann::json()));
    EXPECT_FALSE(call_wants_audio(calls(R"({"not":"an array"})")));

    // One live call among finished ones still counts.
    EXPECT_TRUE(call_wants_audio(calls(R"([{"state":"disconnected"},{"state":"active","connected":true}])")));
}

TEST(AirPods, DistinguishesAirPodsFromAnIPhone) {
    Device buds;
    buds.name = "ZBZ AirPros";
    buds.modalias = "bluetooth:v004Cp200Ed0100";
    buds.uuids = {UUID_A2DP_SINK};
    EXPECT_TRUE(buds.looks_like_airpods());
    EXPECT_FALSE(buds.looks_like_iphone());

    // An iPhone is the same vendor and can carry the same audio profile.
    Device phone;
    phone.name = "iPhone";
    phone.modalias = "bluetooth:v004Cp700Ed0100";
    phone.uuids = {UUID_A2DP_SINK, UUID_MAP_MAS, UUID_PBAP_PSE};
    EXPECT_FALSE(phone.looks_like_airpods());

    // Modalias is absent until SDP has been read, so the name still has to work.
    Device unread;
    unread.name = "AirPods Pro";
    unread.uuids = {UUID_A2DP_SINK};
    EXPECT_TRUE(unread.looks_like_airpods());

    Device speaker;
    speaker.name = "Kitchen Speaker";
    speaker.uuids = {UUID_A2DP_SINK};
    EXPECT_FALSE(speaker.looks_like_airpods());
}

TEST(AirPods, JsonOmitsUnknownListeningMode) {
    AirPodsState state;
    ASSERT_FALSE(state.anc.has_value());

    const nlohmann::json j = to_json(state);
    EXPECT_FALSE(j.contains("anc"));
    // Readers ask for it as a string; a null would throw instead of defaulting.
    EXPECT_EQ(j.value("anc", ""), "");

    state.anc = AncMode::Transparency;
    EXPECT_EQ(to_json(state).value("anc", ""), "transparency");
}
