#pragma once

#include <string>

// The audio server, through pactl, which both PulseAudio and PipeWire answer.
// Every call is a no-op returning empty when pactl is not installed.
namespace tether::audio {

    // The sink the audio server is currently sending new streams to.
    std::string default_sink();

    // The sink for a Bluetooth device ("bluez_output.AC_F2_3C_AF_52_9C.1")
    // or empty when the device has no sink right now.
    std::string bluez_sink(const std::string& address);

    bool set_default_sink(const std::string& name);

    // Waits for a Bluetooth device's sink to come back and makes it the default
    // again. False when it did not appear before the deadline.
    bool restore_default_sink(const std::string& address, int timeout_ms);

    // Switches a Bluetooth device's card off when it is on an A2DP profile, so the audio server
    // lets go of the transport before another host takes the buds. Returns the profile to restore,
    // empty when nothing was switched.
    std::string release_bluez_card(const std::string& address);

    // Switches the card back to `profile`, which builds a new sink under a new index. False when
    // the card is still not on it after one retry.
    bool restore_bluez_card(const std::string& address, const std::string& profile);

    // A sink's raw per-channel volume ("45877 45877"), empty when unknown.
    std::string sink_volume(const std::string& sink);

    // Sets a raw volume from sink_volume() and reads it back. False when it did not take.
    bool set_sink_volume(const std::string& sink, const std::string& volume);

    // Parsers for `pactl list cards` and `pactl get-sink-volume`, exposed for tests.
    std::string active_profile_in(const std::string& cards, const std::string& card);
    std::string volume_in(const std::string& text);

    // Sink name for a Bluetooth address, exposed for tests.
    std::string sink_prefix(const std::string& address);

} // namespace tether::audio
