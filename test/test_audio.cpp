#include <gtest/gtest.h>
#include <tether/audio.hpp>

using namespace tether::audio;

// The audio server names a Bluetooth sink by the address in upper case with
// underscores, whatever case BlueZ reported it in.
TEST(Audio, NamesTheSinkAfterTheAddress) {
    EXPECT_EQ(sink_prefix("ac:f2:3c:af:52:9c"), "bluez_output.AC_F2_3C_AF_52_9C");
    EXPECT_EQ(sink_prefix("AC:F2:3C:AF:52:9C"), "bluez_output.AC_F2_3C_AF_52_9C");
    EXPECT_EQ(sink_prefix(""), "bluez_output.");
}

// No address means no sink to look for, and no pactl call.
TEST(Audio, HasNoSinkWithoutAnAddress) { EXPECT_TRUE(bluez_sink("").empty()); }

// Captured 2026-09-10 after an iPhone call tore down the AirPods transport: the sink
// stayed suspended while the player kept feeding it.
TEST(Audio, SeesAStreamStuckOnASuspendedBluetoothSink) {
    const std::string sinks = "54\talsa_output.pci-0000_c1_00.6.analog-stereo\tPipeWire\ts32le 2ch 48000Hz\tSUSPENDED\n"
                              "249\tbluez_output.F8_D3_F0_3C_84_4A.1\tPipeWire\ts16le 2ch 48000Hz\tSUSPENDED\n";
    const std::string inputs = "236\t249\t235\tPipeWire\tfloat32le 2ch 48000Hz\n";
    EXPECT_TRUE(bluez_sink_stuck_in(sinks, inputs, "F8:D3:F0:3C:84:4A"));

    // Nothing attached: a suspended sink is only idle.
    EXPECT_FALSE(bluez_sink_stuck_in(sinks, "", "F8:D3:F0:3C:84:4A"));
    // A stream on another sink is not this one's.
    EXPECT_FALSE(bluez_sink_stuck_in(sinks, "236\t54\t235\tPipeWire\tfloat32le 2ch 48000Hz\n", "F8:D3:F0:3C:84:4A"));
    const std::string running = "249\tbluez_output.F8_D3_F0_3C_84_4A.1\tPipeWire\ts16le 2ch 48000Hz\tRUNNING\n";
    EXPECT_FALSE(bluez_sink_stuck_in(running, inputs, "F8:D3:F0:3C:84:4A"));
    EXPECT_FALSE(bluez_sink_stuck_in(sinks, inputs, ""));
}

TEST(Audio, ReadsACardsActiveProfile) {
    const std::string cards = "Card #82\n\tName: bluez_card.60_57_C8_30_6A_F7\n\tActive Profile: audio-gateway\n"
                              "Card #245\n\tName: bluez_card.F8_D3_F0_3C_84_4A\n\tProperties:\n"
                              "\t\tdevice.name = \"bluez_card.F8_D3_F0_3C_84_4A\"\n\tActive Profile: a2dp-sink\n";
    EXPECT_EQ(active_profile_in(cards, "bluez_card.F8_D3_F0_3C_84_4A"), "a2dp-sink");
    EXPECT_EQ(active_profile_in(cards, "bluez_card.60_57_C8_30_6A_F7"), "audio-gateway");
    EXPECT_TRUE(active_profile_in(cards, "bluez_card.00_00_00_00_00_00").empty());
}
