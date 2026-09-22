#include <gtest/gtest.h>
#include <tether/bluetooth/monitor.hpp>

#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/types.h>
#include <unistd.h>

using namespace std::chrono_literals;
using tether::bluetooth::probe_secure_connections;

namespace {

    class ScopedEnvironment {
    public:
        ScopedEnvironment(const char* name, const char* value) : name_(name) {
            if (const char* current = std::getenv(name))
                old_value_ = current;
            if (value)
                setenv(name, value, 1);
            else
                unsetenv(name);
        }

        ~ScopedEnvironment() {
            if (old_value_)
                setenv(name_.c_str(), old_value_->c_str(), 1);
            else
                unsetenv(name_.c_str());
        }

        ScopedEnvironment(const ScopedEnvironment&) = delete;
        ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    private:
        std::string name_;
        std::optional<std::string> old_value_;
    };

    class FakeBtmgmt {
    public:
        explicit FakeBtmgmt(const std::string& body) {
            const char* path = std::getenv("PATH");
            old_path_ = path ? path : "";
            dir_ = std::filesystem::temp_directory_path() / ("tether-monitor-test-" + std::to_string(getpid()));
            std::filesystem::remove_all(dir_);
            std::filesystem::create_directories(dir_);

            const auto executable = dir_ / "btmgmt";
            std::ofstream script(executable);
            script << "#!/bin/sh\n" << body;
            script.close();
            std::filesystem::permissions(executable,
                                         std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                                             std::filesystem::perms::owner_exec,
                                         std::filesystem::perm_options::replace);
            setenv("PATH", (dir_.string() + ":" + old_path_).c_str(), 1);
        }

        ~FakeBtmgmt() {
            setenv("PATH", old_path_.c_str(), 1);
            std::filesystem::remove_all(dir_);
        }

        FakeBtmgmt(const FakeBtmgmt&) = delete;
        FakeBtmgmt& operator=(const FakeBtmgmt&) = delete;

    private:
        std::filesystem::path dir_;
        std::string old_path_;
    };

} // namespace

TEST(ContainerCapabilityHints, ReportsHostSecureConnectionsWhenManagementProbeIsUnavailable) {
    {
        ScopedEnvironment secure("TETHER_BLUEZ_SECURE_CONNECTIONS", "true");
        FakeBtmgmt fake("exit 1\n");
        EXPECT_EQ(probe_secure_connections("hci0", 250ms), true);
    }
    {
        ScopedEnvironment secure("TETHER_BLUEZ_SECURE_CONNECTIONS", "false");
        FakeBtmgmt fake("exit 1\n");
        EXPECT_EQ(probe_secure_connections("hci0", 250ms), false);
    }
}

TEST(ContainerCapabilityHints, LocalManagementProbeTakesPrecedence) {
    {
        ScopedEnvironment secure("TETHER_BLUEZ_SECURE_CONNECTIONS", "true");
        FakeBtmgmt fake("printf 'current settings: powered bondable\\n'\n");
        EXPECT_EQ(probe_secure_connections("hci0", 250ms), false);
    }
    {
        ScopedEnvironment secure("TETHER_BLUEZ_SECURE_CONNECTIONS", "false");
        FakeBtmgmt fake("printf 'current settings: powered secure-conn bondable\\n'\n");
        EXPECT_EQ(probe_secure_connections("hci0", 250ms), true);
    }
}

TEST(SecureConnectionsProbe, ParsesEnabledAndDisabledSettings) {
    {
        FakeBtmgmt fake("printf 'current settings: powered secure-conn bondable\\n'\n");
        const auto result = probe_secure_connections("hci0", 250ms);
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(*result);
    }
    {
        FakeBtmgmt fake("printf 'current settings: powered bondable\\n'\n");
        const auto result = probe_secure_connections("hci0", 250ms);
        ASSERT_TRUE(result.has_value());
        EXPECT_FALSE(*result);
    }
}

TEST(SecureConnectionsProbe, TimesOutAndTerminatesTheExactChild) {
    FakeBtmgmt fake("exec sleep 5\n");
    const auto started = std::chrono::steady_clock::now();
    const auto result = probe_secure_connections("hci0", 50ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_FALSE(result.has_value());
    EXPECT_LT(elapsed, 500ms);
}

TEST(SecureConnectionsProbe, DoesNotLeakParentDescriptorsIntoBtmgmt) {
    constexpr int sentinel_fd = 99;
    const int source = open("/dev/null", O_RDONLY);
    ASSERT_GE(source, 0);
    ASSERT_EQ(dup2(source, sentinel_fd), sentinel_fd);
    close(source);

    FakeBtmgmt fake("if [ -e /proc/self/fd/99 ]; then exit 9; fi\n"
                    "printf 'current settings: secure-conn\\n'\n");
    const auto result = probe_secure_connections("hci0", 250ms);
    close(sentinel_fd);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result);
}

// btmgmt epolls its stdin before running the command and epoll rejects
// /dev/null with EPERM, so an inherited /dev/null fd 0 makes it hang.
TEST(SecureConnectionsProbe, GivesBtmgmtAPollableStdin) {
    const int devnull = open("/dev/null", O_RDONLY);
    ASSERT_GE(devnull, 0);
    const int saved_stdin = dup(STDIN_FILENO);
    ASSERT_GE(saved_stdin, 0);
    ASSERT_EQ(dup2(devnull, STDIN_FILENO), STDIN_FILENO);
    close(devnull);

    FakeBtmgmt fake("[ -p /proc/self/fd/0 ] || exit 9\n"
                    "printf 'current settings: secure-conn\\n'\n");
    const auto result = probe_secure_connections("hci0", 250ms);

    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(*result);
}
