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

#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ContentRouting.hpp"
#include "entity/components/Effect.hpp"
#include "entity/components/EffectAnimatedParameters.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/components/EffectParam.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/effects/EffectKind.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/OutputDisplay.hpp"
#include "entity/components/Projector.hpp"
#include "entity/components/Prop.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/project/ProjectManager.hpp"
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

// --- Phase C.12 #9 — MediaLibraryEntry.inputColorSpaceOverride -----------

TEST(ProjectSerializer, MediaLibraryInputColorSpaceOverrideRoundTrip) {
    // Round-trip: a MediaLibraryEntry with a non-empty override must preserve
    // its value across save+load.
    TempFile tf("media_input_cs_override");
    const std::string kPath = "C:/path/to/clip.mov";

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        auto& entry = pm.addMediaFile(kPath);
        entry.inputColorSpaceOverride = "Linear Rec.2020";

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path, &pm))
            << entity::ProjectSerializer::getLastError();
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path, nullptr, &pm))
            << entity::ProjectSerializer::getLastError();

        const auto* loaded = pm.findEntry(kPath);
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->inputColorSpaceOverride, "Linear Rec.2020");
    }
}

TEST(ProjectSerializer, MediaLibraryEmptyOverrideRoundTripsAsEmpty) {
    // An empty override stays empty (= "Auto (decoder)") across the round
    // trip. Save-side intentionally drops the key when empty so older parsers
    // and projects without the field stay byte-identical; load-side defaults
    // missing keys to empty.
    TempFile tf("media_input_cs_empty");
    const std::string kPath = "C:/path/to/another.mov";

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        pm.addMediaFile(kPath);  // override left empty
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path, &pm));
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path, nullptr, &pm));

        const auto* loaded = pm.findEntry(kPath);
        ASSERT_NE(loaded, nullptr);
        EXPECT_TRUE(loaded->inputColorSpaceOverride.empty());
    }
}

TEST(ProjectSerializer, MediaLibraryWithoutOverrideKeyLoadsAsEmpty) {
    // Forward-compatibility gate for existing v6 projects that pre-date
    // Phase C.12 #9: a project file whose mediaLibrary entries have no
    // `inputColorSpaceOverride` key must still load, with the override
    // defaulting to empty.
    TempFile tf("media_no_override_key");

    {
        std::ofstream out(tf.path);
        out << R"({
  "format": "entity_project",
  "version": 6,
  "frameRate": 30,
  "duration": 0,
  "currentFrame": 0,
  "tracks": [],
  "mediaLibrary": [
    { "originalPath": "C:/legacy/clip.mov", "transcodedPath": "", "variant": "" }
  ]
})";
    }
    ASSERT_TRUE(fs::exists(tf.path));

    entt::registry registry;
    entity::Timeline timeline(registry);
    entity::ProjectManager pm;
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path, nullptr, &pm))
        << entity::ProjectSerializer::getLastError();

    const auto* loaded = pm.findEntry("C:/legacy/clip.mov");
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->inputColorSpaceOverride.empty());
}

// --- ADR-0009 / v7 — structured projects: pathKind + archive fields ----

TEST(ProjectSerializer, MediaLibraryPathKindAndArchiveRoundTrip) {
    // A managed entry with archive metadata must survive a full save+load.
    // This is the live-path test for the structured-project model:
    // path_kind=managed, archived_original points at a per-folder
    // .archive/ entry, original_codec records what was archived.
    TempFile tf("v7_managed_with_archive");
    const std::string kPath = "content/act1/intro.mov";

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        auto& entry = pm.addMediaFile(kPath);
        entry.pathKind         = entity::ProjectManager::PathKind::Managed;
        entry.archivedOriginal = "content/act1/.archive/intro.mov";
        entry.originalCodec    = "prores4444";

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path, &pm))
            << entity::ProjectSerializer::getLastError();
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path, nullptr, &pm))
            << entity::ProjectSerializer::getLastError();

        const auto* loaded = pm.findEntry(kPath);
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->pathKind, entity::ProjectManager::PathKind::Managed);
        EXPECT_EQ(loaded->archivedOriginal, "content/act1/.archive/intro.mov");
        EXPECT_EQ(loaded->originalCodec,    "prores4444");
    }
}

TEST(ProjectSerializer, MediaLibraryDefaultPathKindIsLinked) {
    // Default-constructed entry round-trips as Linked. This matches pre-v7
    // behavior — addMediaFile with no further setup means "absolute path,
    // QLab-style reference."
    TempFile tf("v7_default_linked");
    const std::string kPath = "C:/some/abs/path.mov";

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        pm.addMediaFile(kPath);  // no overrides — leaves pathKind=Linked
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path, &pm));
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path, nullptr, &pm));

        const auto* loaded = pm.findEntry(kPath);
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->pathKind, entity::ProjectManager::PathKind::Linked);
        EXPECT_TRUE(loaded->archivedOriginal.empty());
        EXPECT_TRUE(loaded->originalCodec.empty());
    }
}

TEST(ProjectSerializer, ObjectLibraryManagedRoundTrip) {
    // v18 — ObjectLibraryEntry persistence. A Managed entry with a
    // probe-size validity gate must survive save+load with all fields
    // intact, parallel to the MediaLibrary tests above.
    TempFile tf("v18_object_managed");
    const std::string kPath = "objects/props/chair.obj";

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        auto& entry = pm.addObjectFile(kPath,
                                       entity::ProjectManager::PathKind::Managed);
        entry.lastProbeSizeBytes = 12345;
        entry.lastProbeMtimeUnix = 999000111;

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path, &pm))
            << entity::ProjectSerializer::getLastError();
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path, nullptr, &pm))
            << entity::ProjectSerializer::getLastError();

        const auto* loaded = pm.findObjectEntry(kPath);
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->pathKind, entity::ProjectManager::PathKind::Managed);
        EXPECT_EQ(loaded->lastProbeSizeBytes, 12345);
        EXPECT_EQ(loaded->lastProbeMtimeUnix, 999000111);
        // missingOnDisk is transient — never serialized, always resets to false.
        EXPECT_FALSE(loaded->missingOnDisk);
    }
}

TEST(ProjectSerializer, ObjectLibraryLinkedDefault) {
    // Default-constructed ObjectLibraryEntry round-trips as Linked.
    TempFile tf("v18_object_linked");
    const std::string kPath = "C:/external/chair.obj";

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        pm.addObjectFile(kPath);  // no overrides — Linked default
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path, &pm));
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path, nullptr, &pm));

        const auto* loaded = pm.findObjectEntry(kPath);
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->pathKind, entity::ProjectManager::PathKind::Linked);
        EXPECT_EQ(loaded->lastProbeSizeBytes, 0);
    }
}

TEST(ProjectSerializer, ObjectLibraryVersionGroupAutoRoll) {
    // meshPathFor("objects/props/chair.obj") with chair_v01 + chair_v02
    // entries in the library should resolve to chair_v02 (latest in
    // group). The auto-roll is purely string-based; we don't need the
    // files to actually exist on disk for the resolver to return them.
    entity::ProjectManager pm;
    pm.setProjectPath(fs::temp_directory_path() / "fake_proj.entity");

    pm.addObjectFile("objects/props/chair_v01.obj",
                     entity::ProjectManager::PathKind::Managed);
    pm.addObjectFile("objects/props/chair_v02.obj",
                     entity::ProjectManager::PathKind::Managed);

    // Logical reference (no _v tag) — resolver picks the latest.
    const std::string resolved = pm.meshPathFor("objects/props/chair.obj");
    EXPECT_NE(resolved.find("chair_v02.obj"), std::string::npos)
        << "Expected chair_v02 to be the auto-rolled latest, got: " << resolved;

    // Pinned reference (explicit v01) — resolver returns the exact match.
    const std::string pinned = pm.meshPathFor("objects/props/chair_v01.obj");
    EXPECT_NE(pinned.find("chair_v01.obj"), std::string::npos)
        << "Expected pinned chair_v01 to be returned, got: " << pinned;
}

TEST(ProjectSerializer, PreV18ProjectLoadsWithEmptyObjectLibrary) {
    // Forward-compat: a v17 .entity file has no objectLibrary key. The
    // loader accepts the absence and the library starts empty.
    TempFile tf("v17_no_object_lib");

    {
        std::ofstream out(tf.path);
        out << R"({
  "format": "entity_project",
  "version": 17,
  "timeline": { "framerate": 30.0 },
  "tracks": []
})";
    }

    entt::registry registry;
    entity::Timeline timeline(registry);
    entity::ProjectManager pm;
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path, nullptr, &pm));
    EXPECT_TRUE(pm.loadedObjectFiles().empty());
}

TEST(ProjectSerializer, V6ProjectLoadsWithLinkedPathKind) {
    // Forward-compatibility gate: a v6 project file pre-dates pathKind /
    // archivedOriginal / originalCodec entirely. The loader must accept the
    // missing keys and default to Linked (matches pre-v7 absolute-path
    // semantics) with empty archive fields.
    TempFile tf("v6_no_pathkind");

    {
        std::ofstream out(tf.path);
        out << R"({
  "format": "entity_project",
  "version": 6,
  "frameRate": 30,
  "duration": 0,
  "currentFrame": 0,
  "tracks": [],
  "mediaLibrary": [
    { "originalPath": "C:/legacy/clip.mov", "transcodedPath": "", "variant": "" }
  ]
})";
    }
    ASSERT_TRUE(fs::exists(tf.path));

    entt::registry registry;
    entity::Timeline timeline(registry);
    entity::ProjectManager pm;
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path, nullptr, &pm))
        << entity::ProjectSerializer::getLastError();

    const auto* loaded = pm.findEntry("C:/legacy/clip.mov");
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->pathKind, entity::ProjectManager::PathKind::Linked);
    EXPECT_TRUE(loaded->archivedOriginal.empty());
    EXPECT_TRUE(loaded->originalCodec.empty());
}

TEST(ProjectSerializer, EmptyArchiveFieldsAreNotSerialized) {
    // Save-side intentionally omits archivedOriginal / originalCodec when
    // they're empty. Older parsers and projects without the keys stay
    // byte-identical for the common (no-archive) case. pathKind is always
    // emitted because it's a load-bearing semantic.
    TempFile tf("v7_empty_archive_omit");
    const std::string kPath = "C:/some/source.mov";

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entity::ProjectManager pm;
        pm.addMediaFile(kPath);  // empty archive fields
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path, &pm));
    }

    // Read raw JSON and assert keys are absent.
    std::ifstream in(tf.path);
    std::string contents((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    EXPECT_EQ(contents.find("archivedOriginal"), std::string::npos)
        << "Empty archivedOriginal must not be emitted";
    EXPECT_EQ(contents.find("originalCodec"), std::string::npos)
        << "Empty originalCodec must not be emitted";
    EXPECT_NE(contents.find("\"pathKind\""), std::string::npos)
        << "pathKind is always emitted (load-bearing semantic)";
}

// --- ADR-0009 / v7 — ProjectManager::createNew() folder tree -----------

namespace {

// Per-test scratch directory under the system temp dir; nuked at scope
// exit. Used as the parent dir for createNew() so we don't pollute the
// repo or the actual %TEMP%/recently-used trees.
struct TempDir {
    fs::path path;
    explicit TempDir(const char* tag) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path()
             / ("entity_createnew_test_" + std::string(tag) + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST(ProjectManagerCreateNew, BuildsExpectedFolderTree) {
    TempDir parent("tree");
    entity::ProjectManager pm;

    ASSERT_TRUE(pm.createNew(parent.path, "MyShow"));

    // Top-level project file at <parent>/MyShow/MyShow.entity.
    const fs::path projectRoot = parent.path / "MyShow";
    EXPECT_TRUE(fs::exists(projectRoot / "MyShow.entity"));

    // Canonical subdirectory layout. content/ used to seed an unsorted/
    // child but #32 dropped that — content/ itself is the default
    // landing zone now.
    EXPECT_TRUE(fs::is_directory(projectRoot / "content"));
    EXPECT_FALSE(fs::is_directory(projectRoot / "content" / "unsorted"));
    EXPECT_TRUE(fs::is_directory(projectRoot / "presets"));
    EXPECT_TRUE(fs::is_directory(projectRoot / "objects"));
    EXPECT_TRUE(fs::is_directory(projectRoot / "exports"));
    EXPECT_TRUE(fs::is_directory(projectRoot / "snapshots"));
    EXPECT_TRUE(fs::is_directory(projectRoot / ".cache" / "thumbnails"));

    // m_projectPath points at the new file.
    EXPECT_EQ(pm.projectPath(), projectRoot / "MyShow.entity");
}

TEST(ProjectManagerCreateNew, EmptyProjectLoadsAsValidV7) {
    // The .entity file createNew writes must round-trip through
    // ProjectSerializer::load — i.e. it's a real v7 project, not just
    // a stub the loader will reject later.
    TempDir parent("loadback");
    entity::ProjectManager pm;
    ASSERT_TRUE(pm.createNew(parent.path, "MyShow"));

    entt::registry registry;
    entity::Timeline timeline(registry);
    entity::ProjectManager loadPm;
    ASSERT_TRUE(entity::ProjectSerializer::load(
        timeline, pm.projectPath(), nullptr, &loadPm))
        << entity::ProjectSerializer::getLastError();

    // Empty project: no media library entries, default policy.
    EXPECT_TRUE(loadPm.loadedMediaFiles().empty());
}

TEST(ProjectManagerCreateNew, FailsIfTargetExists) {
    TempDir parent("collision");
    entity::ProjectManager pm;
    ASSERT_TRUE(pm.createNew(parent.path, "MyShow"));

    // Second attempt with the same name must refuse rather than
    // clobber an existing project.
    entity::ProjectManager pm2;
    EXPECT_FALSE(pm2.createNew(parent.path, "MyShow"));
    EXPECT_TRUE(pm2.projectPath().empty())
        << "Failed createNew must leave projectPath unset";
}

TEST(ProjectManagerCreateNew, RejectsEmptyOrPathSeparatorNames) {
    TempDir parent("badnames");
    entity::ProjectManager pm;

    EXPECT_FALSE(pm.createNew(parent.path, ""));
    EXPECT_FALSE(pm.createNew(parent.path, "act1/scene2"));
    EXPECT_FALSE(pm.createNew(parent.path, "evil\\name"));

    // None of the rejected names should have left anything on disk
    // beyond the empty parent dir we created.
    int childCount = 0;
    for (const auto& _entry : fs::directory_iterator(parent.path)) {
        (void)_entry;
        ++childCount;
    }
    EXPECT_EQ(childCount, 0);
}

TEST(ProjectManagerCreateNew, ClearsLeftoverMediaLibrary) {
    // createNew is "start fresh." Any media-library state held over from
    // a previous project on the same ProjectManager instance must be
    // cleared before the empty .entity is written; otherwise the new
    // project's first save would mysteriously contain entries from the
    // old one.
    TempDir parent("clear_lib");
    entity::ProjectManager pm;
    pm.addMediaFile("C:/leftover/clip.mov");
    ASSERT_FALSE(pm.loadedMediaFiles().empty());

    ASSERT_TRUE(pm.createNew(parent.path, "FreshShow"));
    EXPECT_TRUE(pm.loadedMediaFiles().empty());
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

// --- v8 — Projector + CalibrationPoint persistence -----------------------
// Bug we're guarding: before v8, calibration points lived only in memory
// on the Projector component. Close-and-reopen erased every crosshair the
// user had placed. Round-trip must preserve pose, lens distortion,
// useResidualWarp opt-in, every cal point, and the linkedOutput→
// OutputDisplay edge.

namespace {

entity::Projector* findProjectorByName(entt::registry& reg, const std::string& name) {
    auto view = reg.view<entity::Projector>();
    for (auto [e, p] : view.each()) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

} // namespace

TEST(ProjectSerializer, ProjectorBasicRoundTrip) {
    TempFile tf("projector_basic");

    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        entt::entity pe = registry.create();
        auto& p = registry.emplace<entity::Projector>(pe);
        p.name            = "Stage Left";
        p.position        = { 1.5f, 2.75f, -3.25f };
        p.rotation        = { -15.0f, 30.0f, 5.5f };
        p.fovDegrees      = 42.5f;
        p.nearClip        = 0.25f;
        p.farClip         = 75.0f;
        p.enabled         = false;  // intentionally non-default
        p.isCalibrated    = true;
        p.distortionK1    = -0.07f;
        p.distortionK2    = 0.013f;
        p.useResidualWarp = true;

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        auto* loaded = findProjectorByName(registry, "Stage Left");
        ASSERT_NE(loaded, nullptr) << "Projector 'Stage Left' missing after load";
        EXPECT_FLOAT_EQ(loaded->position[0], 1.5f);
        EXPECT_FLOAT_EQ(loaded->position[1], 2.75f);
        EXPECT_FLOAT_EQ(loaded->position[2], -3.25f);
        EXPECT_FLOAT_EQ(loaded->rotation[0], -15.0f);
        EXPECT_FLOAT_EQ(loaded->rotation[1], 30.0f);
        EXPECT_FLOAT_EQ(loaded->rotation[2], 5.5f);
        EXPECT_FLOAT_EQ(loaded->fovDegrees, 42.5f);
        EXPECT_FLOAT_EQ(loaded->nearClip, 0.25f);
        EXPECT_FLOAT_EQ(loaded->farClip, 75.0f);
        EXPECT_FALSE(loaded->enabled);
        EXPECT_TRUE(loaded->isCalibrated);
        EXPECT_FLOAT_EQ(loaded->distortionK1, -0.07f);
        EXPECT_FLOAT_EQ(loaded->distortionK2, 0.013f);
        EXPECT_TRUE(loaded->useResidualWarp);
    }
}

TEST(ProjectSerializer, ProjectorCalibrationPointsRoundTrip) {
    // The load-bearing case: every (worldPos, projectorUV, isAligned)
    // tuple the user placed must come back identical.
    TempFile tf("projector_cal_points");

    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        entt::entity pe = registry.create();
        auto& p = registry.emplace<entity::Projector>(pe);
        p.name = "Calibrated";
        p.calibrationPoints.push_back({
            {{0.0f, 0.0f, 0.0f}}, {{0.5f, 0.5f}}, true});
        p.calibrationPoints.push_back({
            {{1.25f, -0.5f, 2.0f}}, {{0.123f, 0.876f}}, true});
        p.calibrationPoints.push_back({
            {{-2.5f, 1.0f, -0.75f}}, {{0.99f, 0.01f}}, false});  // unaligned

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        auto* loaded = findProjectorByName(registry, "Calibrated");
        ASSERT_NE(loaded, nullptr);
        ASSERT_EQ(loaded->calibrationPoints.size(), 3u);

        const auto& cp1 = loaded->calibrationPoints[1];
        EXPECT_FLOAT_EQ(cp1.worldPos[0], 1.25f);
        EXPECT_FLOAT_EQ(cp1.worldPos[1], -0.5f);
        EXPECT_FLOAT_EQ(cp1.worldPos[2], 2.0f);
        EXPECT_FLOAT_EQ(cp1.projectorUV[0], 0.123f);
        EXPECT_FLOAT_EQ(cp1.projectorUV[1], 0.876f);
        EXPECT_TRUE(cp1.isAligned);

        EXPECT_FALSE(loaded->calibrationPoints[2].isAligned);
    }
}

TEST(ProjectSerializer, ProjectorLinkedOutputResolvesByName) {
    // linkedOutput is an entt::entity which isn't stable across save/load.
    // Persist by OutputDisplay.name, re-resolve on load.
    TempFile tf("projector_linked_output");

    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        entt::entity oe = registry.create();
        auto& out = registry.emplace<entity::OutputDisplay>(oe);
        out.name = "Beamer #2";

        entt::entity pe = registry.create();
        auto& p = registry.emplace<entity::Projector>(pe);
        p.name = "Linked";
        p.linkedOutput = oe;

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        auto* loaded = findProjectorByName(registry, "Linked");
        ASSERT_NE(loaded, nullptr);
        // Compare via registry.valid() to avoid gtest pretty-printing
        // entt::null (which forces an entt_traits<__int64> instantiation
        // that doesn't exist). Same idiom as RippleTimeTests.
        ASSERT_TRUE(registry.valid(loaded->linkedOutput));
        ASSERT_TRUE(registry.all_of<entity::OutputDisplay>(loaded->linkedOutput));
        EXPECT_EQ(registry.get<entity::OutputDisplay>(loaded->linkedOutput).name,
                  "Beamer #2");
    }
}

TEST(ProjectSerializer, ProjectorMissingLinkedOutputFallsBackToNull) {
    // If the named OutputDisplay no longer exists at load time (e.g. user
    // deleted it between save and load), linkedOutput must end up null —
    // not a stale invalid entity ID. Calibration window's UI flow handles
    // re-linking.
    TempFile tf("projector_missing_output");

    // Hand-write a v8 project file referencing an output that doesn't
    // exist in the saved "outputs" array.
    {
        std::ofstream out(tf.path);
        out << R"({
  "format": "entity_project",
  "version": 8,
  "outputs": [],
  "projectors": [
    {
      "name": "Orphaned",
      "linkedOutputName": "GhostOutput",
      "calibrationPoints": []
    }
  ]
})";
    }

    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();

    auto* loaded = findProjectorByName(registry, "Orphaned");
    ASSERT_NE(loaded, nullptr);
    // valid() returns false for entt::null and for stale handles alike;
    // either outcome means "linkedOutput points at nothing usable", which
    // is what we want here. Avoids gtest pretty-printing entt::null.
    EXPECT_FALSE(registry.valid(loaded->linkedOutput))
        << "Stale named-output reference must not produce a usable entity ID";
}

TEST(ProjectSerializer, ProjectorTargetSurfacesResolveByScreenName) {
    // targetSurfaces[] persists as an array of Screen names; on load each
    // name resolves against the Screens loaded earlier in the same pass.
    TempFile tf("projector_target_surfaces");

    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        entt::entity s1 = registry.create();
        registry.emplace<entity::Screen>(s1).name = "Front Wall";
        entt::entity s2 = registry.create();
        registry.emplace<entity::Screen>(s2).name = "Floor";

        entt::entity pe = registry.create();
        auto& p = registry.emplace<entity::Projector>(pe);
        p.name = "MultiSurface";
        p.targetSurfaceCount = 2;
        p.targetSurfaces[0] = s1;
        p.targetSurfaces[1] = s2;

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        auto* loaded = findProjectorByName(registry, "MultiSurface");
        ASSERT_NE(loaded, nullptr);
        ASSERT_EQ(loaded->targetSurfaceCount, 2);
        ASSERT_TRUE(registry.valid(loaded->targetSurfaces[0]));
        ASSERT_TRUE(registry.valid(loaded->targetSurfaces[1]));
        EXPECT_EQ(registry.get<entity::Screen>(loaded->targetSurfaces[0]).name,
                  "Front Wall");
        EXPECT_EQ(registry.get<entity::Screen>(loaded->targetSurfaces[1]).name,
                  "Floor");
    }
}

TEST(ProjectSerializer, V7ProjectLoadsWithNoProjectors) {
    // Forward-compatibility gate: a v7 file (no "projectors" key) must load
    // cleanly with zero Projector entities, no errors. Mirrors how the
    // loader handles missing Models / Screens / Outputs arrays.
    TempFile tf("projector_v7_no_key");

    {
        std::ofstream out(tf.path);
        out << R"({
  "format": "entity_project",
  "version": 7,
  "outputs": []
})";
    }

    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();

    EXPECT_EQ(registry.view<entity::Projector>().size(), 0u);
}

// --- Phase 3.6 — v15 OA layer serialization + v14 legacy migration --------

TEST(ProjectSerializer, OALayerRoundTrip) {
    // v15 round-trip: save an OA layer with a PositionX keyframe targeting a
    // Screen; reload and confirm Layer + ObjectAnimationLayer + keyframe all
    // survive intact.
    TempFile tf("oa_layer_roundtrip");

    entt::entity screenEntity = entt::null;
    entt::entity oaEntity     = entt::null;

    // --- Save ---
    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        // Create a Screen target.
        screenEntity = registry.create();
        auto& screen = registry.emplace<entity::Screen>(screenEntity);
        screen.name = "TestScreen";

        // Create a timeline track and OA layer entity.
        entt::entity trackEntity = timeline.createTrack("Track 0");
        entity::TimelineTrack* track = registry.try_get<entity::TimelineTrack>(trackEntity);
        if (!track) { FAIL() << "createTrack returned entity with no TimelineTrack"; }

        oaEntity = registry.create();

        auto& lay = registry.emplace<entity::Layer>(oaEntity);
        lay.kind       = entity::Layer::Kind::ObjectAnimation;
        lay.startFrame = 10;
        lay.duration   = 300;
        lay.trackIndex = track->trackIndex;
        lay.name       = "Spin";
        lay.color      = {0.6f, 0.2f, 0.8f, 1.0f};

        auto& oal = registry.emplace<entity::ObjectAnimationLayer>(oaEntity);
        oal.target          = screenEntity;
        oal.sectionBehavior = entity::SectionBehavior::Locked;
        // ADR-0020: Reset is the non-default; verify it round-trips. Hold is
        // the loader's default for missing keys, so using Reset here proves the
        // serializer actually emits + parses the field instead of defaulting.
        oal.endBehavior     = entity::ObjectAnimationLayer::EndBehavior::Reset;

        auto& ap = registry.emplace<entity::AnimatedProperties>(oaEntity);
        ap.addKeyframe(entity::AnimatableProperty::PositionX, 0,   2.5f);
        ap.addKeyframe(entity::AnimatableProperty::PositionX, 150, 8.0f);

        // EaseInOut + PositionZ: exercises interpolationTypeToJson("ease_in_out"),
        // animatablePropertyToJson("PositionZ"), and easeIn/easeOut persistence.
        ap.addKeyframe(entity::AnimatableProperty::PositionZ, 0,   0.0f,
                       entity::InterpolationType::EaseInOut);
        ap.addKeyframe(entity::AnimatableProperty::PositionZ, 100, 5.0f,
                       entity::InterpolationType::EaseInOut);
        {
            // Patch non-default easeIn/easeOut so deserialization silence isn't hidden.
            auto& posZTrack = ap.getOrCreateTrack(entity::AnimatableProperty::PositionZ);
            posZTrack.keyframes[0].easeIn  = 0.25f;
            posZTrack.keyframes[0].easeOut = 0.75f;
        }

        // ADR-0020: RotationZ — exercises the new enum string mapping in
        // animatablePropertyToJson / jsonToAnimatableProperty.
        ap.addKeyframe(entity::AnimatableProperty::RotationZ, 0,   0.0f);
        ap.addKeyframe(entity::AnimatableProperty::RotationZ, 200, 270.0f);

        track->layers.push_back(oaEntity);

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    // --- Load ---
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        // Find the OA layer entity.
        entt::entity loaded = entt::null;
        for (auto [e, oal, lay] :
             registry.view<entity::ObjectAnimationLayer, entity::Layer>().each()) {
            if (lay.name == "Spin") { loaded = e; break; }
        }
        ASSERT_TRUE(registry.valid(loaded)) << "OA layer 'Spin' not found after load";

        const auto& lay = registry.get<entity::Layer>(loaded);
        EXPECT_EQ(lay.startFrame, 10);
        EXPECT_EQ(lay.duration,   300);
        EXPECT_EQ(lay.name,       "Spin");
        EXPECT_NEAR(lay.color[0], 0.6f, 0.001f);

        const auto& oal = registry.get<entity::ObjectAnimationLayer>(loaded);
        EXPECT_EQ(oal.sectionBehavior, entity::SectionBehavior::Locked);
        EXPECT_EQ(oal.endBehavior,
                  entity::ObjectAnimationLayer::EndBehavior::Reset);

        // Target is resolved by Screen name — must point at "TestScreen".
        ASSERT_TRUE(registry.valid(oal.target)) << "OA layer target is null after load";
        EXPECT_EQ(registry.get<entity::Screen>(oal.target).name, "TestScreen");

        // AnimatedProperties keyframes must have survived.
        const auto* ap = registry.try_get<entity::AnimatedProperties>(loaded);
        ASSERT_NE(ap, nullptr) << "AnimatedProperties missing after load";

        // PositionX — Linear interpolation, two keyframes.
        const auto* pxTrack = ap->getTrack(entity::AnimatableProperty::PositionX);
        ASSERT_NE(pxTrack, nullptr) << "PositionX track missing after load";
        ASSERT_EQ(pxTrack->keyframes.size(), 2u);
        EXPECT_EQ(pxTrack->keyframes[0].frame, 0);
        EXPECT_NEAR(pxTrack->keyframes[0].value, 2.5f, 0.001f);
        EXPECT_EQ(pxTrack->keyframes[0].interpolation, entity::InterpolationType::Linear);
        EXPECT_EQ(pxTrack->keyframes[1].frame, 150);
        EXPECT_NEAR(pxTrack->keyframes[1].value, 8.0f, 0.001f);

        // PositionZ — EaseInOut interpolation + custom easeIn/easeOut values.
        const auto* pzTrack = ap->getTrack(entity::AnimatableProperty::PositionZ);
        ASSERT_NE(pzTrack, nullptr) << "PositionZ track missing after load";
        ASSERT_EQ(pzTrack->keyframes.size(), 2u);
        EXPECT_EQ(pzTrack->keyframes[0].frame, 0);
        EXPECT_NEAR(pzTrack->keyframes[0].value, 0.0f, 0.001f);
        EXPECT_EQ(pzTrack->keyframes[0].interpolation, entity::InterpolationType::EaseInOut);
        EXPECT_NEAR(pzTrack->keyframes[0].easeIn,  0.25f, 0.001f);
        EXPECT_NEAR(pzTrack->keyframes[0].easeOut, 0.75f, 0.001f);
        EXPECT_EQ(pzTrack->keyframes[1].frame, 100);
        EXPECT_NEAR(pzTrack->keyframes[1].value, 5.0f, 0.001f);
        EXPECT_EQ(pzTrack->keyframes[1].interpolation, entity::InterpolationType::EaseInOut);

        // RotationZ (ADR-0020) — exercises the new enum string mapping.
        const auto* rzTrack = ap->getTrack(entity::AnimatableProperty::RotationZ);
        ASSERT_NE(rzTrack, nullptr) << "RotationZ track missing after load";
        ASSERT_EQ(rzTrack->keyframes.size(), 2u);
        EXPECT_EQ(rzTrack->keyframes[0].frame, 0);
        EXPECT_NEAR(rzTrack->keyframes[0].value, 0.0f, 0.001f);
        EXPECT_EQ(rzTrack->keyframes[1].frame, 200);
        EXPECT_NEAR(rzTrack->keyframes[1].value, 270.0f, 0.001f);
    }
}

TEST(ProjectSerializer, V14LegacyClipsKeyLoadsAsClipKind) {
    // Forward-compatibility gate: a v14 file using "clips[]" per-track
    // must load cleanly with the entries treated as kind=Clip. The loader
    // must not require the newer "layers[]" key.
    TempFile tf("v14_clips_key_migration");

    {
        std::ofstream out(tf.path);
        out << R"({
  "format": "entity_project",
  "version": 14,
  "timeline": {"duration": 1800, "framerate": 30, "currentTime": 0},
  "tracks": [
    {
      "index": 0,
      "clips": [
        {
          "filepath": "content/test.mov",
          "mediaType": "prores4444",
          "startFrame": 0,
          "duration": 300,
          "mediaStartFrame": 0,
          "mediaOutFrame": 299,
          "totalMediaFrames": 300,
          "playbackMode": 0,
          "sectionBehavior": "Normal",
          "framerate": 30.0,
          "width": 1920,
          "height": 1080,
          "hasAlpha": false,
          "frameBlending": false,
          "targetScreenName": ""
        }
      ]
    }
  ]
})";
    }

    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();

    // The loaded entity must have a Layer with kind=Clip.
    auto layerView = registry.view<entity::Layer, entity::Clip>();
    int clipCount = 0;
    for (auto [e, lay, clip] : layerView.each()) {
        EXPECT_EQ(lay.kind, entity::Layer::Kind::Clip);
        EXPECT_EQ(clip.filepath, "content/test.mov");
        ++clipCount;
    }
    EXPECT_GT(clipCount, 0) << "No Clip+Layer entity loaded from v14 clips[] key";
    // Must NOT have any ObjectAnimationLayer entities.
    EXPECT_EQ(registry.view<entity::ObjectAnimationLayer>().size(), 0u);
}

TEST(ProjectSerializer, EffectChainRoundTrip) {
    // v16 round-trip: save a clip with an EffectChain of one Effect
    // (kind = core.gaussian_blur, enabled, params [4.5]); reload and
    // confirm Effect + EffectParameters + chain order survive intact.
    // Issue #54.
    TempFile tf("effect_chain_roundtrip");
    const std::uint32_t kBlurKindHash = entity::effects::fnv1a32("core.gaussian_blur");

    // --- Save ---
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        entt::entity trackEntity = timeline.createTrack("Track 0");
        entity::TimelineTrack* track = registry.try_get<entity::TimelineTrack>(trackEntity);
        ASSERT_NE(track, nullptr);

        // Minimal clip — schema requires a Clip + Layer pair.
        entt::entity clipEnt = registry.create();
        auto& clip = registry.emplace<entity::Clip>(clipEnt);
        clip.filepath  = "content/test.mov";
        clip.mediaType = entity::MediaType::VideoProRes4444;
        clip.startFrame = 0;
        clip.duration   = 60;
        clip.framerate  = 30.0;
        auto& lay = registry.emplace<entity::Layer>(clipEnt);
        lay.kind       = entity::Layer::Kind::Clip;
        lay.startFrame = 0;
        lay.duration   = 60;

        // Effect entity + chain.
        entt::entity fxEnt = registry.create();
        auto& fx = registry.emplace<entity::Effect>(fxEnt);
        fx.kindId  = kBlurKindHash;
        fx.enabled = true;
        fx.graphX  = 12.5f;
        fx.graphY  = -7.0f;
        auto& params = registry.emplace<entity::EffectParameters>(fxEnt);
        params.values.push_back(entity::ParamValue::makeFloat(4.5f));
        registry.emplace<entity::EffectAnimatedParameters>(fxEnt);

        auto& chain = registry.emplace<entity::EffectChain>(clipEnt);
        chain.nodes.push_back(fxEnt);
        chain.outputNode = fxEnt;

        track->layers.push_back(clipEnt);

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    // --- Load ---
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        // Find the clip entity.
        entt::entity loadedClip = entt::null;
        for (auto [e, lay, clip] :
             registry.view<entity::Layer, entity::Clip>().each()) {
            if (clip.filepath == "content/test.mov") { loadedClip = e; break; }
        }
        ASSERT_TRUE(registry.valid(loadedClip)) << "clip not found after load";

        // Effect chain must have survived.
        const auto* chain = registry.try_get<entity::EffectChain>(loadedClip);
        ASSERT_NE(chain, nullptr) << "EffectChain missing after load";
        ASSERT_EQ(chain->nodes.size(), 1u);

        entt::entity fxEnt = chain->nodes[0];
        ASSERT_TRUE(registry.valid(fxEnt));

        const auto* fx = registry.try_get<entity::Effect>(fxEnt);
        ASSERT_NE(fx, nullptr);
        EXPECT_EQ(fx->kindId, kBlurKindHash);
        EXPECT_TRUE(fx->enabled);
        EXPECT_FLOAT_EQ(fx->graphX, 12.5f);
        EXPECT_FLOAT_EQ(fx->graphY, -7.0f);

        const auto* paramsLoaded = registry.try_get<entity::EffectParameters>(fxEnt);
        ASSERT_NE(paramsLoaded, nullptr);
        ASSERT_EQ(paramsLoaded->values.size(), 1u);
        EXPECT_FLOAT_EQ(paramsLoaded->values[0].f4[0], 4.5f);
    }
}

TEST(ProjectSerializer, V15ProjectLoadsWithEmptyEffectChain) {
    // Forward compat: a v15 file (no "effects" key on clips) must load
    // cleanly with no EffectChain attached. The loader treats a missing
    // key as "no effects on this layer."
    TempFile tf("v15_no_effects_key");

    {
        std::ofstream out(tf.path);
        out << R"({
  "format": "entity_project",
  "version": 15,
  "timeline": {"duration": 600, "framerate": 30, "currentTime": 0},
  "tracks": [
    {
      "index": 0,
      "layers": [
        {
          "kind": "clip",
          "filepath": "content/test.mov",
          "mediaType": "prores4444",
          "startFrame": 0,
          "duration": 60,
          "mediaStartFrame": 0,
          "mediaOutFrame": 59,
          "totalMediaFrames": 60,
          "playbackMode": 0,
          "sectionBehavior": "Normal",
          "framerate": 30.0,
          "width": 1920,
          "height": 1080,
          "hasAlpha": false,
          "frameBlending": false,
          "targetScreenName": ""
        }
      ]
    }
  ]
})";
    }

    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();

    // No EffectChain should be attached anywhere in the registry.
    EXPECT_EQ(registry.view<entity::EffectChain>().size(), 0u);
    // Clip is still loaded normally.
    EXPECT_GT(registry.view<entity::Clip>().size(), 0u);
}

// --- v19 stage-view opacity for Screen / Prop ----------------------------

TEST(ProjectSerializer, ScreenAndPropStageOpacityRoundTrip) {
    // Screen.opacity has always been persisted, but pre-v19 it was wired to
    // nothing. Prop.opacity is new in v19. Both must survive a round trip.
    TempFile tf("stage_opacity_roundtrip");

    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        entt::entity se = registry.create();
        auto& screen = registry.emplace<entity::Screen>(se);
        screen.name    = "Front Wall";
        screen.opacity = 0.5f;

        entt::entity pe = registry.create();
        auto& prop = registry.emplace<entity::Prop>(pe);
        prop.name    = "Riser";
        prop.opacity = 0.25f;

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        const entity::Screen* loadedScreen = nullptr;
        for (auto [e, s] : registry.view<entity::Screen>().each()) {
            if (s.name == "Front Wall") { loadedScreen = &s; break; }
        }
        ASSERT_NE(loadedScreen, nullptr);
        EXPECT_FLOAT_EQ(loadedScreen->opacity, 0.5f);

        const entity::Prop* loadedProp = nullptr;
        for (auto [e, p] : registry.view<entity::Prop>().each()) {
            if (p.name == "Riser") { loadedProp = &p; break; }
        }
        ASSERT_NE(loadedProp, nullptr);
        EXPECT_FLOAT_EQ(loadedProp->opacity, 0.25f);
    }
}

TEST(ProjectSerializer, PreV19PropLoadsWithOpacityOne) {
    // A v18 project file (no "opacity" on props) must default the new field
    // to 1.0 (fully opaque) so legacy projects don't suddenly go transparent.
    TempFile tf("prop_pre_v19_opacity_default");

    {
        std::ofstream out(tf.path);
        out << R"({
  "format": "entity_project",
  "version": 18,
  "props": [
    {
      "name": "LegacyProp",
      "visible": true,
      "position": [0.0, 0.0, 0.0],
      "rotation": [0.0, 0.0, 0.0],
      "size": [1.0, 1.0, 1.0],
      "displayColor": [0.6, 0.6, 0.6, 1.0],
      "modelName": ""
    }
  ]
})";
    }

    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();

    const entity::Prop* loaded = nullptr;
    for (auto [e, p] : registry.view<entity::Prop>().each()) {
        if (p.name == "LegacyProp") { loaded = &p; break; }
    }
    ASSERT_NE(loaded, nullptr);
    EXPECT_FLOAT_EQ(loaded->opacity, 1.0f);
}

// ADR-0021 M2: project round-trip preserves ContentRouting on a clip with a
// single Direct target. Legacy `targetScreen` alias stays in sync. The
// canonical `contentRouting` JSON object is emitted on save and preferred on
// load over the legacy `targetScreenName` field.
TEST(ProjectSerializer, ContentRoutingDirectRoundTrip) {
    TempFile tf("content_routing_direct_roundtrip");

    // --- Save ---
    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        // Create a target Screen.
        entt::entity screenEnt = registry.create();
        auto& screen = registry.emplace<entity::Screen>(screenEnt);
        screen.name   = "TargetScreen";
        screen.width  = 1920;
        screen.height = 1080;

        // Create a track + clip.
        entt::entity trackEntity = timeline.createTrack("Track 0");
        auto* track = registry.try_get<entity::TimelineTrack>(trackEntity);
        ASSERT_NE(track, nullptr);

        entt::entity clipEnt = registry.create();
        auto& clip = registry.emplace<entity::Clip>(clipEnt);
        clip.filepath  = "content/test.mov";
        clip.mediaType = entity::MediaType::VideoProRes4444;
        clip.startFrame = 0;
        clip.duration   = 60;
        clip.framerate  = 30.0;
        clip.targetScreen = screenEnt;

        auto& lay = registry.emplace<entity::Layer>(clipEnt);
        lay.kind       = entity::Layer::Kind::Clip;
        lay.startFrame = 0;
        lay.duration   = 60;

        // The canonical Plane A state.
        auto& cr = registry.emplace<entity::ContentRouting>(clipEnt);
        cr.mode = entity::RouteMode::Direct;
        entity::RouteTarget rt;
        rt.screen = screenEnt;
        rt.uvRect = {0.0f, 0.0f, 1.0f, 1.0f};
        cr.targets.push_back(rt);

        track->layers.push_back(clipEnt);

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    // --- Load ---
    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();

    // The screen entity is re-created with the same name.
    entt::entity loadedScreen = entt::null;
    for (auto [e, s] : registry.view<entity::Screen>().each()) {
        if (s.name == "TargetScreen") { loadedScreen = e; break; }
    }
    ASSERT_TRUE(loadedScreen != entt::null);

    // The clip entity carries ContentRouting with one Direct target
    // pointing at the loaded screen, and the legacy alias stays in sync.
    int routedClipCount = 0;
    for (auto [e, clip, cr] : registry.view<entity::Clip, entity::ContentRouting>().each()) {
        if (clip.filepath != "content/test.mov") continue;
        ++routedClipCount;
        EXPECT_EQ(cr.mode, entity::RouteMode::Direct);
        ASSERT_EQ(cr.targets.size(), 1u);
        EXPECT_EQ(cr.targets[0].screen, loadedScreen);
        EXPECT_FLOAT_EQ(cr.targets[0].uvRect[0], 0.0f);
        EXPECT_FLOAT_EQ(cr.targets[0].uvRect[2], 1.0f);
        EXPECT_EQ(clip.targetScreen, loadedScreen);
    }
    EXPECT_EQ(routedClipCount, 1) << "Expected exactly one ContentRouting-bearing clip";
}

// ADR-0021 M2: a pre-M2 project with only the legacy `targetScreenName`
// field loads as a ContentRouting with one Direct target. The legacy alias
// and the new canonical state must agree.
TEST(ProjectSerializer, ContentRoutingLegacyTargetScreenMigration) {
    TempFile tf("content_routing_legacy_migration");

    // Hand-written pre-M2 JSON (no `contentRouting` key, only legacy
    // `targetScreenName`). Mirrors the v15 layers[] shape; the version
    // check inside the loader is tolerant of the explicit field omission.
    {
        std::ofstream out(tf.path);
        out << R"({
  "format": "entity_project",
  "version": 17,
  "timeline": {"duration": 1800, "framerate": 30, "currentTime": 0},
  "screens": [
    {"name": "MainWall", "width": 1920, "height": 1080,
     "position": [0,0,0], "rotation": [0,0,0], "size": [4.0, 2.25, 0.0],
     "visible": true, "opacity": 1.0, "zOrder": 0, "type": "LEDWall"}
  ],
  "tracks": [
    {
      "index": 0,
      "layers": [
        {
          "kind": "clip",
          "filepath": "content/test.mov",
          "mediaType": "prores4444",
          "startFrame": 0,
          "duration": 60,
          "mediaStartFrame": 0,
          "mediaOutFrame": 59,
          "totalMediaFrames": 60,
          "playbackMode": 0,
          "sectionBehavior": "Normal",
          "framerate": 30.0,
          "width": 1920,
          "height": 1080,
          "hasAlpha": false,
          "frameBlending": false,
          "targetScreenName": "MainWall"
        }
      ]
    }
  ]
})";
    }

    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();

    entt::entity wall = entt::null;
    for (auto [e, s] : registry.view<entity::Screen>().each()) {
        if (s.name == "MainWall") { wall = e; break; }
    }
    ASSERT_TRUE(wall != entt::null);

    int routedClipCount = 0;
    for (auto [e, clip, cr] : registry.view<entity::Clip, entity::ContentRouting>().each()) {
        if (clip.filepath != "content/test.mov") continue;
        ++routedClipCount;
        EXPECT_EQ(cr.mode, entity::RouteMode::Direct);
        ASSERT_EQ(cr.targets.size(), 1u);
        EXPECT_EQ(cr.targets[0].screen, wall);
        EXPECT_EQ(clip.targetScreen, wall);
    }
    EXPECT_EQ(routedClipCount, 1) << "Expected legacy targetScreenName to materialize one Direct route";
}

// ADR-0021 M2: a clip with no targetScreen (null / empty name) loads as a
// ContentRouting with `mode=Direct, targets=[]` (empty), preserving the
// legacy "render on all visible screens" semantic.
TEST(ProjectSerializer, ContentRoutingNullTargetIsEmptyTargets) {
    TempFile tf("content_routing_null_target");

    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        entt::entity trackEntity = timeline.createTrack("Track 0");
        auto* track = registry.try_get<entity::TimelineTrack>(trackEntity);
        ASSERT_NE(track, nullptr);

        entt::entity clipEnt = registry.create();
        auto& clip = registry.emplace<entity::Clip>(clipEnt);
        clip.filepath  = "content/all_screens.mov";
        clip.mediaType = entity::MediaType::VideoProRes4444;
        clip.startFrame = 0;
        clip.duration   = 60;
        clip.framerate  = 30.0;
        // clip.targetScreen stays entt::null — "render on all".

        auto& lay = registry.emplace<entity::Layer>(clipEnt);
        lay.kind       = entity::Layer::Kind::Clip;
        lay.startFrame = 0;
        lay.duration   = 60;

        // ContentRouting with empty targets.
        auto& cr = registry.emplace<entity::ContentRouting>(clipEnt);
        cr.mode = entity::RouteMode::Direct;

        track->layers.push_back(clipEnt);

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();

    int routedClipCount = 0;
    for (auto [e, clip, cr] : registry.view<entity::Clip, entity::ContentRouting>().each()) {
        if (clip.filepath != "content/all_screens.mov") continue;
        ++routedClipCount;
        EXPECT_EQ(cr.mode, entity::RouteMode::Direct);
        EXPECT_EQ(cr.targets.size(), 0u);
        EXPECT_TRUE(clip.targetScreen == entt::null);
    }
    EXPECT_EQ(routedClipCount, 1);
}

// ADR-0021 M3: project round-trip preserves a Tiled ContentRouting with
// multiple per-screen UV crops — the headline feed-map authoring scenario.
TEST(ProjectSerializer, ContentRoutingTiledRoundTrip) {
    TempFile tf("content_routing_tiled_roundtrip");

    // --- Save ---
    {
        entt::registry registry;
        entity::Timeline timeline(registry);

        // Three target Screens — left, middle, right LED-wall panels.
        entt::entity sLeft = registry.create();
        registry.emplace<entity::Screen>(sLeft).name   = "LeftLED";
        entt::entity sMid = registry.create();
        registry.emplace<entity::Screen>(sMid).name    = "MiddleLED";
        entt::entity sRight = registry.create();
        registry.emplace<entity::Screen>(sRight).name  = "RightLED";

        entt::entity trackEntity = timeline.createTrack("Track 0");
        auto* track = registry.try_get<entity::TimelineTrack>(trackEntity);
        ASSERT_NE(track, nullptr);

        // Panoramic clip carved across the three screens.
        entt::entity clipEnt = registry.create();
        auto& clip = registry.emplace<entity::Clip>(clipEnt);
        clip.filepath  = "content/panorama_5760x1080.mov";
        clip.mediaType = entity::MediaType::VideoProRes4444;
        clip.startFrame = 0;
        clip.duration   = 120;
        clip.framerate  = 30.0;
        // targetScreen alias is left null — Tiled doesn't map to a single
        // screen, so the legacy field reads as "all visible" if anything
        // ever falls back to it.
        clip.targetScreen = entt::null;

        auto& lay = registry.emplace<entity::Layer>(clipEnt);
        lay.kind       = entity::Layer::Kind::Clip;
        lay.startFrame = 0;
        lay.duration   = 120;

        auto& cr = registry.emplace<entity::ContentRouting>(clipEnt);
        cr.mode = entity::RouteMode::Tiled;
        cr.targets.push_back({sLeft,  {0.0f,    0.0f, 0.3333f, 1.0f}});
        cr.targets.push_back({sMid,   {0.3333f, 0.0f, 0.3334f, 1.0f}});
        cr.targets.push_back({sRight, {0.6667f, 0.0f, 0.3333f, 1.0f}});

        track->layers.push_back(clipEnt);

        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    // --- Load ---
    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();

    entt::entity sLeft = entt::null, sMid = entt::null, sRight = entt::null;
    for (auto [e, s] : registry.view<entity::Screen>().each()) {
        if (s.name == "LeftLED")   sLeft  = e;
        if (s.name == "MiddleLED") sMid   = e;
        if (s.name == "RightLED")  sRight = e;
    }
    ASSERT_TRUE(sLeft  != entt::null);
    ASSERT_TRUE(sMid   != entt::null);
    ASSERT_TRUE(sRight != entt::null);

    int tiledClipCount = 0;
    for (auto [e, clip, cr] : registry.view<entity::Clip, entity::ContentRouting>().each()) {
        if (clip.filepath != "content/panorama_5760x1080.mov") continue;
        ++tiledClipCount;
        EXPECT_EQ(cr.mode, entity::RouteMode::Tiled);
        ASSERT_EQ(cr.targets.size(), 3u);
        EXPECT_EQ(cr.targets[0].screen, sLeft);
        EXPECT_FLOAT_EQ(cr.targets[0].uvRect[0], 0.0f);
        EXPECT_FLOAT_EQ(cr.targets[0].uvRect[2], 0.3333f);
        EXPECT_EQ(cr.targets[1].screen, sMid);
        EXPECT_FLOAT_EQ(cr.targets[1].uvRect[0], 0.3333f);
        EXPECT_FLOAT_EQ(cr.targets[1].uvRect[2], 0.3334f);
        EXPECT_EQ(cr.targets[2].screen, sRight);
        EXPECT_FLOAT_EQ(cr.targets[2].uvRect[0], 0.6667f);
        // The legacy alias stays null for Tiled (it can't represent multi-
        // target); the ContentRouting component is the source of truth.
        EXPECT_TRUE(clip.targetScreen == entt::null);
    }
    EXPECT_EQ(tiledClipCount, 1) << "Expected one Tiled-routed clip after reload";
}
