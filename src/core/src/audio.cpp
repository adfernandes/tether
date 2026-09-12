#include "tether/audio.hpp"

#include <tether/log.hpp>

#include <chrono>
#include <glib.h>
#include <sstream>
#include <thread>

namespace tether::audio {

    namespace {

        constexpr int POLL_INTERVAL_MS = 200;
        // A paused player stops feeding its sink in tens of milliseconds.
        constexpr int SINK_QUIET_POLL_MS = 25;
        // How long a card takes to report a profile it was switched to.
        constexpr int PROFILE_WAIT_MS = 1500;

        std::string trimmed(const std::string& text) {
            const size_t begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return {};
            const size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(begin, end - begin + 1);
        }

        // Bounded, because pactl can wait forever on a server that is itself waiting on
        // BlueZ mid profile change. `ok` reports whether pactl ran and succeeded.
        std::string run(const char* command, bool* ok = nullptr) {
            gchar* out = nullptr;
            gint status = 0;
            GError* error = nullptr;
            if (ok)
                *ok = false;
            const std::string bounded = std::string("timeout 5 ") + command;
            if (!g_spawn_command_line_sync(bounded.c_str(), &out, nullptr, &status, &error)) {
                g_clear_error(&error);
                g_free(out);
                return {};
            }
            std::string text = out ? out : "";
            g_free(out);
            if (status != 0)
                return {};
            if (ok)
                *ok = true;
            return text;
        }

        std::string card_name(const std::string& address) {
            return "bluez_card." + sink_prefix(address).substr(sizeof("bluez_output.") - 1);
        }

        bool card_reaches(const std::string& card, const std::string& profile) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(PROFILE_WAIT_MS);
            while (std::chrono::steady_clock::now() < deadline) {
                if (active_profile_in(run("pactl list cards"), card) == profile)
                    return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
            }
            return false;
        }

    } // namespace

    std::string sink_prefix(const std::string& address) {
        std::string name = "bluez_output.";
        for (char c : address)
            name.push_back(c == ':' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        return name;
    }

    std::string default_sink() { return trimmed(run("pactl get-default-sink")); }

    std::string bluez_sink(const std::string& address) {
        if (address.empty())
            return {};
        const std::string prefix = sink_prefix(address);
        std::istringstream lines(run("pactl list short sinks"));
        for (std::string line; std::getline(lines, line);) {
            // index<TAB>name<TAB>driver...
            const size_t first = line.find('\t');
            if (first == std::string::npos)
                continue;
            const size_t second = line.find('\t', first + 1);
            const std::string name = line.substr(first + 1, second - first - 1);
            if (name.rfind(prefix, 0) == 0)
                return name;
        }
        return {};
    }

    bool set_default_sink(const std::string& name) {
        if (name.empty())
            return false;
        bool ok = false;
        run(("pactl set-default-sink " + name).c_str(), &ok);
        return ok;
    }

    bool restore_default_sink(const std::string& address, int timeout_ms) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            const std::string sink = bluez_sink(address);
            if (!sink.empty()) {
                if (default_sink() != sink)
                    set_default_sink(sink);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        }
        debug::log(DEBUG, "audio: no sink for {} within {}ms", address, timeout_ms);
        return false;
    }

    bool sink_quiet(const std::string& sink, int timeout_ms) {
        if (sink.empty())
            return true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (true) {
            bool running = false;
            std::istringstream lines(run("pactl list short sinks"));
            for (std::string line; std::getline(lines, line);) {
                if (line.find(sink) == std::string::npos)
                    continue;
                // index<TAB>name<TAB>driver<TAB>format<TAB>rate<TAB>state
                running = line.rfind("RUNNING") != std::string::npos;
                break;
            }
            if (!running)
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(SINK_QUIET_POLL_MS));
        }
        debug::log(DEBUG, "audio: {} still running after {}ms", sink, timeout_ms);
        return false;
    }

    std::string active_profile_in(const std::string& cards, const std::string& card) {
        constexpr std::string_view ACTIVE = "Active Profile: ";
        bool in_card = false;
        std::istringstream lines(cards);
        for (std::string line; std::getline(lines, line);) {
            const std::string text = trimmed(line);
            if (text.rfind("Card #", 0) == 0)
                in_card = false;
            else if (text == "Name: " + card)
                in_card = true;
            else if (in_card && text.rfind(ACTIVE, 0) == 0)
                return text.substr(ACTIVE.size());
        }
        return {};
    }

    std::string volume_in(const std::string& text) {
        // "Volume: front-left: 45877 /  70% / -9.29 dB,   front-right: 45877 /  70% / -9.29 dB"
        std::string out;
        std::istringstream channels(text.substr(0, text.find('\n')));
        for (std::string channel; std::getline(channels, channel, ',');) {
            const size_t colon = channel.rfind(':');
            if (colon == std::string::npos)
                return {};
            const std::string raw = trimmed(channel.substr(colon + 1, channel.find('/', colon) - colon - 1));
            if (raw.empty() || raw.find_first_not_of("0123456789") != std::string::npos)
                return {};
            out += (out.empty() ? "" : " ") + raw;
        }
        return out;
    }

    std::string release_bluez_card(const std::string& address) {
        if (address.empty())
            return {};
        const std::string card = card_name(address);
        const std::string profile = active_profile_in(run("pactl list cards"), card);
        // Only the playback profile: a headset profile is carrying a call here.
        if (profile.rfind("a2dp", 0) != 0)
            return {};
        bool ok = false;
        run(("pactl set-card-profile " + card + " off").c_str(), &ok);
        if (!ok)
            return {};
        // Gone before it can be rebuilt: switching straight back on can keep the old sink.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(PROFILE_WAIT_MS);
        while (!bluez_sink(address).empty() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        return profile;
    }

    bool revive_bluez_card(const std::string& address) {
        const std::string card = card_name(address);
        if (active_profile_in(run("pactl list cards"), card) != "off")
            return false;
        debug::log(INFO, "audio: {} came back with its profile off; switching A2DP on", card);
        return restore_bluez_card(address, "a2dp-sink");
    }

    bool restore_bluez_card(const std::string& address, const std::string& profile) {
        if (address.empty() || profile.empty())
            return false;
        const std::string card = card_name(address);
        // pactl can report success while the card stays off, so the result is read back.
        for (int attempt = 0; attempt < 2; ++attempt) {
            run(("pactl set-card-profile " + card + " " + profile).c_str());
            if (card_reaches(card, profile))
                return true;
        }
        return false;
    }

    std::string sink_volume(const std::string& sink) {
        if (sink.empty())
            return {};
        return volume_in(run(("pactl get-sink-volume " + sink).c_str()));
    }

    bool set_sink_volume(const std::string& sink, const std::string& volume) {
        if (sink.empty() || volume.empty())
            return false;
        // A sink created moments ago can have its volume overwritten as hardware volume settles.
        for (int attempt = 0; attempt < 2; ++attempt) {
            run(("pactl set-sink-volume " + sink + " " + volume).c_str());
            if (sink_volume(sink) == volume)
                return true;
        }
        return false;
    }

} // namespace tether::audio
