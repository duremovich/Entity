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

// --- restoreOriginal ------------------------------------------------------

TEST(ProjectManagerRestoreOriginal, SwapsArchiveIntoCanonicalAndClearsFields) {
    TempScratch scratch("restore-basic");
    fs::path projectFile = scratch.root / "project" / "Show.entity";
    writeFile(projectFile, "{}");

    // Set up a Managed entry that's been transcode-replaced: canonical
    // path holds (fake) HAP bytes; archive holds the original ProRes bytes.
    const fs::path canonical = projectFile.parent_path()
                              / "content" / "act1" / "intro.mov";
    const fs::path archive   = projectFile.parent_path()
                              / "content" / "act1" / ".archive" / "intro.mov";
    writeFile(canonical, "HAP transcode bytes");
    writeFile(archive,   "ProRes original bytes");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    auto& entry = pm.addMediaFile("content/act1/intro.mov",
                                   entity::ProjectManager::PathKind::Managed);
    entry.archivedOriginal = "content/act1/.archive/intro.mov";
    entry.originalCodec    = "ProRes 4444";

    ASSERT_TRUE(pm.restoreOriginal("content/act1/intro.mov"));

    // Canonical path now holds the original bytes.
    ASSERT_TRUE(fs::exists(canonical));
    EXPECT_EQ(readFile(canonical), "ProRes original bytes");

    // Archive is gone (renamed into canonical).
    EXPECT_FALSE(fs::exists(archive));

    // Entry fields cleared.
    const auto* fresh = pm.findEntry("content/act1/intro.mov");
    ASSERT_NE(fresh, nullptr);
    EXPECT_TRUE(fresh->archivedOriginal.empty());
    EXPECT_TRUE(fresh->originalCodec.empty());
    EXPECT_TRUE(fresh->transcodedPath.empty());
    EXPECT_EQ(fresh->pathKind, entity::ProjectManager::PathKind::Managed);
}

TEST(ProjectManagerRestoreOriginal, RefusesIfNoArchiveSet) {
    TempScratch scratch("restore-no-archive");
    fs::path projectFile = scratch.root / "Show.entity";
    writeFile(projectFile, "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    pm.addMediaFile("content/x.mov",
                    entity::ProjectManager::PathKind::Managed);

    EXPECT_FALSE(pm.restoreOriginal("content/x.mov"));
}

TEST(ProjectManagerRestoreOriginal, RefusesForLinkedEntry) {
    TempScratch scratch("restore-linked");
    fs::path projectFile = scratch.root / "Show.entity";
    writeFile(projectFile, "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    auto& e = pm.addMediaFile("C:/external/x.mov",
                               entity::ProjectManager::PathKind::Linked);
    e.archivedOriginal = "anything";  // shouldn't matter for Linked

    EXPECT_FALSE(pm.restoreOriginal("C:/external/x.mov"));
}

TEST(ProjectManagerRestoreOriginal, RefusesIfArchiveFileMissing) {
    TempScratch scratch("restore-missing-archive");
    fs::path projectFile = scratch.root / "project" / "Show.entity";
    writeFile(projectFile, "{}");

    // Canonical exists; archive does NOT (filesystem state inconsistent
    // with the stored archivedOriginal — could happen if the user deleted
    // .archive/ by hand).
    writeFile(projectFile.parent_path() / "content" / "x.mov", "HAP bytes");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);
    auto& e = pm.addMediaFile("content/x.mov",
                               entity::ProjectManager::PathKind::Managed);
    e.archivedOriginal = "content/.archive/x.mov";  // doesn't exist on disk

    EXPECT_FALSE(pm.restoreOriginal("content/x.mov"));
    // Canonical untouched.
    EXPECT_TRUE(fs::exists(projectFile.parent_path() / "content" / "x.mov"));
    // Entry fields untouched.
    const auto* still = pm.findEntry("content/x.mov");
    ASSERT_NE(still, nullptr);
    EXPECT_FALSE(still->archivedOriginal.empty());
}

TEST(ProjectManagerRestoreOriginal, RefusesForUnregisteredPath) {
    TempScratch scratch("restore-unknown");
    fs::path projectFile = scratch.root / "Show.entity";
    writeFile(projectFile, "{}");

    entity::ProjectManager pm;
    pm.setProjectPath(projectFile);

    EXPECT_FALSE(pm.restoreOriginal("content/never-added.mov"));
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
