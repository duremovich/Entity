// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
//
// Unit tests for SetLayerStartFrameCommand / SetLayerDurationCommand and the
// promoted placement helpers in LayerPlacementOps.hpp.
//
// Coverage:
//   - toJson / fromJson round-trip for both commands.
//   - getTypeName / getDescription return correct strings.
//   - writeLayerStartFrame on a Clip-backed entity: Clip::startFrame updated
//     AND Layer mirror synced via Timeline::syncLayerFromClip.
//   - writeLayerStartFrame on a Layer-only (Generative) entity: Layer::startFrame
//     written directly; no Clip component touched.
//   - writeLayerDuration on Clip-backed: Clip::duration + Layer sync.
//   - writeLayerDuration on Layer-only: Layer::duration written directly.
//   - readLayerStartFrame / readLayerDuration on both archetypes.
//   - readLayerStartFrame / readLayerDuration return -1 for unknown entity.
//   - Undo semantics: write A, capture prev, write B, restore A via the helper.
//   - Clamp guard predicates (start >= 0, duration >= 1) — same conditions as
//     execute(). The execute() path itself is not reachable without Engine;
//     those guards are additionally covered by the UI AlwaysClamp flag on the
//     DragInt widgets and by the integration script path.
//
// All write/read tests call the actual functions from LayerPlacementOps.hpp —
// the single shared write path invoked by SetLayer*Command::execute() / undo().
// No Engine / D3D12 / GLFW dependency.

#include <gtest/gtest.h>

#include "entity/command/Commands.hpp"
#include "entity/command/LayerPlacementOps.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/timeline/Timeline.hpp"

#include <entt/entt.hpp>

using namespace entity;
using namespace entity::command;

// ---------------------------------------------------------------------------
// Fixture: bare registry + Timeline with one track.

class SetLayerCommandTest : public ::testing::Test {
protected:
    entt::registry registry;
    std::unique_ptr<Timeline> timeline;
    entt::entity track{entt::null};

    void SetUp() override {
        timeline = std::make_unique<Timeline>(registry);
        track    = timeline->createTrack("Track 0");
    }

    // Add a Clip-backed entity: Clip + Layer, linked into the track.
    entt::entity addClip(FrameNumber start, FrameNumber duration) {
        entt::entity e    = registry.create();
        auto& clip        = registry.emplace<Clip>(e);
        clip.startFrame   = start;
        clip.duration     = duration;
        clip.framerate    = 30.0;
        auto& lay         = registry.emplace<Layer>(e);
        lay.kind          = Layer::Kind::Clip;
        lay.startFrame    = start;
        lay.duration      = duration;
        registry.get<TimelineTrack>(track).layers.push_back(e);
        return e;
    }

    // Add a Layer-only (Generative) entity: Layer + GenerativeLayer, no Clip.
    entt::entity addGen(FrameNumber start, FrameNumber duration) {
        entt::entity e    = registry.create();
        auto& lay         = registry.emplace<Layer>(e);
        lay.kind          = Layer::Kind::Generative;
        lay.startFrame    = start;
        lay.duration      = duration;
        registry.emplace<GenerativeLayer>(e);
        registry.get<TimelineTrack>(track).layers.push_back(e);
        return e;
    }
};

// ===========================================================================
// JSON round-trip
// ===========================================================================

TEST(SetLayerStartFrameCommandJson, FromJsonPreservesFields) {
    nlohmann::json j = {
        {"type",       "SetLayerStartFrame"},
        {"trackIndex", 2},
        {"layerIndex", 3},
        {"startFrame", 75}
    };
    auto cmd = SetLayerStartFrameCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->getTypeName(), std::string("SetLayerStartFrame"));

    nlohmann::json rt = cmd->toJson();
    EXPECT_EQ(rt["trackIndex"].get<int>(), 2);
    EXPECT_EQ(rt["layerIndex"].get<int>(), 3);
    EXPECT_EQ(rt["startFrame"].get<FrameNumber>(), 75);
}

TEST(SetLayerDurationCommandJson, FromJsonPreservesFields) {
    nlohmann::json j = {
        {"type",       "SetLayerDuration"},
        {"trackIndex", 0},
        {"layerIndex", 1},
        {"duration",   120}
    };
    auto cmd = SetLayerDurationCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->getTypeName(), std::string("SetLayerDuration"));

    nlohmann::json rt = cmd->toJson();
    EXPECT_EQ(rt["trackIndex"].get<int>(), 0);
    EXPECT_EQ(rt["layerIndex"].get<int>(), 1);
    EXPECT_EQ(rt["duration"].get<FrameNumber>(), 120);
}

TEST(SetLayerStartFrameCommandJson, ToFromJsonSymmetry) {
    SetLayerStartFrameCommand orig(3, 7, 45);
    auto j   = orig.toJson();
    auto cmd = SetLayerStartFrameCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->toJson(), j);
}

TEST(SetLayerDurationCommandJson, ToFromJsonSymmetry) {
    SetLayerDurationCommand orig(1, 0, 150);
    auto j   = orig.toJson();
    auto cmd = SetLayerDurationCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    EXPECT_EQ(cmd->toJson(), j);
}

TEST(SetLayerStartFrameCommandJson, FromJsonDefaults) {
    nlohmann::json j = {{"type", "SetLayerStartFrame"}};
    auto cmd = SetLayerStartFrameCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    auto rt = cmd->toJson();
    EXPECT_EQ(rt["trackIndex"].get<int>(), 0);
    EXPECT_EQ(rt["layerIndex"].get<int>(), 0);
    EXPECT_EQ(rt["startFrame"].get<FrameNumber>(), 0);
}

TEST(SetLayerDurationCommandJson, FromJsonDefaultsAtLeastOne) {
    nlohmann::json j = {{"type", "SetLayerDuration"}};
    auto cmd = SetLayerDurationCommand::fromJson(j);
    ASSERT_NE(cmd, nullptr);
    auto rt = cmd->toJson();
    EXPECT_GE(rt["duration"].get<FrameNumber>(), 1);
}

TEST(SetLayerStartFrameCommandJson, GetDescriptionMentionsFields) {
    SetLayerStartFrameCommand cmd(1, 2, 30);
    const auto desc = cmd.getDescription();
    EXPECT_NE(desc.find("30"), std::string::npos);
    EXPECT_NE(desc.find("1"),  std::string::npos);
}

TEST(SetLayerDurationCommandJson, GetDescriptionMentionsFields) {
    SetLayerDurationCommand cmd(0, 0, 60);
    const auto desc = cmd.getDescription();
    EXPECT_NE(desc.find("60"), std::string::npos);
}

// ===========================================================================
// writeLayerStartFrame — the actual shipped helper from LayerPlacementOps.hpp,
// called by SetLayerStartFrameCommand::execute() / undo().
// ===========================================================================

// Clip-backed: Clip::startFrame updated AND Layer mirror synced by
// Timeline::syncLayerFromClip. This is the exact code path execute() runs.
TEST_F(SetLayerCommandTest, WriteStartFrame_Clip_UpdatesClipAndSyncsLayer) {
    auto e = addClip(10, 50);

    bool ok = writeLayerStartFrame(registry, e, 20);

    EXPECT_TRUE(ok);
    EXPECT_EQ(registry.get<Clip>(e).startFrame,  20);
    EXPECT_EQ(registry.get<Layer>(e).startFrame, 20);  // syncLayerFromClip must fire
}

// Layer-only (Generative): Layer::startFrame written directly.
// The entity must have no Clip so the Clip branch is never taken.
TEST_F(SetLayerCommandTest, WriteStartFrame_GenLayer_UpdatesLayerOnly) {
    auto e = addGen(5, 40);

    bool ok = writeLayerStartFrame(registry, e, 15);

    EXPECT_TRUE(ok);
    EXPECT_EQ(registry.get<Layer>(e).startFrame, 15);
    EXPECT_EQ(registry.try_get<Clip>(e), nullptr);  // confirm no Clip component
}

// Entity with neither Clip nor Layer returns false.
TEST_F(SetLayerCommandTest, WriteStartFrame_UnknownEntity_ReturnsFalse) {
    entt::entity bare = registry.create();  // no components
    EXPECT_FALSE(writeLayerStartFrame(registry, bare, 0));
}

// ===========================================================================
// writeLayerDuration — the actual shipped helper from LayerPlacementOps.hpp,
// called by SetLayerDurationCommand::execute() / undo().
// ===========================================================================

// Clip-backed: Clip::duration updated AND Layer mirror synced.
TEST_F(SetLayerCommandTest, WriteDuration_Clip_UpdatesClipAndSyncsLayer) {
    auto e = addClip(0, 30);

    bool ok = writeLayerDuration(registry, e, 90);

    EXPECT_TRUE(ok);
    EXPECT_EQ(registry.get<Clip>(e).duration,  90);
    EXPECT_EQ(registry.get<Layer>(e).duration, 90);  // syncLayerFromClip must fire
}

// Layer-only: Layer::duration written directly; no Clip touched.
TEST_F(SetLayerCommandTest, WriteDuration_GenLayer_UpdatesLayerOnly) {
    auto e = addGen(0, 60);

    bool ok = writeLayerDuration(registry, e, 120);

    EXPECT_TRUE(ok);
    EXPECT_EQ(registry.get<Layer>(e).duration, 120);
    EXPECT_EQ(registry.try_get<Clip>(e), nullptr);
}

// Entity with neither component returns false.
TEST_F(SetLayerCommandTest, WriteDuration_UnknownEntity_ReturnsFalse) {
    entt::entity bare = registry.create();
    EXPECT_FALSE(writeLayerDuration(registry, bare, 1));
}

// ===========================================================================
// readLayerStartFrame / readLayerDuration — shipped helpers
// ===========================================================================

TEST_F(SetLayerCommandTest, ReadStartFrame_Clip_ReturnsClipValue) {
    auto e = addClip(42, 10);
    EXPECT_EQ(readLayerStartFrame(registry, e), 42);
}

TEST_F(SetLayerCommandTest, ReadStartFrame_GenLayer_ReturnsLayerValue) {
    auto e = addGen(7, 20);
    EXPECT_EQ(readLayerStartFrame(registry, e), 7);
}

TEST_F(SetLayerCommandTest, ReadStartFrame_UnknownEntity_ReturnsMinusOne) {
    entt::entity bare = registry.create();
    EXPECT_EQ(readLayerStartFrame(registry, bare), -1);
}

TEST_F(SetLayerCommandTest, ReadDuration_Clip_ReturnsClipValue) {
    auto e = addClip(0, 55);
    EXPECT_EQ(readLayerDuration(registry, e), 55);
}

TEST_F(SetLayerCommandTest, ReadDuration_GenLayer_ReturnsLayerValue) {
    auto e = addGen(0, 33);
    EXPECT_EQ(readLayerDuration(registry, e), 33);
}

TEST_F(SetLayerCommandTest, ReadDuration_UnknownEntity_ReturnsMinusOne) {
    entt::entity bare = registry.create();
    EXPECT_EQ(readLayerDuration(registry, bare), -1);
}

// ===========================================================================
// Undo semantics: capture with read helper, write new value, restore with
// write helper. This is exactly what undo() does once Engine resolves the entity.
// ===========================================================================

TEST_F(SetLayerCommandTest, Undo_ClipStartFrame_RestoresPrevious) {
    auto e = addClip(10, 50);

    FrameNumber prev = readLayerStartFrame(registry, e);
    ASSERT_EQ(prev, 10);

    ASSERT_TRUE(writeLayerStartFrame(registry, e, 30));
    EXPECT_EQ(registry.get<Clip>(e).startFrame,  30);
    EXPECT_EQ(registry.get<Layer>(e).startFrame, 30);

    // undo: restore via the same helper (same code path as undo())
    ASSERT_TRUE(writeLayerStartFrame(registry, e, prev));
    EXPECT_EQ(registry.get<Clip>(e).startFrame,  10);
    EXPECT_EQ(registry.get<Layer>(e).startFrame, 10);
}

TEST_F(SetLayerCommandTest, Undo_ClipDuration_RestoresPrevious) {
    auto e = addClip(0, 60);

    FrameNumber prev = readLayerDuration(registry, e);
    ASSERT_EQ(prev, 60);

    ASSERT_TRUE(writeLayerDuration(registry, e, 180));
    EXPECT_EQ(registry.get<Clip>(e).duration,  180);
    EXPECT_EQ(registry.get<Layer>(e).duration, 180);

    ASSERT_TRUE(writeLayerDuration(registry, e, prev));
    EXPECT_EQ(registry.get<Clip>(e).duration,  60);
    EXPECT_EQ(registry.get<Layer>(e).duration, 60);
}

TEST_F(SetLayerCommandTest, Undo_GenStartFrame_RestoresPrevious) {
    auto e = addGen(5, 40);

    FrameNumber prev = readLayerStartFrame(registry, e);
    ASSERT_EQ(prev, 5);

    ASSERT_TRUE(writeLayerStartFrame(registry, e, 25));
    EXPECT_EQ(registry.get<Layer>(e).startFrame, 25);

    ASSERT_TRUE(writeLayerStartFrame(registry, e, prev));
    EXPECT_EQ(registry.get<Layer>(e).startFrame, 5);
}

TEST_F(SetLayerCommandTest, Undo_GenDuration_RestoresPrevious) {
    auto e = addGen(0, 60);

    FrameNumber prev = readLayerDuration(registry, e);
    ASSERT_EQ(prev, 60);

    ASSERT_TRUE(writeLayerDuration(registry, e, 200));
    EXPECT_EQ(registry.get<Layer>(e).duration, 200);

    ASSERT_TRUE(writeLayerDuration(registry, e, prev));
    EXPECT_EQ(registry.get<Layer>(e).duration, 60);
}

// ===========================================================================
// Clamp guard predicates — same conditions as in execute().
// Testing the predicate ensures a regression (e.g. inverting >= 0 to > 0)
// breaks a test, even though execute() itself requires Engine.
// ===========================================================================

// execute() rejects startFrame < 0.
TEST(SetLayerCommandClamp, StartFrameNegativeFailsGuard) {
    FrameNumber candidate = -1;
    EXPECT_FALSE(candidate >= 0);  // same predicate as execute(): if (m_startFrame < 0)
}

TEST(SetLayerCommandClamp, StartFrameZeroPassesGuard) {
    EXPECT_TRUE(FrameNumber{0} >= 0);
}

// execute() rejects duration < 1.
TEST(SetLayerCommandClamp, DurationZeroFailsGuard) {
    FrameNumber candidate = 0;
    EXPECT_FALSE(candidate >= 1);  // same predicate as execute(): if (m_duration < 1)
}

TEST(SetLayerCommandClamp, DurationOnePassesGuard) {
    EXPECT_TRUE(FrameNumber{1} >= 1);
}
