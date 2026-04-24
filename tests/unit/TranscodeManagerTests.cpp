/**
 * Unit tests for TranscodeWorker + TranscodeManager.
 *
 * Round-trip test re-transcodes the existing HAP fixture through the
 * manager (HAP → HAP Alpha). It's semantically a no-op but exercises the
 * full decode → sws → encode path, because the vcpkg FFmpeg build in CI
 * doesn't ship a PNG or image2 decoder (verified via probe_codecs: only
 * HAP / H264 / ProRes decode are present).
 *
 * Output is written into a temporary cache dir and torn down at end-of-test.
 *
 * Requires: FFmpeg with snappy feature; test_media/hap_gradient/test.mov.
 */

#include <gtest/gtest.h>

#include "entity/media/TranscodeManager.hpp"
#include "entity/media/HapFormat.hpp"
#include "entity/core/Types.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

using namespace entity;
namespace fs = std::filesystem;

namespace {

// Locate the repo root so tests run the same whether CTest invokes them
// from build/ or the user runs them from tests/. Walks up looking for the
// CMakeLists.txt at the project root.
fs::path findRepoRoot() {
    fs::path p = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(p / "CMakeLists.txt") && fs::exists(p / "test_media")) return p;
        if (p.has_parent_path() && p.parent_path() != p) p = p.parent_path();
        else break;
    }
    return fs::current_path();
}

struct TempCacheDir {
    fs::path path;
    TempCacheDir() {
        path = fs::temp_directory_path()
             / ("entity_hap_cache_test_"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempCacheDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// Wait up to `timeoutMs` for the worker's state to become Done or Failed.
TranscodeState waitForTerminal(const TranscodeManager& mgr,
                                const std::string& src,
                                int timeoutMs = 15000) {
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        auto st = mgr.statusOf(src);
        if (st && (st->state == TranscodeState::Done || st->state == TranscodeState::Failed)) {
            return st->state;
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs)) {
            return st ? st->state : TranscodeState::Failed;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

} // namespace

// --- Free helpers ----------------------------------------------------------

TEST(TranscodeManagerHelpers, IsHapMediaType) {
    EXPECT_TRUE(isHapMediaType(MediaType::VideoHAP));
    EXPECT_TRUE(isHapMediaType(MediaType::VideoHAPAlpha));
    EXPECT_TRUE(isHapMediaType(MediaType::VideoHAPQ));
    EXPECT_TRUE(isHapMediaType(MediaType::VideoHAPR));
    EXPECT_FALSE(isHapMediaType(MediaType::VideoProRes4444));
    EXPECT_FALSE(isHapMediaType(MediaType::PNGSequence));
    EXPECT_FALSE(isHapMediaType(MediaType::Unknown));
}

TEST(TranscodeManagerHelpers, ShortHashDeterministicAndWide) {
    auto a = shortHashOfPath("/some/path/file.mov");
    auto b = shortHashOfPath("/some/path/file.mov");
    auto c = shortHashOfPath("/some/path/other.mov");
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(a.size(), 8u);
}

TEST(TranscodeManagerHelpers, DeriveDstPathIncludesStemHashVariant) {
    TranscodeManager mgr;
    mgr.setCacheDir("/tmp/cache");
    auto dst = mgr.deriveDstPath("/home/user/my clip.mov", "hap_alpha");
    EXPECT_EQ(dst.parent_path(), fs::path("/tmp/cache"));
    const std::string name = dst.filename().string();
    EXPECT_NE(name.find("my clip_"), std::string::npos);       // stem preserved
    EXPECT_NE(name.find(".hap_alpha.mov"), std::string::npos); // variant + ext
}

TEST(TranscodeManagerHelpers, DeriveDstPathSanitizesImageSequencePercent) {
    TranscodeManager mgr;
    mgr.setCacheDir("/tmp/cache");
    auto dst = mgr.deriveDstPath("/m/gradient_seq/frame_%03d.png", "hap");
    // '%' is illegal in Windows filenames — we replace it with '_'.
    EXPECT_EQ(dst.filename().string().find('%'), std::string::npos);
}

// --- Full transcode round-trip --------------------------------------------

TEST(TranscodeManagerRoundTrip, HapToHapAlpha) {
    const fs::path repo = findRepoRoot();
    const fs::path srcFile = repo / "test_media" / "hap_gradient" / "test.mov";
    ASSERT_TRUE(fs::exists(srcFile))
        << "hap_gradient fixture missing at " << srcFile;

    TempCacheDir tmp;
    TranscodeManager mgr;
    mgr.setCacheDir(tmp.path);

    const std::string src = srcFile.string();
    const std::string dst = mgr.enqueue(src, "hap_alpha", 0.0);
    EXPECT_FALSE(dst.empty());
    EXPECT_TRUE(mgr.has(src));

    // Enqueuing the same src again must be idempotent.
    const std::string dst2 = mgr.enqueue(src, "hap_alpha", 0.0);
    EXPECT_EQ(dst, dst2);

    const TranscodeState terminal = waitForTerminal(mgr, src);
    auto snap = mgr.statusOf(src);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(terminal, TranscodeState::Done)
        << "result=" << ResultToString(snap->result);
    EXPECT_EQ(snap->result, Result::Success);
    EXPECT_EQ(snap->outputPath, dst);
    EXPECT_GT(snap->framesDone, 0);
    EXPECT_GE(snap->progress01, 0.0f);

    // Output is a real file, non-empty.
    ASSERT_TRUE(fs::exists(dst));
    EXPECT_GT(fs::file_size(dst), 256u);

    // Cleanup test — Done workers are reaped by clearDone().
    mgr.clearDone();
    EXPECT_FALSE(mgr.has(src));
}

TEST(TranscodeManagerRoundTrip, NonExistentSourceFailsCleanly) {
    TempCacheDir tmp;
    TranscodeManager mgr;
    mgr.setCacheDir(tmp.path);

    const std::string src = "does/not/exist_i_promise.mov";
    (void)mgr.enqueue(src);
    const TranscodeState terminal = waitForTerminal(mgr, src, /*timeoutMs*/ 5000);
    EXPECT_EQ(terminal, TranscodeState::Failed);
    auto snap = mgr.statusOf(src);
    ASSERT_TRUE(snap.has_value());
    EXPECT_NE(snap->result, Result::Success);
}

// Regression: failing workers must survive clearDone() so the MediaBin
// can keep showing the "Failed" state + Retry menu item. Previously
// clearFinished() reaped them immediately, causing a one-frame-flash
// bug where the UI fell back to the grey "Source" label.
TEST(TranscodeManagerRoundTrip, ClearDoneKeepsFailedWorkers) {
    TempCacheDir tmp;
    TranscodeManager mgr;
    mgr.setCacheDir(tmp.path);

    const std::string src = "still/does/not/exist.mov";
    (void)mgr.enqueue(src);
    EXPECT_EQ(waitForTerminal(mgr, src, 5000), TranscodeState::Failed);

    // clearDone() must NOT remove Failed workers.
    mgr.clearDone();
    auto snap = mgr.statusOf(src);
    ASSERT_TRUE(snap.has_value());
    EXPECT_EQ(snap->state, TranscodeState::Failed);

    // remove() explicitly wipes the entry regardless of state.
    mgr.remove(src);
    EXPECT_FALSE(mgr.has(src));
}
