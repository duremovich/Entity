/**
 * Unit tests for orphan-showfile recovery (ADR-0009):
 *
 *   - ProjectManager::rebuildStructure  — idempotent canonical-dir mkdir
 *   - ProjectManager::findMissingManagedMedia — filename-match restore
 *
 * Both are pure project-state helpers (no EnTT / GPU / FFmpeg), so the
 * tests operate on a real temp dir and assert on the on-disk result.
 */

#include <gtest/gtest.h>

#include "entity/project/ProjectManager.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeTempRoot(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path()
                  / ("entity_recovery_test_" + std::string(tag) + "_"
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

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), {}};
}

}  // namespace

// --- rebuildStructure ------------------------------------------------------

TEST(ProjectManagerRebuild, CreatesAllCanonicalSubdirsFromEmpty) {
    TempScratch scratch("rebuild-empty");
    fs::path projectFile = scratch.root / "Show.entity";
    writeFile(projectFile, "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);

    int created = pm.rebuildStructure();
    EXPECT_EQ(created, 6);  // content/unsorted, presets, objects, exports, snapshots, .cache/thumbnails

    EXPECT_TRUE(fs::exists(scratch.root / "content"   / "unsorted"));
    EXPECT_TRUE(fs::exists(scratch.root / "presets"));
    EXPECT_TRUE(fs::exists(scratch.root / "objects"));
    EXPECT_TRUE(fs::exists(scratch.root / "exports"));
    EXPECT_TRUE(fs::exists(scratch.root / "snapshots"));
    EXPECT_TRUE(fs::exists(scratch.root / ".cache"    / "thumbnails"));
}

TEST(ProjectManagerRebuild, IdempotentSecondRunCreatesNothing) {
    TempScratch scratch("rebuild-idem");
    fs::path projectFile = scratch.root / "Show.entity";
    writeFile(projectFile, "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    pm.rebuildStructure();

    int second = pm.rebuildStructure();
    EXPECT_EQ(second, 0);  // already-created dirs aren't double-counted
}

TEST(ProjectManagerRebuild, NoProjectLoadedIsNoop) {
    entity::ProjectManager pm;
    EXPECT_EQ(pm.rebuildStructure(), 0);
}

TEST(ProjectManagerRebuild, OnlyMissingDirsAreCreated) {
    TempScratch scratch("rebuild-partial");
    fs::path projectFile = scratch.root / "Show.entity";
    writeFile(projectFile, "{}");

    // Pre-create some subdirs so rebuild only fills the gaps.
    fs::create_directories(scratch.root / "presets");
    fs::create_directories(scratch.root / "content" / "unsorted");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    int created = pm.rebuildStructure();
    EXPECT_EQ(created, 4);  // objects + exports + snapshots + .cache/thumbnails
}

// --- findMissingManagedMedia ----------------------------------------------

TEST(ProjectManagerFindMedia, RestoresMatchingFilenameIntoCanonicalPath) {
    TempScratch scratch("find-basic");
    fs::path projectFile = scratch.root / "project" / "Show.entity";
    writeFile(projectFile, "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    pm.addMediaFile("content/act1/intro.mov",
                    entity::ProjectManager::PathKind::Managed);
    pm.addMediaFile("content/act1/missing.mov",
                    entity::ProjectManager::PathKind::Managed);

    // Project root is missing all media. Set up a "backup" folder with
    // one of the two matching files.
    fs::path backup = scratch.root / "backup_archive";
    writeFile(backup / "deeper" / "intro.mov", "intro contents");
    // Note: missing.mov is NOT in the backup — should remain unrestored.

    auto result = pm.findMissingManagedMedia(backup);
    EXPECT_EQ(result.missingBefore, 2);
    EXPECT_EQ(result.matched,        1);
    EXPECT_EQ(result.copied,         1);
    EXPECT_EQ(result.stillMissing,   1);

    // intro.mov copied to canonical path with parent dirs created.
    fs::path canonical = projectFile.parent_path()
                       / "content" / "act1" / "intro.mov";
    ASSERT_TRUE(fs::exists(canonical));
    EXPECT_EQ(readFile(canonical), "intro contents");

    // missing.mov was not in backup, so not created.
    fs::path stillMissing = projectFile.parent_path()
                          / "content" / "act1" / "missing.mov";
    EXPECT_FALSE(fs::exists(stillMissing));

    // Source file in backup folder NOT deleted.
    EXPECT_TRUE(fs::exists(backup / "deeper" / "intro.mov"));
}

TEST(ProjectManagerFindMedia, SkipsLinkedEntries) {
    TempScratch scratch("find-skips-linked");
    fs::path projectFile = scratch.root / "project" / "Show.entity";
    writeFile(projectFile, "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    pm.addMediaFile("C:/external/foo.mov",
                    entity::ProjectManager::PathKind::Linked);

    fs::path backup = scratch.root / "backup";
    writeFile(backup / "foo.mov", "data");

    auto result = pm.findMissingManagedMedia(backup);
    // Linked entries are NOT considered missing for this op — they're
    // user-controlled. missingBefore counts only Managed.
    EXPECT_EQ(result.missingBefore, 0);
    EXPECT_EQ(result.matched,       0);
    EXPECT_EQ(result.copied,        0);
}

TEST(ProjectManagerFindMedia, NothingMissingIsNoop) {
    TempScratch scratch("find-nothing");
    fs::path projectFile = scratch.root / "project" / "Show.entity";
    writeFile(projectFile, "{}");

    // Managed entry whose canonical path IS already on disk.
    writeFile(scratch.root / "project" / "content" / "a.mov", "x");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    pm.addMediaFile("content/a.mov",
                    entity::ProjectManager::PathKind::Managed);

    fs::path backup = scratch.root / "backup";
    fs::create_directories(backup);

    auto result = pm.findMissingManagedMedia(backup);
    EXPECT_EQ(result.missingBefore, 0);
    EXPECT_EQ(result.copied,        0);
}

TEST(ProjectManagerFindMedia, NonExistentSearchDirIsNoop) {
    TempScratch scratch("find-bogus");
    fs::path projectFile = scratch.root / "project" / "Show.entity";
    writeFile(projectFile, "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    pm.addMediaFile("content/x.mov",
                    entity::ProjectManager::PathKind::Managed);

    auto result = pm.findMissingManagedMedia("C:/this/path/does/not/exist");
    // Bails before counting missing entries — treats unreachable searchDir
    // as a no-op, not an error.
    EXPECT_EQ(result.missingBefore, 0);
    EXPECT_EQ(result.copied,        0);
}

TEST(ProjectManagerFindMedia, FirstMatchWinsForDuplicateFilenames) {
    TempScratch scratch("find-dup");
    fs::path projectFile = scratch.root / "project" / "Show.entity";
    writeFile(projectFile, "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    pm.addMediaFile("content/dup.mov",
                    entity::ProjectManager::PathKind::Managed);

    fs::path backup = scratch.root / "backup";
    // Two files with the same basename but different bodies. We don't
    // assert which one wins (depends on the filesystem walk order), only
    // that something is copied and the result count is consistent.
    writeFile(backup / "a" / "dup.mov", "version A");
    writeFile(backup / "b" / "dup.mov", "version B");

    auto result = pm.findMissingManagedMedia(backup);
    EXPECT_EQ(result.missingBefore, 1);
    EXPECT_EQ(result.matched,       1);
    EXPECT_EQ(result.copied,        1);
    EXPECT_EQ(result.stillMissing,  0);

    fs::path canonical = projectFile.parent_path() / "content" / "dup.mov";
    ASSERT_TRUE(fs::exists(canonical));
    const std::string body = readFile(canonical);
    EXPECT_TRUE(body == "version A" || body == "version B");
}
