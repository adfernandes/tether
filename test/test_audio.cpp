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
