// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
//
// Unit tests for RenameTrackCommand and the v28 TrackUiState serializer
// contract.
//
// Coverage:
//   - RenameTrackCommand toJson / fromJson round-trip.
//   - getTypeName / getDescription return correct strings.
//   - TrackUiState component direct-write (the same registry path that
//     execute() / undo() use). No Engine / D3D12 dependency.
//   - ProjectSerializer v28 round-trip: named + shy track saved and
//     restored correctly.
//   - Legacy tolerance: a file without "name" / "shy" keys loads with
//     empty name and shy=false.

#include <gtest/gtest.h>

#include "entity/command/Commands.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/TrackUiState.hpp"
#include "entity/project/ProjectSerializer.hpp"
#include "entity/timeline/Timeline.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace entity;
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers

static fs::path tempPath(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path()
         / ("entity_track_name_test_" + std::string(tag) + "_"
            + std::to_string(stamp) + ".entity");
}

struct TempFile {
    fs::path path;
    explicit TempFile(const char* tag) : path(tempPath(tag)) {}
    ~TempFile() { std::error_code ec; fs::remove(path, ec); }
};

// ---------------------------------------------------------------------------
// RenameTrackCommand -- JSON / description tests (no Engine required)

TEST(RenameTrackCommandJson, GetTypeName) {
    RenameTrackCommand cmd(2, "My Track");
    EXPECT_EQ(std::string(cmd.getTypeName()), "RenameTrack");
}

TEST(RenameTrackCommandJson, GetDescription) {
    RenameTrackCommand cmd(0, "Intro");
    EXPECT_NE(cmd.getDescription().find("Intro"), std::string::npos);
    // Description should reference track 1 (1-based) for index 0.
    EXPECT_NE(cmd.getDescription().find("1"), std::string::npos);
}

TEST(RenameTrackCommandJson, ToJsonRoundTrip) {
    RenameTrackCommand cmd(3, "Finale");
    auto j = cmd.toJson();
    EXPECT_EQ(j.value("type", std::string{}), "RenameTrack");
    EXPECT_EQ(j.value("trackIndex", -1), 3);
    EXPECT_EQ(j.value("newName", std::string{}), "Finale");
}

TEST(RenameTrackCommandJson, FromJsonRoundTrip) {
    nlohmann::json j = {{"type","RenameTrack"},{"trackIndex",1},{"newName","Act 2"}};
    auto cmd = RenameTrackCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(std::string(cmd->getTypeName()), "RenameTrack");
    // The reconstructed command should re-serialize with the same fields.
    auto j2 = cmd->toJson();
    EXPECT_EQ(j2.value("trackIndex", -1), 1);
    EXPECT_EQ(j2.value("newName", std::string{}), "Act 2");
}

TEST(RenameTrackCommandJson, FromJsonDefaultsOnMissingFields) {
    nlohmann::json j = {{"type","RenameTrack"}};
    auto cmd = RenameTrackCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    auto j2 = cmd->toJson();
    EXPECT_EQ(j2.value("trackIndex", -1), 0);
    EXPECT_EQ(j2.value("newName", std::string{"sentinel"}), "");
}

// ---------------------------------------------------------------------------
// TrackUiState direct-registry tests (mirrors execute() / undo() logic)
// No Engine required — tests the actual component path the command uses.

class TrackUiStateTest : public ::testing::Test {
protected:
    entt::registry registry;
    std::unique_ptr<Timeline> timeline;
    entt::entity track{entt::null};

    void SetUp() override {
        timeline = std::make_unique<Timeline>(registry);
        track    = timeline->createTrack("Track 0");
    }
};

TEST_F(TrackUiStateTest, EmplaceAndReadName) {
    registry.emplace<TrackUiState>(track, TrackUiState{"Scene 1", false});
    const auto* ui = registry.try_get<TrackUiState>(track);
    ASSERT_NE(ui, nullptr);
    EXPECT_EQ(ui->name, "Scene 1");
    EXPECT_FALSE(ui->shy);
}

TEST_F(TrackUiStateTest, UpdateName) {
    registry.emplace<TrackUiState>(track, TrackUiState{"Old", false});
    auto* ui = registry.try_get<TrackUiState>(track);
    ASSERT_NE(ui, nullptr);
    std::string prev = ui->name;
    ui->name = "New";
    EXPECT_EQ(prev, "Old");
    EXPECT_EQ(registry.try_get<TrackUiState>(track)->name, "New");
}

TEST_F(TrackUiStateTest, UndoRestoresPreviousName) {
    // Simulate execute(): capture previous, write new.
    registry.emplace<TrackUiState>(track, TrackUiState{"Before", false});
    auto* ui = registry.try_get<TrackUiState>(track);
    std::string prev = ui->name;
    ui->name = "After";
    EXPECT_EQ(ui->name, "After");
    // Simulate undo(): restore previous.
    ui->name = prev;
    EXPECT_EQ(ui->name, "Before");
}

TEST_F(TrackUiStateTest, AbsentComponentMeansNoName) {
    // Without TrackUiState the renderer falls back to "Track N".
    EXPECT_EQ(registry.try_get<TrackUiState>(track), nullptr);
}

// ---------------------------------------------------------------------------
// ProjectSerializer v28 round-trip tests

TEST(ProjectSerializerV28, NamedShy_RoundTrip) {
    TempFile tf("v28_named_shy");

    // --- save ---
    {
        entt::registry reg;
        Timeline tl(reg);
        entt::entity te = tl.createTrack("Track 0");
        reg.emplace<TrackUiState>(te, TrackUiState{"Main Stage", true});

        bool ok = ProjectSerializer::save(tl, tf.path);
        ASSERT_TRUE(ok) << "Save failed: " << ProjectSerializer::getLastError();
    }

    // --- load ---
    {
        entt::registry reg;
        Timeline tl(reg);

        bool ok = ProjectSerializer::load(tl, tf.path);
        ASSERT_TRUE(ok) << "Load failed: " << ProjectSerializer::getLastError();

        const auto& tracks = tl.getTracks();
        ASSERT_EQ(tracks.size(), 1u);

        const auto* ui = reg.try_get<TrackUiState>(tracks[0]);
        ASSERT_NE(ui, nullptr);
        EXPECT_EQ(ui->name, "Main Stage");
        EXPECT_TRUE(ui->shy);
    }
}

TEST(ProjectSerializerV28, EmptyName_NotSerialized_NoComponent) {
    TempFile tf("v28_empty_name");

    // A track with no TrackUiState should round-trip without one.
    {
        entt::registry reg;
        Timeline tl(reg);
        tl.createTrack("Track 0");
        bool ok = ProjectSerializer::save(tl, tf.path);
        ASSERT_TRUE(ok);
    }

    {
        entt::registry reg;
        Timeline tl(reg);
        bool ok = ProjectSerializer::load(tl, tf.path);
        ASSERT_TRUE(ok);

        const auto& tracks = tl.getTracks();
        ASSERT_EQ(tracks.size(), 1u);
        // No name/shy keys written -> no TrackUiState on load.
        EXPECT_EQ(reg.try_get<TrackUiState>(tracks[0]), nullptr);
    }
}

TEST(ProjectSerializerV28, LegacyFile_NoNameKey_LoadsWithDefaults) {
    // Write a project file that looks like a v27 file (no "name"/"shy" keys).
    TempFile tf("v28_legacy");
    {
        nlohmann::json j;
        j["format"]    = "entity_project";
        j["version"]   = 27;
        j["frameRate"] = 30.0;
        j["tracks"]    = nlohmann::json::array();
        nlohmann::json track;
        track["index"]  = 0;
        track["layers"] = nlohmann::json::array();
        j["tracks"].push_back(track);
        std::ofstream ofs(tf.path);
        ofs << j.dump(2);
    }

    entt::registry reg;
    Timeline tl(reg);
    bool ok = ProjectSerializer::load(tl, tf.path);
    ASSERT_TRUE(ok) << "Load of legacy file failed: " << ProjectSerializer::getLastError();

    const auto& tracks = tl.getTracks();
    ASSERT_EQ(tracks.size(), 1u);
    // Legacy file has no name/shy keys -> no TrackUiState emplaced.
    EXPECT_EQ(reg.try_get<TrackUiState>(tracks[0]), nullptr);
}
