#pragma once

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

        // Whether anything is currently paused by this object.
        bool holding() const;

        // Drops the remembered players without touching them.
        void forget();

        // Bus names of every MPRIS player currently reporting Playing.
        std::vector<std::string> playing() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // Set for the daemon's lifetime
    extern MediaControl* g_media;

} // namespace tether
