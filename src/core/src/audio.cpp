#include "tether/audio.hpp"

#include <tether/log.hpp>

#include <chrono>
#include <glib.h>
#include <sstream>
#include <thread>

namespace tether::audio {

    namespace {

        constexpr int POLL_INTERVAL_MS = 200;

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

} // namespace tether::audio
