#include "scoped_env.hpp"
#include <gtest/gtest.h>
#include <tether/file_transfer.hpp>
#include <tether/base64.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

class FileTransferTest : public ::testing::Test {
protected:
    void SetUp() override {
        setenv("HOME", home_.c_str(), 1);
        std::filesystem::remove_all(home_);
        std::filesystem::create_directories(home_ / "Downloads");
    }

    void TearDown() override {
        std::filesystem::remove_all(home_);
    }

    const std::filesystem::path home_ =
        std::filesystem::temp_directory_path() / ("tether-tests-" + std::to_string(getpid()));
};

TEST_F(FileTransferTest, AbsoluteDownloadOverrideWins) {
    tether::testing::ScopedEnv downloads("XDG_DOWNLOAD_DIR", home_ / "inbox");
    tether::FileReceiveManager mgr;
    ASSERT_TRUE(mgr.handle_start("override", "hello.txt", 3));
    ASSERT_TRUE(mgr.handle_chunk("override", 0, tether::base64_encode((const unsigned char*)"abc", 3)));
    ASSERT_TRUE(mgr.handle_end("override"));
    EXPECT_TRUE(std::filesystem::exists(home_ / "inbox/hello.txt"));
    EXPECT_FALSE(std::filesystem::exists(home_ / "Downloads/hello.txt"));
}

TEST_F(FileTransferTest, InvalidDownloadOverrideFallsBack) {
    for (const std::string value : {"", "relative/inbox"}) {
        tether::testing::ScopedEnv downloads("XDG_DOWNLOAD_DIR", value);
        tether::FileReceiveManager mgr;
        ASSERT_TRUE(mgr.handle_start("fallback", "fallback.txt", 0));
        ASSERT_TRUE(mgr.handle_end("fallback"));
        EXPECT_TRUE(std::filesystem::exists(home_ / "Downloads/fallback.txt"));
        std::filesystem::remove(home_ / "Downloads/fallback.txt");
    }
}

TEST_F(FileTransferTest, SingleChunkTransfer) {
    tether::FileReceiveManager mgr;
    
    std::string t_id = "test_trans_1";
    mgr.handle_start(t_id, "hello.txt", 11);
    
    std::string content = "hello world";
    std::string b64 = tether::base64_encode(reinterpret_cast<const unsigned char*>(content.data()), content.size());
    
    mgr.handle_chunk(t_id, 0, b64);
    EXPECT_TRUE(mgr.handle_end(t_id));
    
    // Ensure output matches native directory targeting logic
    EXPECT_TRUE(std::filesystem::exists(home_ / "Downloads/hello.txt"));
    
    std::ifstream iff(home_ / "Downloads/hello.txt");
    std::string res;
    std::getline(iff, res);
    EXPECT_EQ(res, "hello world");
}

TEST_F(FileTransferTest, NameDeduplicatorAppendsCounterGracefully) {
    tether::FileReceiveManager mgr;
    
    // First transmission
    std::string t_id1 = "test_trans_1";
    mgr.handle_start(t_id1, "image.png", 3);
    mgr.handle_chunk(t_id1, 0, tether::base64_encode((const unsigned char*)"ABC", 3));
    mgr.handle_end(t_id1);
    
    // Identical name collision transmission
    std::string t_id2 = "test_trans_2";
    mgr.handle_start(t_id2, "image.png", 3);
    mgr.handle_chunk(t_id2, 0, tether::base64_encode((const unsigned char*)"DEF", 3));
    mgr.handle_end(t_id2);
    
    EXPECT_TRUE(std::filesystem::exists(home_ / "Downloads/image.png"));
    EXPECT_TRUE(std::filesystem::exists(home_ / "Downloads/image(1).png"));
    
    // Verify isolated integrity natively
    std::ifstream i2(home_ / "Downloads/image(1).png");
    std::string r2;
    std::getline(i2, r2);
    EXPECT_EQ(r2, "DEF");
}

TEST_F(FileTransferTest, MultiChunkTransferConcatenates) {
    tether::FileReceiveManager mgr;

    std::string t_id = "multi";
    ASSERT_TRUE(mgr.handle_start(t_id, "parts.txt", 6));
    EXPECT_TRUE(mgr.handle_chunk(t_id, 0, tether::base64_encode((const unsigned char*)"abc", 3)));
    EXPECT_TRUE(mgr.handle_chunk(t_id, 1, tether::base64_encode((const unsigned char*)"def", 3)));
    EXPECT_TRUE(mgr.handle_end(t_id));

    std::ifstream iff(home_ / "Downloads/parts.txt");
    std::string res;
    std::getline(iff, res);
    EXPECT_EQ(res, "abcdef");
}

TEST_F(FileTransferTest, RejectsChunkPastDeclaredSize) {
    tether::FileReceiveManager mgr;

    std::string t_id = "overflow";
    ASSERT_TRUE(mgr.handle_start(t_id, "small.bin", 3));

    // Peer declared 3 bytes but streams 8. The extra must not be written.
    EXPECT_FALSE(mgr.handle_chunk(t_id, 0, tether::base64_encode((const unsigned char*)"AAAABBBB", 8)));
    EXPECT_FALSE(mgr.handle_end(t_id)) << "aborted transfer must not report success";
    EXPECT_FALSE(std::filesystem::exists(home_ / "Downloads/small.bin"));
}

TEST_F(FileTransferTest, TruncatedTransferIsNotReportedAsSuccess) {
    tether::FileReceiveManager mgr;

    std::string t_id = "short";
    ASSERT_TRUE(mgr.handle_start(t_id, "short.bin", 10));
    EXPECT_TRUE(mgr.handle_chunk(t_id, 0, tether::base64_encode((const unsigned char*)"ABC", 3)));

    // Only 3 of 10 bytes arrived; this used to return true and leave a partial file.
    EXPECT_FALSE(mgr.handle_end(t_id));
    EXPECT_FALSE(std::filesystem::exists(home_ / "Downloads/short.bin"));
}

TEST_F(FileTransferTest, DuplicateTransferIdIsRejected) {
    tether::FileReceiveManager mgr;

    EXPECT_TRUE(mgr.handle_start("dup", "a.txt", 3));
    EXPECT_FALSE(mgr.handle_start("dup", "b.txt", 3));
}

TEST_F(FileTransferTest, UnknownTransferIdIsRejected) {
    tether::FileReceiveManager mgr;

    EXPECT_FALSE(mgr.handle_chunk("nope", 0, tether::base64_encode((const unsigned char*)"AB", 2)));
    EXPECT_FALSE(mgr.handle_end("nope"));
}

TEST_F(FileTransferTest, PathTraversalInFilenameStaysInDownloads) {
    tether::FileReceiveManager mgr;

    ASSERT_TRUE(mgr.handle_start("trav", "../../../etc/tether_pwned", 3));
    ASSERT_TRUE(mgr.handle_chunk("trav", 0, tether::base64_encode((const unsigned char*)"xyz", 3)));
    ASSERT_TRUE(mgr.handle_end("trav"));

    EXPECT_TRUE(std::filesystem::exists(home_ / "Downloads/tether_pwned"));
    EXPECT_FALSE(std::filesystem::exists(home_ / "etc/tether_pwned"));
}
