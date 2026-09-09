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
