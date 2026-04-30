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
