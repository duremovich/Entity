/**
 * Unit tests for ProjectManager's path resolution helpers (ADR-0009).
 *
 * resolveMediaPath / decoderPathFor have to handle a few subtle cases:
 *   - Linked entries: pass through unchanged (the link-in-place path).
 *   - Managed entries: project-relative path joined with the project root.
 *   - Managed entry but no project root yet: passthrough (caller treats
 *     that as "not openable until a project loads").
 *   - Path not in library: passthrough (covers the brief window during
 *     ingestion before addMediaFile fires).
 *   - decoderPathFor prefers transcoded path when on disk; the same
 *     Managed/Linked routing applies to transcoded entries.
 *
 * These guarantees underpin the import-flow rewrite — if any of them
 * regress, Managed projects will fail to open clips after a project move
 * or fail to round-trip via save+load.
 */

#include <gtest/gtest.h>

#include "entity/project/ProjectManager.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

fs::path makeTempProjectRoot(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path()
                  / ("entity_path_test_" + std::string(tag) + "_"
                     + std::to_string(stamp));
    fs::create_directories(root);
    return root;
}

struct TempProject {
    fs::path root;
    fs::path projectFile;

    explicit TempProject(const char* tag) : root(makeTempProjectRoot(tag)) {
        projectFile = root / "Show.entity";
        // Touch the project file so projectPath().parent_path() is meaningful.
        std::ofstream(projectFile) << "{}";
    }
    ~TempProject() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

}  // namespace

TEST(ProjectManagerPath, ResolveLinkedReturnsAbsoluteUnchanged) {
    TempProject tp("linked");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    const std::string abs = "C:/external/media/foo.mov";
    pm.addMediaFile(abs, entity::ProjectManager::PathKind::Linked);

    EXPECT_EQ(pm.resolveMediaPath(abs), abs);
}

TEST(ProjectManagerPath, ResolveManagedJoinsProjectRoot) {
    TempProject tp("managed");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    const std::string rel = "content/act1/intro.mov";
    pm.addMediaFile(rel, entity::ProjectManager::PathKind::Managed);

    fs::path expected = tp.root / "content/act1/intro.mov";
    fs::path actual(pm.resolveMediaPath(rel));
    // Compare canonicalized forms so trailing slash / separator differences
    // don't fail the test on Windows.
    EXPECT_EQ(actual.lexically_normal(), expected.lexically_normal());
}

TEST(ProjectManagerPath, ResolveManagedWithoutProjectRootIsPassthrough) {
    entity::ProjectManager pm;
    // Deliberately don't setProjectPath — simulates pre-load state.
    const std::string rel = "content/act1/intro.mov";
    pm.addMediaFile(rel, entity::ProjectManager::PathKind::Managed);

    EXPECT_EQ(pm.resolveMediaPath(rel), rel);
}

TEST(ProjectManagerPath, ResolveUnregisteredPathIsPassthrough) {
    TempProject tp("unregistered");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    // Not in the library — caller may be in the middle of ingestion.
    EXPECT_EQ(pm.resolveMediaPath("C:/some/abs/path.mov"),
              "C:/some/abs/path.mov");
}

TEST(ProjectManagerPath, ResolveEmptyIsEmpty) {
    entity::ProjectManager pm;
    EXPECT_EQ(pm.resolveMediaPath(""), "");
}

TEST(ProjectManagerPath, DecoderPathForLinkedFallsBackToOriginalWhenNoTranscode) {
    TempProject tp("linked-no-transcode");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    const std::string abs = (tp.root / "external.mov").string();
    pm.addMediaFile(abs, entity::ProjectManager::PathKind::Linked);

    // No transcodedPath set -> should return the original (resolved).
    EXPECT_EQ(pm.decoderPathFor(abs), abs);
}

TEST(ProjectManagerPath, DecoderPathForPrefersTranscodedWhenOnDisk) {
    TempProject tp("transcoded-on-disk");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    const std::string original = (tp.root / "src.mov").string();
    fs::path transPath = tp.root / "transcoded.mov";
    std::ofstream(transPath) << "fake hap";  // Just needs to exist on disk.

    pm.addMediaFile(original, entity::ProjectManager::PathKind::Linked);
    pm.setTranscodedPath(original, transPath.string(), "hap_alpha");

    EXPECT_EQ(pm.decoderPathFor(original), transPath.string());
}

TEST(ProjectManagerPath, DecoderPathForFallsBackWhenTranscodedMissing) {
    TempProject tp("transcoded-missing");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    const std::string original = (tp.root / "src.mov").string();
    pm.addMediaFile(original, entity::ProjectManager::PathKind::Linked);
    // Transcoded path points to a file that doesn't exist on disk —
    // common when the user cleaned the cache. decoderPathFor must still
    // give us *something* the decoder can attempt.
    pm.setTranscodedPath(original, "C:/nope/nonexistent.mov", "hap_alpha");

    EXPECT_EQ(pm.decoderPathFor(original), original);
}

TEST(ProjectManagerPath, AddMediaFileDefaultsToLinked) {
    entity::ProjectManager pm;
    auto& entry = pm.addMediaFile("C:/A.mov");
    EXPECT_EQ(entry.pathKind, entity::ProjectManager::PathKind::Linked);
}

TEST(ProjectManagerPath, AddMediaFileSetsKindOnFirstRegistration) {
    entity::ProjectManager pm;
    auto& entry = pm.addMediaFile("content/A.mov",
                                   entity::ProjectManager::PathKind::Managed);
    EXPECT_EQ(entry.pathKind, entity::ProjectManager::PathKind::Managed);
}

TEST(ProjectManagerPath, AddMediaFileIsIdempotentAndPreservesKind) {
    entity::ProjectManager pm;
    pm.addMediaFile("content/A.mov",
                    entity::ProjectManager::PathKind::Managed);
    // Re-add with a different kind — should NOT flip the existing entry.
    auto& again = pm.addMediaFile("content/A.mov",
                                   entity::ProjectManager::PathKind::Linked);
    EXPECT_EQ(again.pathKind, entity::ProjectManager::PathKind::Managed);
    EXPECT_EQ(pm.loadedMediaFiles().size(), 1u);
}
