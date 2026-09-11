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

    // HACK: Whether a stream is attached to a Bluetooth device's sink while the sink stays
    // suspended. PipeWire does not reopen a transport the device tore down under it.
    bool bluez_sink_stuck(const std::string& address);

    // HACK: Switches a Bluetooth device's card profile off and back, which rebuilds its sink.
    bool restart_bluez_card(const std::string& address);

    // HACK: Parsers for `pactl list short sinks` with `pactl list short sink-inputs`, and for
    // `pactl list cards`, exposed for tests.
    bool bluez_sink_stuck_in(const std::string& short_sinks,
                             const std::string& short_inputs,
                             const std::string& address);
    std::string active_profile_in(const std::string& cards, const std::string& card);

    // Sink name for a Bluetooth address, exposed for tests.
    std::string sink_prefix(const std::string& address);

} // namespace tether::audio
