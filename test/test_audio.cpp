#include <gtest/gtest.h>
#include <tether/audio.hpp>

using namespace tether::audio;

// The audio server names a Bluetooth sink by the address in upper case with
// underscores, whatever case BlueZ reported it in.
TEST(Audio, NamesTheSinkAfterTheAddress) {
    EXPECT_EQ(sink_prefix("02:00:00:00:00:02"), "bluez_output.02_00_00_00_00_02");
    EXPECT_EQ(sink_prefix("02:00:00:00:00:02"), "bluez_output.02_00_00_00_00_02");
    EXPECT_EQ(sink_prefix(""), "bluez_output.");
}

// No address means no sink to look for, and no pactl call.
TEST(Audio, HasNoSinkWithoutAnAddress) { EXPECT_TRUE(bluez_sink("").empty()); }

// Raw values, not percentages: a percentage round trip loses a step on every handoff.
TEST(Audio, ReadsARawSinkVolume) {
    // Captured 2026-09-11 from `pactl get-sink-volume` on PipeWire 1.6.8.
    EXPECT_EQ(volume_in("Volume: front-left: 45877 /  70% / -9.29 dB,   front-right: 45877 /  70% / -9.29 dB\n"
                        "        balance 0.00\n"),
              "45877 45877");
    EXPECT_EQ(volume_in("Volume: mono: 11453 /  17% / -45.43 dB\n"), "11453");
    EXPECT_TRUE(volume_in("").empty());
    EXPECT_TRUE(volume_in("Failure: No such entity\n").empty());
}

TEST(Audio, ReadsACardsActiveProfile) {
    const std::string cards = "Card #82\n\tName: bluez_card.02_00_00_00_00_01\n\tActive Profile: audio-gateway\n"
                              "Card #245\n\tName: bluez_card.02_00_00_00_00_03\n\tProperties:\n"
                              "\t\tdevice.name = \"bluez_card.02_00_00_00_00_03\"\n\tActive Profile: a2dp-sink\n";
    EXPECT_EQ(active_profile_in(cards, "bluez_card.02_00_00_00_00_03"), "a2dp-sink");
    EXPECT_EQ(active_profile_in(cards, "bluez_card.02_00_00_00_00_01"), "audio-gateway");
    EXPECT_TRUE(active_profile_in(cards, "bluez_card.00_00_00_00_00_00").empty());
}
