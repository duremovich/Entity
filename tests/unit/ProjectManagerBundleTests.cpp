/**
 * Unit tests for ProjectManager::saveAsBundle (ADR-0009).
 *
 * Each test sets up a tiny on-disk project with content/, presets/, and
 * a .cache/ that should NOT appear in the bundle. Then runs saveAsBundle
 * to a fresh location and asserts on the destination shape.
 */

#include <gtest/gtest.h>

#include "entity/project/ProjectManager.hpp"
#include "entity/project/ProjectSerializer.hpp"
#include "entity/timeline/Timeline.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeTempRoot(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path()
                  / ("entity_bundle_test_" + std::string(tag) + "_"
                     + std::to_string(stamp));
    fs::create_directories(root);
    return root;
}

struct TempScratch {
    fs::path root;
    explicit TempScratch(const char* tag) : root(makeTempRoot(tag)) {}
    ~TempScratch() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p) << body;
}

}  // namespace

TEST(ProjectManagerBundle, CopiesContentTreeAndPresetsExcludesCache) {
    TempScratch scratch("copies-content");

    // Source project layout (mimics what createNew + a few imports would produce).
    const fs::path srcRoot = scratch.root / "Source";
    writeFile(srcRoot / "Source.entity", "{}");  // minimal placeholder
    writeFile(srcRoot / "content" / "act1" / "intro.mov",          "intro bytes");
    writeFile(srcRoot / "content" / "act1" / ".archive" / "intro.mov", "archive bytes");
    writeFile(srcRoot / "content" / "unsorted" / "leftover.mov",   "leftover bytes");
    writeFile(srcRoot / "presets" / "color.json",                  "{\"k\":\"v\"}");
    writeFile(srcRoot / "objects" / "stage.obj",                   "obj bytes");
    writeFile(srcRoot / ".cache"  / "thumbnails" / "thumb1.png",   "thumb bytes");
    writeFile(srcRoot / ".cache"  / "transcode" / "old.mov",       "stale cache");

    // Wire up ProjectManager pointed at the source project.
    entity::ProjectManager pm;
    pm.setProjectPath(srcRoot / "Source.entity");
    // Add a Managed entry so we can verify the .entity round-trips with the
    // path-relative payload after the bundle.
    auto& entry = pm.addMediaFile("content/act1/intro.mov",
                                   entity::ProjectManager::PathKind::Managed);
    entry.archivedOriginal = "content/act1/.archive/intro.mov";
    entry.originalCodec    = "ProRes 4444";

    // Need a Timeline + registry for save() to work. The ProjectSerializer
    // expects to serialize tracks/clips, so a real Timeline is required.
    entt::registry reg;
    entity::Timeline timeline(reg);
    pm.initialize(&timeline, &reg, /*renderer=*/nullptr);

    // Save once into the source project so the .entity file has real v7 contents.
    ASSERT_TRUE(pm.save(srcRoot / "Source.entity"));

    // Now bundle to a fresh location.
    const fs::path bundleParent = scratch.root / "bundle_target";
    fs::create_directories(bundleParent);

    ASSERT_TRUE(pm.saveAsBundle(bundleParent, "MyBundle"));

    const fs::path bundleRoot = bundleParent / "MyBundle";
    ASSERT_TRUE(fs::exists(bundleRoot));

    // .entity file at the new root.
    EXPECT_TRUE(fs::exists(bundleRoot / "MyBundle.entity"));

    // content/ tree fully copied (including .archive/ subfolder).
    EXPECT_TRUE(fs::exists(bundleRoot / "content" / "act1" / "intro.mov"));
    EXPECT_TRUE(fs::exists(bundleRoot / "content" / "act1" / ".archive" / "intro.mov"));
    EXPECT_TRUE(fs::exists(bundleRoot / "content" / "unsorted" / "leftover.mov"));

    // presets/ + objects/ copied.
    EXPECT_TRUE(fs::exists(bundleRoot / "presets" / "color.json"));
    EXPECT_TRUE(fs::exists(bundleRoot / "objects" / "stage.obj"));

    // .cache/ NOT copied — machine-local, regenerable.
    EXPECT_FALSE(fs::exists(bundleRoot / ".cache"));

    // m_projectPath updated to the new file.
    EXPECT_EQ(pm.projectPath(), bundleRoot / "MyBundle.entity");

    // Source project untouched.
    EXPECT_TRUE(fs::exists(srcRoot / "Source.entity"));
    EXPECT_TRUE(fs::exists(srcRoot / "content" / "act1" / "intro.mov"));
}

TEST(ProjectManagerBundle, RefusesEmptyName) {
    TempScratch scratch("empty-name");
    fs::path src = scratch.root / "Source";
    writeFile(src / "Source.entity", "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(src / "Source.entity");
    entt::registry reg;
    entity::Timeline timeline(reg);
    pm.initialize(&timeline, &reg, nullptr);
    ASSERT_TRUE(pm.save());

    EXPECT_FALSE(pm.saveAsBundle(scratch.root / "tgt", ""));
    EXPECT_FALSE(fs::exists(scratch.root / "tgt"));
}

TEST(ProjectManagerBundle, RefusesNameWithPathSeparators) {
    TempScratch scratch("bad-name");
    fs::path src = scratch.root / "Source";
    writeFile(src / "Source.entity", "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(src / "Source.entity");
    entt::registry reg;
    entity::Timeline timeline(reg);
    pm.initialize(&timeline, &reg, nullptr);
    ASSERT_TRUE(pm.save());

    EXPECT_FALSE(pm.saveAsBundle(scratch.root / "tgt", "a/b"));
    EXPECT_FALSE(pm.saveAsBundle(scratch.root / "tgt", "a\\b"));
}

TEST(ProjectManagerBundle, RefusesIfTargetExists) {
    TempScratch scratch("collision");
    fs::path src = scratch.root / "Source";
    writeFile(src / "Source.entity", "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(src / "Source.entity");
    entt::registry reg;
    entity::Timeline timeline(reg);
    pm.initialize(&timeline, &reg, nullptr);
    ASSERT_TRUE(pm.save());

    fs::path target = scratch.root / "bundle";
    fs::create_directories(target / "AlreadyHere");

    EXPECT_FALSE(pm.saveAsBundle(target, "AlreadyHere"));
    // Original project path unchanged after a refused bundle.
    EXPECT_EQ(pm.projectPath(), src / "Source.entity");
}

TEST(ProjectManagerBundle, RefusesIfNoProjectLoaded) {
    TempScratch scratch("no-project");
    entity::ProjectManager pm;
    // Deliberately don't setProjectPath / save first.
    EXPECT_FALSE(pm.saveAsBundle(scratch.root, "Whatever"));
    EXPECT_FALSE(fs::exists(scratch.root / "Whatever"));
}

TEST(ProjectManagerBundle, BundledProjectLoadsAndResolvesManagedPaths) {
    TempScratch scratch("roundtrip");

    // Source project with one Managed clip in content/act1/.
    const fs::path srcRoot = scratch.root / "Src";
    writeFile(srcRoot / "Src.entity", "{}");
    writeFile(srcRoot / "content" / "act1" / "clip.mov", "managed clip bytes");

    entity::ProjectManager pm;
    pm.setProjectPath(srcRoot / "Src.entity");
    entt::registry reg;
    entity::Timeline timeline(reg);
    pm.initialize(&timeline, &reg, nullptr);
    pm.addMediaFile("content/act1/clip.mov",
                    entity::ProjectManager::PathKind::Managed);
    ASSERT_TRUE(pm.save());

    // Bundle to a new location.
    ASSERT_TRUE(pm.saveAsBundle(scratch.root / "bundle_dst", "Show"));

    // Reload the bundled project from disk in a fresh ProjectManager.
    entity::ProjectManager pm2;
    entt::registry reg2;
    entity::Timeline tl2(reg2);
    pm2.initialize(&tl2, &reg2, nullptr);

    // Use ProjectSerializer directly — pm2.load needs an IRenderer for
    // GPU-side bookkeeping, which we don't have in unit tests.
    ASSERT_TRUE(entity::ProjectSerializer::load(
        tl2,
        scratch.root / "bundle_dst" / "Show" / "Show.entity",
        nullptr,
        &pm2));
    pm2.setProjectPath(scratch.root / "bundle_dst" / "Show" / "Show.entity");

    // Managed entry survived the bundle and resolves against the new root.
    const auto* entry = pm2.findEntry("content/act1/clip.mov");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->pathKind, entity::ProjectManager::PathKind::Managed);

    fs::path resolved(pm2.resolveMediaPath("content/act1/clip.mov"));
    EXPECT_EQ(resolved.lexically_normal(),
              (scratch.root / "bundle_dst" / "Show" / "content" / "act1" / "clip.mov")
                  .lexically_normal());
    EXPECT_TRUE(fs::exists(resolved));
}
