/**
 * Unit tests for entity::Settings load/save.
 *
 * Uses a temp directory + the explicit-path overloads so the test can't
 * touch the user's real %APPDATA%/Entity/settings.json.
 */

#include <gtest/gtest.h>

#include "entity/core/Settings.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeTempPath(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path()
         / ("entity_settings_test_" + std::string(tag) + "_" + std::to_string(stamp) + ".json");
}

struct TempFile {
    fs::path path;
    explicit TempFile(const char* tag) : path(makeTempPath(tag)) {}
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
        fs::remove(path.string() + ".tmp", ec); // belt-and-braces
    }
};

} // namespace

TEST(Settings, DefaultsHaveExpectedFrameCacheBudget) {
    entity::Settings s;
    // Default bumped from 512 MiB → 2 GiB on 2026-05-23 to size the cache
    // for 4K H.264 multi-clip workloads (2× 4K crossfade ≈ 594 MB, which
    // thrashed at 512 MiB). Original plan was Decision 2 of
    // ~/.claude/plans/so-even-with-hap-cosmic-glacier.md. Test gates the
    // contract so a future refactor can't quietly drop the budget.
    EXPECT_EQ(s.frameCacheBytes, 2048ull * 1024ull * 1024ull);
}

TEST(Settings, MissingFileReturnsDefaults) {
    TempFile tf("missing");
    ASSERT_FALSE(fs::exists(tf.path));

    auto loaded = entity::loadSettings(tf.path);
    EXPECT_EQ(loaded.frameCacheBytes, 2048ull * 1024ull * 1024ull);
}

TEST(Settings, RoundTripPreservesValue) {
    TempFile tf("roundtrip");

    entity::Settings written;
    written.frameCacheBytes = 2ull * 1024ull * 1024ull * 1024ull; // 2 GiB
    ASSERT_TRUE(entity::saveSettings(written, tf.path));
    ASSERT_TRUE(fs::exists(tf.path));

    auto loaded = entity::loadSettings(tf.path);
    EXPECT_EQ(loaded.frameCacheBytes, written.frameCacheBytes);
}

TEST(Settings, CorruptJsonFallsBackToDefaults) {
    TempFile tf("corrupt");
    {
        std::ofstream out(tf.path);
        out << "{ this is not valid json ;;;";
    }
    ASSERT_TRUE(fs::exists(tf.path));

    auto loaded = entity::loadSettings(tf.path);
    EXPECT_EQ(loaded.frameCacheBytes, 2048ull * 1024ull * 1024ull);
}

TEST(Settings, MissingKeyKeepsDefault) {
    // Forward-compat: a settings.json written by an older build (no
    // frameCacheBytes key) should load with the default value.
    TempFile tf("missingkey");
    {
        std::ofstream out(tf.path);
        out << R"({"version": 1})";
    }
    auto loaded = entity::loadSettings(tf.path);
    EXPECT_EQ(loaded.frameCacheBytes, 2048ull * 1024ull * 1024ull);
}

TEST(Settings, WrongTypedKeyKeepsDefault) {
    // A field whose JSON type is wrong (string instead of unsigned int) must
    // not throw or silently coerce — load defensively, fall back to default.
    TempFile tf("wrongtype");
    {
        std::ofstream out(tf.path);
        out << R"({"frameCacheBytes": "not a number"})";
    }
    auto loaded = entity::loadSettings(tf.path);
    EXPECT_EQ(loaded.frameCacheBytes, 2048ull * 1024ull * 1024ull);
}

TEST(Settings, SaveCreatesParentDir) {
    auto dirStamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = fs::temp_directory_path() / ("entity_settings_test_dir_" + std::to_string(dirStamp));
    auto path = dir / "nested" / "settings.json";

    // Best-effort cleanup before; deferred RAII would be nicer but the
    // structure is one-off enough to inline.
    std::error_code ec;
    fs::remove_all(dir, ec);
    ASSERT_FALSE(fs::exists(dir));

    entity::Settings s;
    EXPECT_TRUE(entity::saveSettings(s, path));
    EXPECT_TRUE(fs::exists(path));

    fs::remove_all(dir, ec); // cleanup
}

TEST(Settings, SettingsPathReturnsAbsolute) {
    // Whatever platform we're on, settingsPath() should return an absolute
    // path inside a per-user config directory. We don't pin the exact value
    // (it's user/platform dependent) but we do gate that it's not relative
    // and ends in settings.json.
    auto p = entity::settingsPath();
    EXPECT_TRUE(p.is_absolute());
    EXPECT_EQ(p.filename().string(), "settings.json");
}

// ---------------------------------------------------------------------------
// Phase C.12 #7 — OCIO color settings.
// ---------------------------------------------------------------------------

TEST(Settings, OcioFieldsHaveExpectedDefaults) {
    entity::Settings s;
    EXPECT_TRUE(s.ocioConfigPath.empty());
    EXPECT_EQ(s.defaultVideoInputCs, "Linear Rec.709 (sRGB)");
    EXPECT_EQ(s.defaultPngInputCs,   "sRGB - Display");
    EXPECT_TRUE(s.defaultDisplay.empty());
    EXPECT_TRUE(s.defaultView.empty());
}

TEST(Settings, OcioFieldsRoundTrip) {
    TempFile tf("ocio_roundtrip");

    entity::Settings written;
    written.ocioConfigPath      = "C:/configs/custom.ocio";
    written.defaultVideoInputCs = "ACEScg";
    written.defaultPngInputCs   = "Linear Rec.709 (sRGB)";
    written.defaultDisplay      = "Rec.1886";
    written.defaultView         = "Rec.709";
    ASSERT_TRUE(entity::saveSettings(written, tf.path));

    auto loaded = entity::loadSettings(tf.path);
    EXPECT_EQ(loaded.ocioConfigPath,      written.ocioConfigPath);
    EXPECT_EQ(loaded.defaultVideoInputCs, written.defaultVideoInputCs);
    EXPECT_EQ(loaded.defaultPngInputCs,   written.defaultPngInputCs);
    EXPECT_EQ(loaded.defaultDisplay,      written.defaultDisplay);
    EXPECT_EQ(loaded.defaultView,         written.defaultView);
}

TEST(Settings, OcioFieldsMissingKeysKeepDefaults) {
    // Forward-compat: a settings.json from a build older than C.12 #7 has
    // no OCIO keys. Loading must not throw and must leave defaults intact.
    TempFile tf("ocio_missingkeys");
    {
        std::ofstream out(tf.path);
        out << R"({"version": 1, "frameCacheBytes": 268435456})";
    }
    auto loaded = entity::loadSettings(tf.path);
    EXPECT_EQ(loaded.frameCacheBytes,     268435456ull);
    EXPECT_TRUE(loaded.ocioConfigPath.empty());
    EXPECT_EQ(loaded.defaultVideoInputCs, "Linear Rec.709 (sRGB)");
    EXPECT_EQ(loaded.defaultPngInputCs,   "sRGB - Display");
    EXPECT_TRUE(loaded.defaultDisplay.empty());
    EXPECT_TRUE(loaded.defaultView.empty());
}

TEST(Settings, OcioFieldsWrongTypeKeepsDefaults) {
    TempFile tf("ocio_wrongtype");
    {
        std::ofstream out(tf.path);
        out << R"({"defaultVideoInputCs": 42, "defaultDisplay": false})";
    }
    auto loaded = entity::loadSettings(tf.path);
    EXPECT_EQ(loaded.defaultVideoInputCs, "Linear Rec.709 (sRGB)");
    EXPECT_TRUE(loaded.defaultDisplay.empty());
}

TEST(Settings, ActiveSettingsSnapshotPublishAndRead) {
    // The decoder threads consult activeSettings() — it must read whatever
    // Engine last published. Mutex-guarded copy semantics; the snapshot is
    // independent of the original after publish.
    entity::Settings s;
    s.defaultVideoInputCs = "ACEScg";
    s.defaultPngInputCs   = "Raw";
    entity::publishActiveSettings(s);

    auto snap = entity::activeSettings();
    EXPECT_EQ(snap.defaultVideoInputCs, "ACEScg");
    EXPECT_EQ(snap.defaultPngInputCs,   "Raw");

    // Mutating the original after publish doesn't affect the snapshot
    // (publishActiveSettings stores a copy).
    s.defaultVideoInputCs = "MUTATED";
    auto snap2 = entity::activeSettings();
    EXPECT_EQ(snap2.defaultVideoInputCs, "ACEScg");

    // Restore defaults so other tests aren't affected by leakage.
    entity::publishActiveSettings(entity::Settings{});
}
