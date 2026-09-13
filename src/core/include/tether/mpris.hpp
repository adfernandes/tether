#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tether {

    // Pauses whatever is playing on the desktop and can put it back, over MPRIS on the session bus.
    class MediaControl {
    public:
        MediaControl();
        ~MediaControl();

        MediaControl(const MediaControl&) = delete;
        MediaControl& operator=(const MediaControl&) = delete;

        // Pauses every player currently reporting Playing and remembers which
        // ones, so resume() can put them back. Returns how many were
        // paused. Does nothing while already holding a pause.
        size_t pause();

        // Plays what this object paused, then forgets them.
        size_t resume();

        // Play/pause for a button press: pauses what is playing, otherwise plays what this object
        // paused, otherwise the players the last toggle paused, otherwise the first paused player.
        // Returns how many players it touched.
        size_t toggle();

        // Whether anything is currently paused by this object.
        bool holding() const;

        // Drops the remembered players without touching them.
        void forget();

        // Bus names of every MPRIS player currently reporting Playing.
        std::vector<std::string> playing() const;

        // Calls `on_change` on a thread of its own each time a player reports a new PlaybackStatus,
        // with the player's unique bus name. Players that leave PlaybackStatus empty are not seen.
        // Call once.
        void watch(std::function<void(const std::string& player, bool playing)> on_change);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // Set for the daemon's lifetime
    extern MediaControl* g_media;

} // namespace tether
