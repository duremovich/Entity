#include <gtest/gtest.h>

#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Screen.hpp"
#include "entity/bus/Message.hpp"

#include <entt/entt.hpp>
#include <cstdint>

using namespace entity;

// ---------------------------------------------------------------------------
// OA show-side re-eval unit tests.
//
// These tests hand-construct a bus::SceneSnapshot with one
// ObjectAnimationLayerSnapshot carrying baked tracks and call
// buildRenderFrame(out, scene) directly — bypassing buildSceneSnapshot
// entirely. This isolates the 3.7 show-side code path at lines 966-996 of
// PlaybackTimeAuthority.cpp so the assertions fail if that block is removed
// or broken, regardless of whether buildSceneSnapshot also folds the same
// values in via ObjectAnimationOutput (the 3.4 editor-side path).
// ---------------------------------------------------------------------------

namespace {

// Build a minimal SceneSnapshot with one Screen and one OA layer targeting it.
// The OA layer has a single PositionX track with two keyframes: (0, startVal)
// and (duration-1, endVal). Linear interpolation.
bus::SceneSnapshot makeOASnapshot(std::uint64_t screenEntity,
                                  FrameNumber    layerStart,
                                  FrameNumber    layerDuration,
                                  float          startVal,
                                  float          endVal,
                                  AnimatableProperty prop = AnimatableProperty::PositionX,
                                  bus::OAEndBehavior endBehavior = bus::OAEndBehavior::Hold) {
    bus::SceneSnapshot scene;

    bus::ScreenSnapshot ss;
    ss.entity   = screenEntity;
    ss.name     = "TestScreen";
    ss.position = {0.0f, 0.0f, 0.0f};
    ss.rotation = {0.0f, 0.0f, 0.0f};
    ss.scale    = {1.0f, 1.0f, 1.0f};
    scene.screens.push_back(ss);

    bus::BakedKeyframe k0;
    k0.frame         = 0;
    k0.value         = startVal;
    k0.interpolation = static_cast<int>(InterpolationType::Linear);
    k0.easeIn        = 0.42f;
    k0.easeOut       = 0.58f;

    bus::BakedKeyframe k1;
    k1.frame         = layerDuration - 1;
    k1.value         = endVal;
    k1.interpolation = static_cast<int>(InterpolationType::Linear);
    k1.easeIn        = 0.42f;
    k1.easeOut       = 0.58f;

    bus::BakedTrack track;
    track.property  = static_cast<int>(prop);
    track.enabled   = true;
    track.keyframes = {k0, k1};

    bus::ObjectAnimationLayerSnapshot oas;
    oas.targetScreen = screenEntity;
    oas.startFrame   = layerStart;
    oas.duration     = layerDuration;
    oas.trackIndex   = 0;
    oas.endBehavior  = endBehavior;
    oas.tracks.push_back(std::move(track));

    scene.objectAnimationLayers.push_back(std::move(oas));
    return scene;
}

class OAShowSideReevalTest : public ::testing::Test {
protected:
    entt::registry        registry;
    Timeline              timeline{registry};
    PlaybackTimeAuthority auth{registry, &timeline};

    // A stable uint64 we use as the screen entity ID in snapshots.
    // The show-side path matches on ss.entity == oa.targetScreen, so the
    // actual entt::entity value doesn't matter as long as they agree.
    static constexpr std::uint64_t kScreenId = 42u;
};

} // namespace

// At the first keyframe (frame == layerStart), PositionX must equal startVal.
TEST_F(OAShowSideReevalTest, PositionX_AtLayerStart_EqualsStartVal) {
    timeline.seekToFrame(0);  // currentFrame = 0, layerStart = 0
    auto scene = makeOASnapshot(kScreenId, /*layerStart*/0, /*duration*/100,
                                /*startVal*/3.0f, /*endVal*/9.0f);
    bus::RenderFrame rf;
    auth.buildRenderFrame(rf, scene);

    ASSERT_EQ(rf.screens.size(), 1u);
    EXPECT_NEAR(rf.screens[0].position[0], 3.0f, 0.01f);
}

// At the last keyframe (frame == layerStart + duration - 1), PositionX must
// equal endVal.
TEST_F(OAShowSideReevalTest, PositionX_AtLayerEnd_EqualsEndVal) {
    timeline.seekToFrame(99);  // last keyframe at duration-1 = 99
    auto scene = makeOASnapshot(kScreenId, /*layerStart*/0, /*duration*/100,
                                /*startVal*/3.0f, /*endVal*/9.0f);
    bus::RenderFrame rf;
    auth.buildRenderFrame(rf, scene);

    ASSERT_EQ(rf.screens.size(), 1u);
    EXPECT_NEAR(rf.screens[0].position[0], 9.0f, 0.01f);
}

// At the midpoint frame the value must be interpolated linearly.
// Two keyframes: (0, 3.0) and (99, 9.0). At frame 33 (1/3 of the way),
// expected value = 3.0 + (9.0 - 3.0) * (33/99) = 3.0 + 2.0 = 5.0.
TEST_F(OAShowSideReevalTest, PositionX_AtMidpoint_IsLinearlyInterpolated) {
    timeline.seekToFrame(33);
    auto scene = makeOASnapshot(kScreenId, /*layerStart*/0, /*duration*/100,
                                /*startVal*/3.0f, /*endVal*/9.0f);
    bus::RenderFrame rf;
    auth.buildRenderFrame(rf, scene);

    ASSERT_EQ(rf.screens.size(), 1u);
    EXPECT_NEAR(rf.screens[0].position[0], 5.0f, 0.05f);
}

// ADR-0020 Reset mode: when currentFrame is past the layer end window, the
// override is cleared and the screen falls back to its base position. This
// is opt-in via OAEndBehavior::Reset; the new default is Hold.
TEST_F(OAShowSideReevalTest, EndBehavior_Reset_AfterEnd_DoesNotOverrideScreen) {
    timeline.seekToFrame(200);  // well past duration=100
    auto scene = makeOASnapshot(kScreenId, /*layerStart*/0, /*duration*/100,
                                /*startVal*/3.0f, /*endVal*/9.0f,
                                AnimatableProperty::PositionX,
                                bus::OAEndBehavior::Reset);
    // The editor-side bake skips after-end-Reset layers from the bus entirely.
    // Mimic that here so the show-side re-eval has nothing to fold from.
    scene.objectAnimationLayers.clear();

    bus::RenderFrame rf;
    auth.buildRenderFrame(rf, scene);

    ASSERT_EQ(rf.screens.size(), 1u);
    EXPECT_NEAR(rf.screens[0].position[0], 0.0f, 0.001f);
}

// ADR-0020 Hold mode (default): past the layer end window, the show thread
// continues to apply the last evaluated value so the screen stays parked.
TEST_F(OAShowSideReevalTest, EndBehavior_Hold_AfterEnd_KeepsLastValue) {
    timeline.seekToFrame(200);  // well past duration=100
    auto scene = makeOASnapshot(kScreenId, /*layerStart*/0, /*duration*/100,
                                /*startVal*/3.0f, /*endVal*/9.0f,
                                AnimatableProperty::PositionX,
                                bus::OAEndBehavior::Hold);
    bus::RenderFrame rf;
    auth.buildRenderFrame(rf, scene);

    ASSERT_EQ(rf.screens.size(), 1u);
    // Hold keeps the last evaluated value (KeyframeTrack::evaluate clamps to
    // last keyframe value when frame >= last frame).
    EXPECT_NEAR(rf.screens[0].position[0], 9.0f, 0.001f);
}

// Before the layer's start frame there's no "last evaluated value" to hold,
// so neither Hold nor Reset should fold anything into the screen.
TEST_F(OAShowSideReevalTest, EndBehavior_BeforeStart_AlwaysNoOverride) {
    timeline.seekToFrame(0);  // before layerStart=50
    auto scene = makeOASnapshot(kScreenId, /*layerStart*/50, /*duration*/100,
                                /*startVal*/3.0f, /*endVal*/9.0f,
                                AnimatableProperty::PositionX,
                                bus::OAEndBehavior::Hold);
    bus::RenderFrame rf;
    auth.buildRenderFrame(rf, scene);

    ASSERT_EQ(rf.screens.size(), 1u);
    EXPECT_NEAR(rf.screens[0].position[0], 0.0f, 0.001f);
}

// Rotation (maps to Y-axis yaw for OA, per Option A convention) and PositionZ
// exercise different switch branches in the re-eval path.
TEST_F(OAShowSideReevalTest, RotationY_Track_FoldsIntoScreenRotation) {
    timeline.seekToFrame(0);
    auto scene = makeOASnapshot(kScreenId, 0, 100, /*startVal*/45.0f, 90.0f,
                                AnimatableProperty::Rotation);
    bus::RenderFrame rf;
    auth.buildRenderFrame(rf, scene);

    ASSERT_EQ(rf.screens.size(), 1u);
    EXPECT_NEAR(rf.screens[0].rotation[1], 45.0f, 0.01f);  // Y-axis
}

TEST_F(OAShowSideReevalTest, PositionZ_Track_FoldsIntoScreenPositionZ) {
    timeline.seekToFrame(0);
    auto scene = makeOASnapshot(kScreenId, 0, 100, /*startVal*/7.0f, 14.0f,
                                AnimatableProperty::PositionZ);
    bus::RenderFrame rf;
    auth.buildRenderFrame(rf, scene);

    ASSERT_EQ(rf.screens.size(), 1u);
    EXPECT_NEAR(rf.screens[0].position[2], 7.0f, 0.01f);
}

// ADR-0020: RotationZ is the new dedicated Z-axis (roll) channel for OA layers.
// Legacy `Rotation` continues to map to Y on OA (yaw), but RotationZ goes
// where it should — index [2] of the rotation array.
TEST_F(OAShowSideReevalTest, RotationZ_Track_FoldsIntoScreenRotationZ) {
    timeline.seekToFrame(0);
    auto scene = makeOASnapshot(kScreenId, 0, 100, /*startVal*/30.0f, 60.0f,
                                AnimatableProperty::RotationZ);
    bus::RenderFrame rf;
    auth.buildRenderFrame(rf, scene);

    ASSERT_EQ(rf.screens.size(), 1u);
    EXPECT_NEAR(rf.screens[0].rotation[2], 30.0f, 0.01f);
    // Sanity: the legacy yaw channel must remain untouched by a RotationZ
    // track (it's a separate axis), so screen base rotation[1] stays at 0.
    EXPECT_NEAR(rf.screens[0].rotation[1], 0.0f, 0.001f);
}

// Layer with a non-zero startFrame: currentFrame=50, layerStart=30,
// localFrame=20. Two keyframes at (0, 0.0) and (39, 4.0).
// At localFrame=20 (halfway through 40 frames): expected = 2.0.
TEST_F(OAShowSideReevalTest, NonZeroLayerStart_LocalFrameComputedCorrectly) {
    timeline.seekToFrame(50);  // localFrame = 50 - 30 = 20
    bus::SceneSnapshot scene;

    bus::ScreenSnapshot ss;
    ss.entity   = kScreenId;
    ss.position = {0.0f, 0.0f, 0.0f};
    scene.screens.push_back(ss);

    bus::BakedKeyframe k0; k0.frame = 0;  k0.value = 0.0f;
    k0.interpolation = static_cast<int>(InterpolationType::Linear);
    bus::BakedKeyframe k1; k1.frame = 39; k1.value = 4.0f;
    k1.interpolation = static_cast<int>(InterpolationType::Linear);

    bus::BakedTrack track;
    track.property  = static_cast<int>(AnimatableProperty::PositionX);
    track.enabled   = true;
    track.keyframes = {k0, k1};

    bus::ObjectAnimationLayerSnapshot oas;
    oas.targetScreen = kScreenId;
    oas.startFrame   = 30;
    oas.duration     = 40;
    oas.trackIndex   = 0;
    oas.tracks.push_back(std::move(track));
    scene.objectAnimationLayers.push_back(std::move(oas));

    bus::RenderFrame rf;
    auth.buildRenderFrame(rf, scene);

    ASSERT_EQ(rf.screens.size(), 1u);
    // localFrame=20, t=20/39 ≈ 0.513, value = 4.0 * 0.513 ≈ 2.05
    EXPECT_NEAR(rf.screens[0].position[0], 2.05f, 0.1f);
}
