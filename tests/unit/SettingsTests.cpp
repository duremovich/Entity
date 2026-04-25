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
    // Plan calls for 512 MiB default per Decision 2 in
    // ~/.claude/plans/so-even-with-hap-cosmic-glacier.md. Test gates the
    // contract so a future refactor can't quietly drop the budget.
    EXPECT_EQ(s.frameCacheBytes, 512ull * 1024ull * 1024ull);
}

TEST(Settings, MissingFileReturnsDefaults) {
    TempFile tf("missing");
    ASSERT_FALSE(fs::exists(tf.path));

    auto loaded = entity::loadSettings(tf.path);
    EXPECT_EQ(loaded.frameCacheBytes, 512ull * 1024ull * 1024ull);
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
    EXPECT_EQ(loaded.frameCacheBytes, 512ull * 1024ull * 1024ull);
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
    EXPECT_EQ(loaded.frameCacheBytes, 512ull * 1024ull * 1024ull);
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
    EXPECT_EQ(loaded.frameCacheBytes, 512ull * 1024ull * 1024ull);
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
