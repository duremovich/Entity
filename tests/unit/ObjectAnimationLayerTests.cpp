#include <gtest/gtest.h>
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/ObjectAnimationOutput.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/systems/AnimationSystem.hpp"
#include <entt/entt.hpp>

using namespace entity;

namespace {

class OALayerTest : public ::testing::Test {
protected:
    entt::registry registry;
    std::unique_ptr<Timeline> timeline;
    std::unique_ptr<AnimationSystem> animSystem;
    entt::entity track{entt::null};

    void SetUp() override {
        timeline   = std::make_unique<Timeline>(registry);
        animSystem = std::make_unique<AnimationSystem>();
        animSystem->setTimeline(timeline.get());
        animSystem->initialize(registry);
        track = timeline->createTrack("Track 1");
    }

    // Create a minimal OA layer entity (no Clip, no Screen target needed for
    // AnimationSystem unit tests — we assert ObjectAnimationOutput directly).
    entt::entity addOALayer(FrameNumber start, FrameNumber duration) {
        entt::entity e = registry.create();

        auto& lay = registry.emplace<Layer>(e);
        lay.kind       = Layer::Kind::ObjectAnimation;
        lay.startFrame = start;
        lay.duration   = duration;
        lay.trackIndex = 0;

        auto& oal = registry.emplace<ObjectAnimationLayer>(e);
        oal.target          = entt::null;
        oal.sectionBehavior = SectionBehavior::Normal;

        registry.emplace<AnimatedProperties>(e);

        registry.get<TimelineTrack>(track).layers.push_back(e);
        return e;
    }
};

} // namespace

// --- OA layer carries correct kind -------------------------------------------

TEST_F(OALayerTest, LayerKindIsObjectAnimation) {
    auto e = addOALayer(0, 60);
    const auto& lay = registry.get<Layer>(e);
    EXPECT_EQ(lay.kind, Layer::Kind::ObjectAnimation);
}

// --- AnimationSystem writes ObjectAnimationOutput when active ----------------

TEST_F(OALayerTest, AnimSystemWritesOutputWhenActive) {
    auto e = addOALayer(0, 60);

    auto& ap = registry.get<AnimatedProperties>(e);
    ap.addKeyframe(AnimatableProperty::PositionX, 0,  0.0f);
    ap.addKeyframe(AnimatableProperty::PositionX, 30, 1.0f);

    // Seek to frame 15 (halfway between kf0 and kf30 → value=0.5)
    timeline->seekToFrame(15);
    animSystem->update(registry, 0.0f);

    const auto* out = registry.try_get<ObjectAnimationOutput>(e);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(out->hasPosition);
    EXPECT_NEAR(out->positionOverride[0], 0.5f, 0.01f);
}

// --- AnimationSystem does NOT write output when OA layer is inactive ---------

TEST_F(OALayerTest, AnimSystemSkipsOutputWhenInactive) {
    auto e = addOALayer(100, 60);  // active only at frames [100, 160)

    auto& ap = registry.get<AnimatedProperties>(e);
    ap.addKeyframe(AnimatableProperty::PositionX, 0, 5.0f);

    // Playhead at frame 0 — OA layer not yet active
    timeline->seekToFrame(0);
    animSystem->update(registry, 0.0f);

    // No ObjectAnimationOutput should be written (or if it exists from a prior
    // tick, hasPosition must be false because the entity was just created and
    // the system skips inactive layers before calling get_or_emplace).
    const auto* out = registry.try_get<ObjectAnimationOutput>(e);
    // Either not created, or created but flags clear (reset at top of OA loop)
    if (out) {
        EXPECT_FALSE(out->hasPosition);
    }
}

// --- 3D axis coverage --------------------------------------------------------

TEST_F(OALayerTest, PositionZEvaluatedIntoOutput) {
    auto e = addOALayer(0, 60);
    auto& ap = registry.get<AnimatedProperties>(e);
    ap.addKeyframe(AnimatableProperty::PositionZ, 0, 3.0f);

    timeline->seekToFrame(0);
    animSystem->update(registry, 0.0f);

    const auto* out = registry.try_get<ObjectAnimationOutput>(e);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(out->hasPosition);
    EXPECT_NEAR(out->positionOverride[2], 3.0f, 0.01f);
}

TEST_F(OALayerTest, RotationXEvaluatedIntoOutput) {
    auto e = addOALayer(0, 60);
    auto& ap = registry.get<AnimatedProperties>(e);
    ap.addKeyframe(AnimatableProperty::RotationX, 0, 45.0f);

    timeline->seekToFrame(0);
    animSystem->update(registry, 0.0f);

    const auto* out = registry.try_get<ObjectAnimationOutput>(e);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(out->hasRotation);
    EXPECT_NEAR(out->rotationOverride[0], 45.0f, 0.01f);
}

TEST_F(OALayerTest, ScaleZEvaluatedIntoOutput) {
    auto e = addOALayer(0, 60);
    auto& ap = registry.get<AnimatedProperties>(e);
    ap.addKeyframe(AnimatableProperty::ScaleZ, 0, 2.0f);

    timeline->seekToFrame(0);
    animSystem->update(registry, 0.0f);

    const auto* out = registry.try_get<ObjectAnimationOutput>(e);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(out->hasSize);
    EXPECT_NEAR(out->sizeOverride[2], 2.0f, 0.01f);
}

// --- Output reset each tick --------------------------------------------------

TEST_F(OALayerTest, OutputResetEachTick) {
    auto e = addOALayer(0, 60);
    auto& ap = registry.get<AnimatedProperties>(e);
    ap.addKeyframe(AnimatableProperty::PositionX, 0, 1.0f);

    // First tick — output written
    timeline->seekToFrame(0);
    animSystem->update(registry, 0.0f);
    {
        const auto* out = registry.try_get<ObjectAnimationOutput>(e);
        ASSERT_NE(out, nullptr);
        EXPECT_TRUE(out->hasPosition);
    }

    // Remove keyframes so no tracks are active on the next tick
    ap.clearAll();
    animSystem->update(registry, 0.0f);

    // The entity has AnimatedProperties but no keyframes — the outer loop
    // skips it via hasAnyKeyframes(). The OA branch iterates the oaView and
    // also finds no keyframes, so it resets the output and leaves flags false.
    // Actually: hasAnyKeyframes() guards the outer *clip* loop only. The OA
    // branch checks per-track hasKeyframes(). Flags must be cleared.
    const auto* out = registry.try_get<ObjectAnimationOutput>(e);
    if (out) {
        EXPECT_FALSE(out->hasPosition);
        EXPECT_FALSE(out->hasRotation);
        EXPECT_FALSE(out->hasSize);
    }
}

// --- Entities with only Clip (no ObjectAnimationLayer) are not affected ------

TEST_F(OALayerTest, ClipEntityNotTouchedByOABranch) {
    // Create a bare clip entity with no ObjectAnimationLayer
    entt::entity clipEntity = registry.create();
    auto& lay = registry.emplace<Layer>(clipEntity);
    lay.kind       = Layer::Kind::Clip;
    lay.startFrame = 0;
    lay.duration   = 60;
    registry.emplace<AnimatedProperties>(clipEntity);

    timeline->seekToFrame(0);
    animSystem->update(registry, 0.0f);

    // No ObjectAnimationOutput should be created on a Clip entity
    EXPECT_EQ(registry.try_get<ObjectAnimationOutput>(clipEntity), nullptr);
}
