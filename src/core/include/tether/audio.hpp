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

    // Sink name for a Bluetooth address, exposed for tests.
    std::string sink_prefix(const std::string& address);

} // namespace tether::audio
