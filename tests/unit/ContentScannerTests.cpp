#include "entity/project/ContentScanner.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using entity::ContentScanner;

namespace {

class ContentScannerTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_root = fs::temp_directory_path() / "entity_content_scanner_test" /
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fs::create_directories(m_root / "content" / "unsorted");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(m_root, ec);
    }

    void writeFile(const std::string& relUnderContent, std::size_t bytes = 16) {
        fs::path p = m_root / "content" / relUnderContent;
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary);
        std::string blob(bytes, 'x');
        f.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    }

    void deleteFile(const std::string& relUnderContent) {
        fs::remove(m_root / "content" / relUnderContent);
    }

    fs::path m_root;
};

}  // namespace

TEST_F(ContentScannerTest, FirstTickDoesNotEmitAdded) {
    writeFile("unsorted/intro.mov");

    ContentScanner s;
    s.start(m_root);
    // Avoid letting the worker thread tick — we'll drive it manually.
    s.stop();
    s.start(m_root);
    s.stop();
    // Now drive a single tick on this thread.
    ContentScanner s2;
    s2.start(m_root);
    s2.stop();
    s2.tickForTesting();
    auto deltas = s2.drain();
    // First sighting only — no Added yet (need two stable ticks).
    EXPECT_TRUE(deltas.empty());
}

TEST_F(ContentScannerTest, AddedAfterTwoStableTicks) {
    writeFile("unsorted/intro.mov");

    ContentScanner s;
    s.start(m_root);
    s.stop();  // we don't want the worker thread driving scans
    s.tickForTesting();           // tick 1: discover, no emit
    s.tickForTesting();           // tick 2: stable, emit Added
    auto deltas = s.drain();
    ASSERT_EQ(deltas.size(), 1u);
    EXPECT_EQ(deltas[0].kind, ContentScanner::DeltaKind::Added);
    EXPECT_EQ(deltas[0].relativePath, "content/unsorted/intro.mov");
}

TEST_F(ContentScannerTest, RemovedFiresAfterFileDeleted) {
    writeFile("unsorted/intro.mov");

    ContentScanner s;
    s.start(m_root);
    s.stop();
    s.tickForTesting();
    s.tickForTesting();
    (void)s.drain();  // consume the Added

    deleteFile("unsorted/intro.mov");
    s.tickForTesting();
    auto deltas = s.drain();
    ASSERT_EQ(deltas.size(), 1u);
    EXPECT_EQ(deltas[0].kind, ContentScanner::DeltaKind::Removed);
    EXPECT_EQ(deltas[0].relativePath, "content/unsorted/intro.mov");
}

TEST_F(ContentScannerTest, RemovedNotEmittedIfNeverReported) {
    writeFile("unsorted/intro.mov");

    ContentScanner s;
    s.start(m_root);
    s.stop();
    s.tickForTesting();  // tick 1: discover, no emit
    deleteFile("unsorted/intro.mov");
    s.tickForTesting();  // file vanished before second stable tick
    auto deltas = s.drain();
    EXPECT_TRUE(deltas.empty());
}

TEST_F(ContentScannerTest, IgnoresHiddenDirectories) {
    fs::create_directories(m_root / "content" / "unsorted" / ".archive");
    {
        std::ofstream f(m_root / "content" / "unsorted" / ".archive" / "old.mov");
        f << "blob";
    }

    ContentScanner s;
    s.start(m_root);
    s.stop();
    s.tickForTesting();
    s.tickForTesting();
    auto deltas = s.drain();
    EXPECT_TRUE(deltas.empty());  // .archive/ is skipped
}

TEST_F(ContentScannerTest, IgnoresNonMediaExtensions) {
    writeFile("unsorted/notes.txt");
    writeFile("unsorted/proj.entity");
    writeFile("unsorted/sidecar.json");

    ContentScanner s;
    s.start(m_root);
    s.stop();
    s.tickForTesting();
    s.tickForTesting();
    auto deltas = s.drain();
    EXPECT_TRUE(deltas.empty());
}

TEST_F(ContentScannerTest, IgnoresInFlightPatterns) {
    writeFile("unsorted/syncing.mov.tmp");
    writeFile("unsorted/copy.ffs_tmp");
    writeFile("unsorted/~office.mov");

    ContentScanner s;
    s.start(m_root);
    s.stop();
    s.tickForTesting();
    s.tickForTesting();
    auto deltas = s.drain();
    EXPECT_TRUE(deltas.empty());
}

TEST_F(ContentScannerTest, RecursesIntoSubdirectories) {
    writeFile("act1/intro.mov");
    writeFile("act2/outro.mov");

    ContentScanner s;
    s.start(m_root);
    s.stop();
    s.tickForTesting();
    s.tickForTesting();
    auto deltas = s.drain();
    ASSERT_EQ(deltas.size(), 2u);

    std::vector<std::string> rels;
    for (const auto& d : deltas) {
        EXPECT_EQ(d.kind, ContentScanner::DeltaKind::Added);
        rels.push_back(d.relativePath);
    }
    std::sort(rels.begin(), rels.end());
    EXPECT_EQ(rels[0], "content/act1/intro.mov");
    EXPECT_EQ(rels[1], "content/act2/outro.mov");
}

TEST_F(ContentScannerTest, MissingContentDirEmitsNothing) {
    // No content/ subdir exists yet.
    fs::remove_all(m_root / "content");

    ContentScanner s;
    s.start(m_root);
    s.stop();
    s.tickForTesting();
    auto deltas = s.drain();
    EXPECT_TRUE(deltas.empty());
}

TEST_F(ContentScannerTest, StartStopLifecycle) {
    ContentScanner s;
    EXPECT_FALSE(s.isRunning());
    s.start(m_root);
    EXPECT_TRUE(s.isRunning());
    s.stop();
    EXPECT_FALSE(s.isRunning());
    // Calling stop() again is a no-op.
    s.stop();
    EXPECT_FALSE(s.isRunning());
}
