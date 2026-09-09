#pragma once

#include <string>

namespace tether {

    // Locks the desktop session. With `command` empty this asks logind.
    bool lock_session(const std::string& command);

} // namespace tether
