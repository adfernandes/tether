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
