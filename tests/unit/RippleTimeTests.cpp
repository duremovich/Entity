#include <gtest/gtest.h>
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <entt/entt.hpp>

using namespace entity;

namespace {
// Set up a Timeline with a single track holding clips at specified
// (startFrame, duration) positions. Clips are bare-data only (no FFmpeg
// contexts) which is fine — the ripple helpers only touch Clip.startFrame /
// duration / AnimatedProperties.
class RippleTimeTest : public ::testing::Test {
protected:
    entt::registry registry;
    std::unique_ptr<Timeline> timeline;
    entt::entity track{entt::null};

    void SetUp() override {
        timeline = std::make_unique<Timeline>(registry);
        track = timeline->createTrack("Track 1");
    }

    entt::entity addClip(FrameNumber start, FrameNumber duration) {
        entt::entity e = registry.create();
        auto& clip = registry.emplace<Clip>(e);
        clip.startFrame = start;
        clip.duration = duration;
        clip.framerate = 30.0;
        auto& lay = registry.emplace<Layer>(e);
        lay.kind       = Layer::Kind::Clip;
        lay.startFrame = start;
        lay.duration   = duration;
        registry.get<TimelineTrack>(track).layers.push_back(e);
        return e;
    }

    const Clip& clipOf(entt::entity e) const { return registry.get<Clip>(e); }
};
}

// --- Insert: pure shift (gap between clips) ----------------------------------

TEST_F(RippleTimeTest, InsertAtGap_ShiftsLaterClips) {
    auto a = addClip(0, 100);
    auto b = addClip(200, 100);  // gap is [100, 200)
    auto c = addClip(400, 50);

    auto rec = timeline->rippleInsertTime(150, 30);
    ASSERT_TRUE(rec.success);
    EXPECT_EQ(rec.splits.size(), 0u);
    EXPECT_EQ(rec.shifted.size(), 2u);

    EXPECT_EQ(clipOf(a).startFrame, 0);   // before insert: untouched
    EXPECT_EQ(clipOf(b).startFrame, 230);
    EXPECT_EQ(clipOf(c).startFrame, 430);
}

TEST_F(RippleTimeTest, InsertAtGap_UndoRestores) {
    auto a = addClip(0, 100);
    auto b = addClip(200, 100);
    auto rec = timeline->rippleInsertTime(150, 30);
    ASSERT_TRUE(rec.success);

    timeline->undoRippleInsertTime(rec);
    EXPECT_EQ(clipOf(a).startFrame, 0);
    EXPECT_EQ(clipOf(b).startFrame, 200);
    EXPECT_EQ(rec.shifted.size(), 0u);
    EXPECT_EQ(rec.splits.size(), 0u);
}

// --- Insert: split case (insert lands inside a clip) ------------------------

TEST_F(RippleTimeTest, InsertInsideClip_SplitsAndShiftsRightHalf) {
    auto a = addClip(0, 100);     // spans [0, 100)
    auto b = addClip(150, 50);    // entirely after — will shift

    auto rec = timeline->rippleInsertTime(40, 25);
    ASSERT_TRUE(rec.success);
    ASSERT_EQ(rec.splits.size(), 1u);
    EXPECT_EQ(rec.splits[0].originalEntity, a);

    // Original 'a' truncated to left half [0, 40)
    EXPECT_EQ(clipOf(a).startFrame, 0);
    EXPECT_EQ(clipOf(a).duration, 40);

    // Right half got created, shifted by +25.
    entt::entity rightHalf = rec.splits[0].newRightEntity;
    // Compare via registry.valid() to avoid gtest pretty-printing entt::null
    // (which forces an entt_traits<__int64> instantiation that doesn't exist).
    ASSERT_TRUE(registry.valid(rightHalf));
    EXPECT_EQ(clipOf(rightHalf).startFrame, 65);   // 40 + 25
    EXPECT_EQ(clipOf(rightHalf).duration, 60);     // 100 - 40

    // 'b' shifted too.
    EXPECT_EQ(clipOf(b).startFrame, 175);          // 150 + 25
}

TEST_F(RippleTimeTest, InsertInsideClip_UndoMergesBack) {
    auto a = addClip(0, 100);
    auto b = addClip(150, 50);

    auto rec = timeline->rippleInsertTime(40, 25);
    ASSERT_TRUE(rec.success);
    entt::entity rightHalf = rec.splits[0].newRightEntity;
    ASSERT_TRUE(registry.valid(rightHalf));

    timeline->undoRippleInsertTime(rec);

    // Right half destroyed, original duration restored, b back in place.
    EXPECT_FALSE(registry.valid(rightHalf));
    EXPECT_EQ(clipOf(a).startFrame, 0);
    EXPECT_EQ(clipOf(a).duration, 100);
    EXPECT_EQ(clipOf(b).startFrame, 150);
}

TEST_F(RippleTimeTest, InsertInsideClip_PreservesAnimatedPropertiesOnUndo) {
    auto a = addClip(0, 100);
    {
        auto& ap = registry.emplace<AnimatedProperties>(a);
        auto& tr = ap.getOrCreateTrack(AnimatableProperty::Opacity);
        tr.addKeyframe(10, 0.25f);
        tr.addKeyframe(60, 0.75f);
    }

    auto rec = timeline->rippleInsertTime(40, 25);
    ASSERT_TRUE(rec.success);
    timeline->undoRippleInsertTime(rec);

    auto* apAfter = registry.try_get<AnimatedProperties>(a);
    ASSERT_NE(apAfter, nullptr);
    auto* tr = apAfter->getTrack(AnimatableProperty::Opacity);
    ASSERT_NE(tr, nullptr);
    ASSERT_EQ(tr->keyframes.size(), 2u);
    EXPECT_EQ(tr->keyframes[0].frame, 10);
    EXPECT_FLOAT_EQ(tr->keyframes[0].value, 0.25f);
    EXPECT_EQ(tr->keyframes[1].frame, 60);
    EXPECT_FLOAT_EQ(tr->keyframes[1].value, 0.75f);
}

// --- Delete: clean shift case -----------------------------------------------

TEST_F(RippleTimeTest, DeleteCleanRange_ShiftsLaterClipsLeft) {
    auto a = addClip(0, 100);
    auto b = addClip(300, 100);

    auto rec = timeline->rippleDeleteTime(150, 250);  // gap interior
    ASSERT_TRUE(rec.success);
    EXPECT_EQ(rec.shifted.size(), 1u);

    EXPECT_EQ(clipOf(a).startFrame, 0);
    EXPECT_EQ(clipOf(b).startFrame, 200);  // 300 - 100
}

TEST_F(RippleTimeTest, DeleteCleanRange_UndoRestores) {
    auto a = addClip(0, 100);
    auto b = addClip(300, 100);

    auto rec = timeline->rippleDeleteTime(150, 250);
    ASSERT_TRUE(rec.success);
    timeline->undoRippleDeleteTime(rec);

    EXPECT_EQ(clipOf(a).startFrame, 0);
    EXPECT_EQ(clipOf(b).startFrame, 300);
}

// --- Delete: refusal cases (v1 doesn't handle clip-spanning) ----------------

TEST_F(RippleTimeTest, DeleteRefusesWhenClipOverlapsRange) {
    auto a = addClip(0, 100);     // spans 0-99
    auto b = addClip(200, 100);

    // Range [50, 80) overlaps clip a — should refuse.
    auto rec = timeline->rippleDeleteTime(50, 80);
    EXPECT_FALSE(rec.success);

    // Nothing moved.
    EXPECT_EQ(clipOf(a).startFrame, 0);
    EXPECT_EQ(clipOf(a).duration, 100);
    EXPECT_EQ(clipOf(b).startFrame, 200);
}

TEST_F(RippleTimeTest, DeleteRefusesWhenClipEntirelyInsideRange) {
    auto a = addClip(60, 30);     // entirely inside [50, 100)
    auto rec = timeline->rippleDeleteTime(50, 100);
    EXPECT_FALSE(rec.success);
    EXPECT_EQ(clipOf(a).startFrame, 60);
}
