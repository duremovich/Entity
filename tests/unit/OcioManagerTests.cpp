/**
 * Unit tests for OcioManager.
 *
 * Covers the bundled-config happy path (subtask 1) and the fallback paths
 * (bogus file, identity config). No GPU or rendering dependencies — purely
 * exercises the OCIO::Config loader + accessors.
 */

#include <gtest/gtest.h>

#include "entity/color/OcioManager.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

using namespace entity;

namespace {

bool contains(const std::vector<std::string>& v, const std::string& needle) {
    return std::find(v.begin(), v.end(), needle) != v.end();
}

bool containsAnyOf(const std::vector<std::string>& v,
                   std::initializer_list<const char*> needles) {
    for (const char* n : needles) {
        if (contains(v, n)) return true;
    }
    return false;
}

} // namespace

TEST(OcioManagerTests, LoadsBuiltinConfigByDefault) {
    OcioManager mgr;
    bool ok = mgr.initialize();
    EXPECT_TRUE(ok) << "Built-in config should load successfully";
    EXPECT_TRUE(mgr.isUsingBuiltinConfig());
    EXPECT_FALSE(mgr.isUsingFallback());
    EXPECT_NE(mgr.getConfig(), nullptr);
    EXPECT_FALSE(mgr.configSource().empty());
}

TEST(OcioManagerTests, ListsACESStudioColorSpaces) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    auto cs = mgr.listColorSpaces();
    EXPECT_GT(cs.size(), 10u)
        << "ACES Studio Config should have dozens of color spaces, got "
        << cs.size();

    // Spot-check well-known names that any ACES-based config carries. Some
    // OCIO builds register the names with slight prefixes (e.g.
    // "ACES2065-1" vs "ACES - ACES2065-1"); accept either form.
    EXPECT_TRUE(containsAnyOf(cs, {"ACEScg", "ACES - ACEScg"}))
        << "Expected ACEScg color space";
    EXPECT_TRUE(containsAnyOf(cs, {"ACES2065-1", "ACES - ACES2065-1"}))
        << "Expected ACES2065-1 color space";
}

TEST(OcioManagerTests, HasDisplaysAndDefaultView) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());

    auto displays = mgr.listDisplays();
    ASSERT_GT(displays.size(), 0u) << "Built-in config must list at least one display";

    std::string defDisplay = mgr.getDefaultDisplay();
    EXPECT_FALSE(defDisplay.empty());
    EXPECT_TRUE(contains(displays, defDisplay))
        << "Default display '" << defDisplay << "' should be in the displays list";

    auto views = mgr.listViews(defDisplay);
    EXPECT_GT(views.size(), 0u);

    std::string defView = mgr.getDefaultView(defDisplay);
    EXPECT_FALSE(defView.empty());
    EXPECT_TRUE(contains(views, defView));
}

// Use an under-the-temp-dir guaranteed-missing filename. Avoids Windows'
// ~20s SMB/drive-letter retry on \\bogus\share\... or Z:\... lookups.
static std::string makeMissingOcioPath(const char* tag) {
    auto dir = std::filesystem::temp_directory_path();
    return (dir / (std::string("entity_ocio_missing_") + tag + ".ocio")).string();
}

TEST(OcioManagerTests, FallsBackOnMissingConfigPath) {
    OcioManager mgr;
    bool ok = mgr.initialize(makeMissingOcioPath("a"));
    EXPECT_FALSE(ok) << "Bogus path should fail to load";
    EXPECT_TRUE(mgr.isUsingFallback());
    EXPECT_FALSE(mgr.isUsingBuiltinConfig());
    EXPECT_NE(mgr.getConfig(), nullptr) << "Fallback identity config still non-null";
}

TEST(OcioManagerTests, ReinitializeReplacesConfig) {
    OcioManager mgr;
    ASSERT_TRUE(mgr.initialize());
    ASSERT_TRUE(mgr.isUsingBuiltinConfig());

    bool ok = mgr.initialize(makeMissingOcioPath("b"));
    EXPECT_FALSE(ok);
    EXPECT_TRUE(mgr.isUsingFallback());
    EXPECT_FALSE(mgr.isUsingBuiltinConfig())
        << "Re-init must clear the previous builtin flag";
}
