/**
 * Unit tests for ProjectManager::collectLinkedIntoProject (ADR-0009).
 *
 * Each test sets up a tiny on-disk project (root + .entity file), wires
 * up a few mediaLibrary entries (mix of Linked / Managed, with and
 * without cache-dir transcodes), runs collect, and asserts on:
 *  - the on-disk shape (canonical content path, archive sibling)
 *  - the in-memory entry state (pathKind, originalPath, archivedOriginal)
 *  - the returned rewrites list (Engine uses these to update clip.filepath)
 *
 * No EnTT / D3D12 / FFmpeg involved — collectLinkedIntoProject is pure
 * project-state plumbing. The clip-side rewrite happens on Engine and
 * is exercised end-to-end via the integration test surface.
 */

#include <gtest/gtest.h>

#include "entity/project/ProjectManager.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeTempProjectRoot(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path()
                  / ("entity_collect_test_" + std::string(tag) + "_"
                     + std::to_string(stamp));
    fs::create_directories(root);
    return root;
}

struct TempProject {
    fs::path root;
    fs::path projectFile;

    explicit TempProject(const char* tag) : root(makeTempProjectRoot(tag)) {
        projectFile = root / "Show.entity";
        std::ofstream(projectFile) << "{}";
    }
    ~TempProject() {
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

TEST(ProjectManagerCollect, LinkedSourceOnlyCopiesToContentUnsorted) {
    TempProject tp("linked-only");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    // External source — outside the project root.
    fs::path externalDir = tp.root.parent_path() / "external_media";
    fs::create_directories(externalDir);
    fs::path externalFile = externalDir / "intro.mov";
    writeFile(externalFile, "fake source");

    pm.addMediaFile(externalFile.string(),
                    entity::ProjectManager::PathKind::Linked);

    auto result = pm.collectLinkedIntoProject();
    EXPECT_EQ(result.collected, 1);
    EXPECT_EQ(result.missing,   0);
    ASSERT_EQ(result.rewrites.size(), 1u);
    EXPECT_EQ(result.rewrites[0].first,  externalFile.string());
    EXPECT_EQ(result.rewrites[0].second, "content/unsorted/intro.mov");

    // Canonical file copied into the project.
    EXPECT_TRUE(fs::exists(tp.root / "content" / "unsorted" / "intro.mov"));
    // No archive — there was no transcode to displace.
    EXPECT_FALSE(fs::exists(tp.root / "content" / "unsorted" / ".archive"));
    // External source NOT deleted.
    EXPECT_TRUE(fs::exists(externalFile));

    // Entry flipped to Managed with project-relative originalPath.
    const auto* entry = pm.findEntry("content/unsorted/intro.mov");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->pathKind, entity::ProjectManager::PathKind::Managed);
    EXPECT_TRUE(entry->archivedOriginal.empty());
    EXPECT_TRUE(entry->originalCodec.empty());
    // Old (absolute) entry is gone.
    EXPECT_EQ(pm.findEntry(externalFile.string()), nullptr);

    std::error_code ec;
    fs::remove_all(externalDir, ec);
}

TEST(ProjectManagerCollect, LinkedWithCacheTranscodeArchivesOriginalAndRelocatesTranscode) {
    TempProject tp("linked-with-transcode");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    fs::path externalDir = tp.root.parent_path() / "external_media";
    fs::create_directories(externalDir);
    fs::path externalSrc = externalDir / "clip.mov";
    writeFile(externalSrc, "ProRes-shaped bytes");

    fs::path cacheDir = tp.root.parent_path() / "cache";
    fs::create_directories(cacheDir);
    fs::path cacheTrans = cacheDir / "clip_abc123.hap_alpha.mov";
    writeFile(cacheTrans, "HAP transcode bytes");

    auto& entry = pm.addMediaFile(externalSrc.string(),
                                   entity::ProjectManager::PathKind::Linked);
    entry.transcodedPath = cacheTrans.string();
    entry.variant        = "hap_alpha";

    auto result = pm.collectLinkedIntoProject();
    EXPECT_EQ(result.collected, 1);

    // Canonical content path holds what was the cache-dir transcode.
    fs::path canonical = tp.root / "content" / "unsorted" / "clip.mov";
    ASSERT_TRUE(fs::exists(canonical));
    {
        std::ifstream in(canonical, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), {});
        EXPECT_EQ(body, "HAP transcode bytes");
    }

    // Pre-transcode source archived next to it.
    fs::path archive = tp.root / "content" / "unsorted" / ".archive" / "clip.mov";
    ASSERT_TRUE(fs::exists(archive));
    {
        std::ifstream in(archive, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(in)), {});
        EXPECT_EQ(body, "ProRes-shaped bytes");
    }

    // Cache-dir transcode is gone (moved, not copied).
    EXPECT_FALSE(fs::exists(cacheTrans));
    // External source still present (we copied it, didn't move).
    EXPECT_TRUE(fs::exists(externalSrc));

    // Entry state.
    const auto* fresh = pm.findEntry("content/unsorted/clip.mov");
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(fresh->pathKind, entity::ProjectManager::PathKind::Managed);
    EXPECT_EQ(fresh->archivedOriginal, "content/unsorted/.archive/clip.mov");
    // .mov with non-HAP bytes — detectMediaType falls back to ProRes 4444.
    EXPECT_EQ(fresh->originalCodec, "ProRes 4444");
    // transcodedPath cleared (Managed convention).
    EXPECT_TRUE(fresh->transcodedPath.empty());

    std::error_code ec;
    fs::remove_all(externalDir, ec);
    fs::remove_all(cacheDir, ec);
}

TEST(ProjectManagerCollect, AlreadyManagedIsSkipped) {
    TempProject tp("already-managed");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    pm.addMediaFile("content/unsorted/x.mov",
                    entity::ProjectManager::PathKind::Managed);

    auto result = pm.collectLinkedIntoProject();
    EXPECT_EQ(result.collected,      0);
    EXPECT_EQ(result.missing,        0);
    EXPECT_EQ(result.alreadyManaged, 1);
    EXPECT_TRUE(result.rewrites.empty());

    // Entry untouched.
    const auto* entry = pm.findEntry("content/unsorted/x.mov");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->pathKind, entity::ProjectManager::PathKind::Managed);
}

TEST(ProjectManagerCollect, MissingLinkedSourceIsLeftAsLinked) {
    TempProject tp("missing-source");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    pm.addMediaFile("C:/no/such/path.mov",
                    entity::ProjectManager::PathKind::Linked);

    auto result = pm.collectLinkedIntoProject();
    EXPECT_EQ(result.collected, 0);
    EXPECT_EQ(result.missing,   1);

    // Entry unchanged.
    const auto* entry = pm.findEntry("C:/no/such/path.mov");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->pathKind, entity::ProjectManager::PathKind::Linked);
}

TEST(ProjectManagerCollect, FilenameCollisionsAreSuffixed) {
    TempProject tp("collisions");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    fs::path dirA = tp.root.parent_path() / "external_a";
    fs::path dirB = tp.root.parent_path() / "external_b";
    fs::create_directories(dirA);
    fs::create_directories(dirB);
    writeFile(dirA / "clip.mov", "from A");
    writeFile(dirB / "clip.mov", "from B");

    pm.addMediaFile((dirA / "clip.mov").string(),
                    entity::ProjectManager::PathKind::Linked);
    pm.addMediaFile((dirB / "clip.mov").string(),
                    entity::ProjectManager::PathKind::Linked);

    auto result = pm.collectLinkedIntoProject();
    EXPECT_EQ(result.collected, 2);

    // First lands as "clip.mov", second gets " (1)" suffix.
    EXPECT_TRUE(fs::exists(tp.root / "content" / "unsorted" / "clip.mov"));
    EXPECT_TRUE(fs::exists(tp.root / "content" / "unsorted" / "clip (1).mov"));

    std::error_code ec;
    fs::remove_all(dirA, ec);
    fs::remove_all(dirB, ec);
}

TEST(ProjectManagerCollect, CustomSubfolderHonored) {
    TempProject tp("custom-sub");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);

    fs::path dir = tp.root.parent_path() / "external_custom";
    fs::create_directories(dir);
    writeFile(dir / "intro.mov", "x");

    pm.addMediaFile((dir / "intro.mov").string(),
                    entity::ProjectManager::PathKind::Linked);

    auto result = pm.collectLinkedIntoProject("act1");
    EXPECT_EQ(result.collected, 1);
    EXPECT_TRUE(fs::exists(tp.root / "content" / "act1" / "intro.mov"));
    ASSERT_EQ(result.rewrites.size(), 1u);
    EXPECT_EQ(result.rewrites[0].second, "content/act1/intro.mov");

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST(ProjectManagerCollect, NoProjectLoadedIsNoop) {
    entity::ProjectManager pm;
    // Deliberately don't setProjectPath.
    pm.addMediaFile("C:/x/y.mov", entity::ProjectManager::PathKind::Linked);

    auto result = pm.collectLinkedIntoProject();
    EXPECT_EQ(result.collected, 0);
    EXPECT_EQ(result.missing,   0);
    EXPECT_EQ(result.alreadyManaged, 0);
    EXPECT_TRUE(result.rewrites.empty());
}
