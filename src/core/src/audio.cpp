#include "tether/audio.hpp"

#include <tether/log.hpp>

#include <chrono>
#include <glib.h>
#include <sstream>
#include <thread>
#include <vector>

namespace tether::audio {

    namespace {

        constexpr int POLL_INTERVAL_MS = 200;
        // HACK: Between switching a card off and back on, so the audio server finishes tearing it down.
        constexpr int PROFILE_SETTLE_MS = 2000;

        std::string trimmed(const std::string& text) {
            const size_t begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return {};
            const size_t end = text.find_last_not_of(" \t\r\n");
            return text.substr(begin, end - begin + 1);
        }

        // Never blocks the caller for long, pactl answers locally or not at all.
        // `ok` reports whether pactl ran and succeeded.
        std::string run(const char* command, bool* ok = nullptr) {
            gchar* out = nullptr;
            gint status = 0;
            GError* error = nullptr;
            if (ok)
                *ok = false;
            if (!g_spawn_command_line_sync(command, &out, nullptr, &status, &error)) {
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

        std::vector<std::string> tab_fields(const std::string& line) {
            std::vector<std::string> out;
            std::istringstream fields(line);
            for (std::string field; std::getline(fields, field, '\t');)
                out.push_back(field);
            return out;
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

    // HACK
    bool bluez_sink_stuck_in(const std::string& short_sinks,
                             const std::string& short_inputs,
                             const std::string& address) {
        if (address.empty())
            return false;
        const std::string prefix = sink_prefix(address);
        std::string index;
        std::istringstream sinks(short_sinks);
        for (std::string line; std::getline(sinks, line);) {
            // index<TAB>name<TAB>driver<TAB>format<TAB>state
            const auto fields = tab_fields(line);
            if (fields.size() < 5 || fields[1].rfind(prefix, 0) != 0)
                continue;
            if (trimmed(fields[4]) != "SUSPENDED")
                return false;
            index = fields[0];
            break;
        }
        if (index.empty())
            return false;
        std::istringstream inputs(short_inputs);
        for (std::string line; std::getline(inputs, line);) {
            // index<TAB>sink<TAB>client<TAB>driver<TAB>format
            const auto fields = tab_fields(line);
            if (fields.size() >= 2 && fields[1] == index)
                return true;
        }
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

    // HACK
    bool bluez_sink_stuck(const std::string& address) {
        if (address.empty())
            return false;
        return bluez_sink_stuck_in(run("pactl list short sinks"), run("pactl list short sink-inputs"), address);
    }

    // HACK
    bool restart_bluez_card(const std::string& address) {
        if (address.empty())
            return false;
        const std::string card = "bluez_card." + sink_prefix(address).substr(sizeof("bluez_output.") - 1);
        const std::string profile = active_profile_in(run("pactl list cards"), card);
        if (profile.empty() || profile == "off")
            return false;
        bool ok = false;
        run(("pactl set-card-profile " + card + " off").c_str(), &ok);
        if (!ok)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(PROFILE_SETTLE_MS));
        run(("pactl set-card-profile " + card + " " + profile).c_str(), &ok);
        return ok;
    }

} // namespace tether::audio
