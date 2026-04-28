/**
 * Unit tests for entity::ProjectSerializer.
 *
 * Phase C.12 #8 added OutputDisplay.ocioDisplay + ocioView and bumped
 * PROJECT_VERSION 5 → 6. These tests gate two contracts:
 *   - v6 round-trip preserves the OCIO display/view fields.
 *   - v5 (and older) project files load with empty OCIO display/view —
 *     i.e. the loader treats absence as "use the active config's default
 *     at draw time", not a hard error.
 */

#include <gtest/gtest.h>

#include "entity/components/OutputDisplay.hpp"
#include "entity/project/ProjectSerializer.hpp"
#include "entity/timeline/Timeline.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeTempProjectPath(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path()
         / ("entity_project_test_" + std::string(tag) + "_" + std::to_string(stamp) + ".entity");
}

struct TempFile {
    fs::path path;
    explicit TempFile(const char* tag) : path(makeTempProjectPath(tag)) {}
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

// Locate the saved OutputDisplay entity by name. Returns nullptr if missing.
entity::OutputDisplay* findOutputByName(entt::registry& reg, const std::string& name) {
    auto view = reg.view<entity::OutputDisplay>();
    for (auto [e, out] : view.each()) {
        if (out.name == name) return &out;
    }
    return nullptr;
}

} // namespace

TEST(ProjectSerializer, OcioDisplayAndViewRoundTrip) {
    TempFile tf("ocio_roundtrip");

    // --- Save phase ---
    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        entt::entity outEntity = registry.create();
        auto& out = registry.emplace<entity::OutputDisplay>(outEntity);
        out.name = "Stage Projector";
        out.ocioDisplay = "Rec.1886";
        out.ocioView = "Rec.709";

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
        ASSERT_TRUE(fs::exists(tf.path));
    }

    // --- Load phase (fresh registry) ---
    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        auto* loaded = findOutputByName(registry, "Stage Projector");
        ASSERT_NE(loaded, nullptr) << "Output 'Stage Projector' missing after load";
        EXPECT_EQ(loaded->ocioDisplay, "Rec.1886");
        EXPECT_EQ(loaded->ocioView, "Rec.709");
    }
}

TEST(ProjectSerializer, EmptyOcioFieldsRoundTripAsEmpty) {
    // Default-constructed OutputDisplay leaves ocioDisplay/ocioView empty
    // (= "use the OCIO config's default display+view at draw time"). Make
    // sure that signal survives a round trip — we don't want the loader to
    // accidentally fill in a literal "" → "(config default)" string or vice
    // versa.
    TempFile tf("ocio_empty_roundtrip");

    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        entt::entity outEntity = registry.create();
        auto& out = registry.emplace<entity::OutputDisplay>(outEntity);
        out.name = "Default Output";
        // ocioDisplay / ocioView intentionally left empty.

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path));
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path));

        auto* loaded = findOutputByName(registry, "Default Output");
        ASSERT_NE(loaded, nullptr);
        EXPECT_TRUE(loaded->ocioDisplay.empty());
        EXPECT_TRUE(loaded->ocioView.empty());
    }
}

TEST(ProjectSerializer, V5ProjectLoadsWithEmptyOcioFields) {
    // Forward-compatibility gate: a v5 project file (pre-Phase C.12 #8) has
    // no ocioDisplay / ocioView keys on its outputs. The loader must accept
    // the missing keys and resolve them to empty strings, which downstream
    // code treats as "use the OCIO config default".
    TempFile tf("v5_no_ocio");

    {
        std::ofstream out(tf.path);
        out << R"({
  "format": "entity_project",
  "version": 5,
  "frameRate": 30,
  "duration": 0,
  "currentFrame": 0,
  "tracks": [],
  "outputs": [
    {
      "name": "Legacy Output",
      "outputIndex": 0,
      "type": 0,
      "enabled": false,
      "width": 1920,
      "height": 1080,
      "refreshRate": 60.0,
      "physicalDisplayIndex": -1,
      "deviceName": "",
      "displayName": "",
      "brightness": 1.0,
      "gamma": 1.0,
      "fullscreen": false,
      "windowX": 0,
      "windowY": 0,
      "windowWidth": 1280,
      "windowHeight": 720,
      "inputRegion": { "x": 0.0, "y": 0.0, "width": 1.0, "height": 1.0 },
      "sourceScreenName": ""
    }
  ]
})";
    }
    ASSERT_TRUE(fs::exists(tf.path));

    entt::registry registry;
    entity::Timeline timeline(registry);

    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();

    auto* loaded = findOutputByName(registry, "Legacy Output");
    ASSERT_NE(loaded, nullptr) << "Legacy v5 output failed to load";
    EXPECT_TRUE(loaded->ocioDisplay.empty())
        << "v5 file had no ocioDisplay key; loader must default to empty";
    EXPECT_TRUE(loaded->ocioView.empty())
        << "v5 file had no ocioView key; loader must default to empty";
}
