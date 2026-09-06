#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <sstream>
#include <tether/crypto.hpp>
#include <unistd.h>

class CryptoTest : public ::testing::Test {
protected:
    void SetUp() override {
        setenv("HOME", home_.c_str(), 1);
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_);
        // Crypto caches its contexts and paths, and init() short-circuits on them.
        // Without this every test after the first runs against a config directory
        // that SetUp deleted: the writes fail and the assertions that read from
        // disk pass against nothing.
        tether::Crypto::instance().reset_for_tests();
    }

    void TearDown() override { std::filesystem::remove_all(home_); }

    const std::filesystem::path home_ =
        std::filesystem::temp_directory_path() / ("tether-tests-" + std::to_string(getpid()));
};

TEST_F(CryptoTest, CompleteCryptographicLifecycle) {
    EXPECT_TRUE(tether::Crypto::instance().init());

    EXPECT_TRUE(std::filesystem::exists(home_ / ".config/tether/key.pem"));
    EXPECT_TRUE(std::filesystem::exists(home_ / ".config/tether/cert.pem"));
    EXPECT_TRUE(std::filesystem::exists(home_ / ".config/tether/known_hosts.json"));

    EXPECT_NE(tether::Crypto::instance().get_server_context(), nullptr);
    EXPECT_NE(tether::Crypto::instance().get_client_context(), nullptr);

    std::string test_fp = "012345abcdef";
    EXPECT_FALSE(tether::Crypto::instance().is_host_known(test_fp));

    tether::Crypto::instance().add_known_host("Unit Test Device", test_fp);
    EXPECT_TRUE(tether::Crypto::instance().is_host_known(test_fp));

    std::string dump = tether::Crypto::instance().get_known_hosts_dump();
    EXPECT_NE(dump.find("Unit Test Device"), std::string::npos);
}

TEST_F(CryptoTest, KnownHostsPersistsAcrossInit) {
    EXPECT_TRUE(tether::Crypto::instance().init());

    tether::Crypto::instance().add_known_host("Persistent Device", "persistent_fp");
    EXPECT_TRUE(tether::Crypto::instance().is_host_known("persistent_fp"));

    std::string dump = tether::Crypto::instance().get_known_hosts_dump();
    EXPECT_NE(dump.find("Persistent Device"), std::string::npos);
}

TEST_F(CryptoTest, MultipleKnownHosts) {
    EXPECT_TRUE(tether::Crypto::instance().init());

    tether::Crypto::instance().add_known_host("Device1", "fp1");
    tether::Crypto::instance().add_known_host("Device2", "fp2");
    tether::Crypto::instance().add_known_host("Device3", "fp3");

    EXPECT_TRUE(tether::Crypto::instance().is_host_known("fp1"));
    EXPECT_TRUE(tether::Crypto::instance().is_host_known("fp2"));
    EXPECT_TRUE(tether::Crypto::instance().is_host_known("fp3"));
    EXPECT_FALSE(tether::Crypto::instance().is_host_known("fp4"));
}

TEST_F(CryptoTest, RemoveKnownHost) {
    EXPECT_TRUE(tether::Crypto::instance().init());

    tether::Crypto::instance().add_known_host("Doomed Device", "doomed_fp");
    EXPECT_TRUE(tether::Crypto::instance().is_host_known("doomed_fp"));

    EXPECT_TRUE(tether::Crypto::instance().remove_known_host("doomed_fp"));
    EXPECT_FALSE(tether::Crypto::instance().is_host_known("doomed_fp"));
    EXPECT_EQ(tether::Crypto::instance().get_known_hosts_dump().find("Doomed Device"), std::string::npos);

    // Idempotent, and the removal survives a reload from disk.
    EXPECT_FALSE(tether::Crypto::instance().remove_known_host("doomed_fp"));

    const auto hosts = home_ / ".config/tether/known_hosts.json";
    ASSERT_TRUE(std::filesystem::exists(hosts)) << "nothing was written, so the check below proves nothing";
    std::ifstream iff(hosts);
    std::stringstream on_disk;
    on_disk << iff.rdbuf();
    EXPECT_EQ(on_disk.str().find("doomed_fp"), std::string::npos);
}

TEST_F(CryptoTest, UnreadableKnownHostsIsNotOverwritten) {
    if (geteuid() == 0)
        GTEST_SKIP() << "root reads regardless of mode bits";

    EXPECT_TRUE(tether::Crypto::instance().init());
    tether::Crypto::instance().add_known_host("Precious Device", "precious_fp");

    // An unreadable file must never be rewritten: that would silently wipe every pairing.
    const auto hosts = home_ / ".config/tether/known_hosts.json";
    std::filesystem::permissions(hosts, std::filesystem::perms::none);
    std::filesystem::last_write_time(hosts, std::filesystem::file_time_type::clock::now());

    EXPECT_FALSE(tether::Crypto::instance().is_host_known("precious_fp"));

    std::filesystem::permissions(hosts, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    std::ifstream iff(hosts);
    std::stringstream on_disk;
    on_disk << iff.rdbuf();
    EXPECT_NE(on_disk.str().find("precious_fp"), std::string::npos);
}

TEST_F(CryptoTest, GetMyFingerprint) {
    EXPECT_TRUE(tether::Crypto::instance().init());
    std::string fp = tether::Crypto::instance().get_my_fingerprint();
    EXPECT_FALSE(fp.empty());
    EXPECT_EQ(fp.size(), 64);
}
