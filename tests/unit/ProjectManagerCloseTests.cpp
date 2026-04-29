/**
 * Unit tests for `ProjectManager::closeProject()` (ADR-0009).
 *
 * The PM-level contract is small but load-bearing: closeProject() must
 * drop every piece of state that pins the manager to a specific project,
 * so the next createNew/load lands in a clean slate. Tests here lock
 * down the field-by-field clearing without exercising the Engine's
 * orchestration (output windows, transcodes, ECS entities, undo stack)
 * which is integration-tested via the full-launch path.
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
                  / ("entity_close_test_" + std::string(tag) + "_"
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

}  // namespace

TEST(ProjectManagerClose, ClearsPathAndMediaLibrary) {
    TempProject tp("clears_state");
    entity::ProjectManager pm;
    pm.setProjectPath(tp.projectFile);
    pm.addMediaFile("C:/external/a.mov", entity::ProjectManager::PathKind::Linked);
    pm.addMediaFile("content/act1/b.mov", entity::ProjectManager::PathKind::Managed);

    ASSERT_FALSE(pm.projectPath().empty());
    ASSERT_EQ(pm.loadedMediaFiles().size(), 2u);

    pm.closeProject();

    EXPECT_TRUE(pm.projectPath().empty());
    EXPECT_TRUE(pm.loadedMediaFiles().empty());
}

TEST(ProjectManagerClose, ResetsNonHapImportPolicyToAsk) {
    entity::ProjectManager pm;
    pm.setNonHapImportPolicy(
        entity::ProjectManager::NonHapImportPolicy::AlwaysTranscode);
    ASSERT_EQ(pm.nonHapImportPolicy(),
              entity::ProjectManager::NonHapImportPolicy::AlwaysTranscode);

    pm.closeProject();

    // Policy is per-project (persisted in the .entity file). On close
    // it goes back to the default so the next New Project / Open
    // doesn't inherit the previous show's preference.
    EXPECT_EQ(pm.nonHapImportPolicy(),
              entity::ProjectManager::NonHapImportPolicy::Ask);
}

TEST(ProjectManagerClose, IsIdempotentOnEmptyState) {
    entity::ProjectManager pm;

    // First call: nothing to clear.
    pm.closeProject();
    EXPECT_TRUE(pm.projectPath().empty());
    EXPECT_TRUE(pm.loadedMediaFiles().empty());

    // Second call: still safe.
    pm.closeProject();
    EXPECT_TRUE(pm.projectPath().empty());
    EXPECT_TRUE(pm.loadedMediaFiles().empty());
}

TEST(ProjectManagerClose, AllowsReuseAfterClose) {
    TempProject tp1("reuse_a");
    entity::ProjectManager pm;
    pm.setProjectPath(tp1.projectFile);
    pm.addMediaFile("C:/old/file.mov", entity::ProjectManager::PathKind::Linked);

    pm.closeProject();

    // After close we can drive the same instance into a different
    // project with no leftover state from the first one.
    TempProject tp2("reuse_b");
    pm.setProjectPath(tp2.projectFile);
    pm.addMediaFile("content/new/file.mov",
                    entity::ProjectManager::PathKind::Managed);

    EXPECT_EQ(pm.projectPath(), tp2.projectFile);
    ASSERT_EQ(pm.loadedMediaFiles().size(), 1u);
    EXPECT_EQ(pm.loadedMediaFiles()[0].originalPath, "content/new/file.mov");
    EXPECT_EQ(pm.loadedMediaFiles()[0].pathKind,
              entity::ProjectManager::PathKind::Managed);
}
