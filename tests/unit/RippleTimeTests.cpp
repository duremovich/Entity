#include <gtest/gtest.h>
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <entt/entt.hpp>

using namespace entity;

namespace {
// Set up a Timeline with a single track. Clips are bare-data only (no FFmpeg
// contexts) which is fine — the ripple helpers only touch placement /
// AnimatedProperties / mediaStartFrame. Layer-only entities (Signal here)
// exercise the archetype-generic placement path.
class RippleTimeTest : public ::testing::Test {
protected:
    entt::registry registry;
    std::unique_ptr<Timeline> timeline;
    entt::entity track{entt::null};

    void SetUp() override {
        timeline = std::make_unique<Timeline>(registry);
        track = timeline->createTrack("Track 1");
    }

    entt::entity addClip(FrameNumber start, FrameNumber duration,
                         FrameNumber mediaStart = 0) {
        entt::entity e = registry.create();
        auto& clip = registry.emplace<Clip>(e);
        clip.startFrame = start;
        clip.duration = duration;
        clip.mediaStartFrame = mediaStart;
        clip.framerate = 30.0;
        auto& lay = registry.emplace<Layer>(e);
        lay.kind       = Layer::Kind::Clip;
        lay.startFrame = start;
        lay.duration   = duration;
        registry.get<TimelineTrack>(track).layers.push_back(e);
        return e;
    }

    // A Layer-only entity (Signal archetype: Layer + SignalLayer, no Clip).
    // Cannot be snapshot-deleted, so it drives the delete pre-flight abort.
    entt::entity addSignalLayer(FrameNumber start, FrameNumber duration) {
        entt::entity e = registry.create();
        auto& lay = registry.emplace<Layer>(e);
        lay.kind       = Layer::Kind::Signal;
        lay.startFrame = start;
        lay.duration   = duration;
        registry.emplace<SignalLayer>(e);
        registry.get<TimelineTrack>(track).layers.push_back(e);
        return e;
    }

    const Clip& clipOf(entt::entity e) const { return registry.get<Clip>(e); }
    const Layer& layerOf(entt::entity e) const { return registry.get<Layer>(e); }
};
}

// ============================================================================
// INSERT
// ============================================================================

// --- Insert: pure shift (gap between clips) ---------------------------------

TEST_F(RippleTimeTest, InsertAtGap_ShiftsLaterClips) {
    auto a = addClip(0, 100);
    auto b = addClip(200, 100);  // gap is [100, 200)
    auto c = addClip(400, 50);

    auto rec = timeline->rippleInsertTime(150, 30);
    ASSERT_TRUE(rec.success);
    EXPECT_EQ(rec.lengthened.size(), 0u);
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
    EXPECT_EQ(rec.lengthened.size(), 0u);
}

// --- Insert: spanning case now LENGTHENS (no split) -------------------------

TEST_F(RippleTimeTest, InsertInsideClip_LengthensSpanningClip) {
    auto a = addClip(0, 100);     // spans [0, 100)
    auto b = addClip(150, 50);    // entirely after — will shift

    auto rec = timeline->rippleInsertTime(40, 25);
    ASSERT_TRUE(rec.success);
    ASSERT_EQ(rec.lengthened.size(), 1u);
    EXPECT_EQ(rec.lengthened[0].entity, a);

    // 'a' lengthened in place — start unchanged, duration += 25. No split entity.
    EXPECT_EQ(clipOf(a).startFrame, 0);
    EXPECT_EQ(clipOf(a).duration, 125);

    // 'b' shifted.
    EXPECT_EQ(clipOf(b).startFrame, 175);   // 150 + 25
}

TEST_F(RippleTimeTest, InsertInsideClip_UndoRestoresDuration) {
    auto a = addClip(0, 100);
    auto b = addClip(150, 50);

    auto rec = timeline->rippleInsertTime(40, 25);
    ASSERT_TRUE(rec.success);
    timeline->undoRippleInsertTime(rec);

    EXPECT_EQ(clipOf(a).startFrame, 0);
    EXPECT_EQ(clipOf(a).duration, 100);
    EXPECT_EQ(clipOf(b).startFrame, 150);
}

// --- Insert: keyframe shift inside a lengthened clip ------------------------

TEST_F(RippleTimeTest, InsertInsideClip_ShiftsKeyframesAfterPivot) {
    auto a = addClip(0, 100);     // clip-relative pivot = insertFrame - start = 40
    {
        auto& ap = registry.emplace<AnimatedProperties>(a);
        auto& tr = ap.getOrCreateTrack(AnimatableProperty::Opacity);
        tr.addKeyframe(10, 0.25f);   // before pivot 40 — stays
        tr.addKeyframe(60, 0.75f);   // after pivot 40 — shifts +25
    }

    auto rec = timeline->rippleInsertTime(40, 25);
    ASSERT_TRUE(rec.success);
    ASSERT_EQ(rec.keyframes.size(), 1u);

    auto* tr = registry.get<AnimatedProperties>(a).getTrack(AnimatableProperty::Opacity);
    ASSERT_NE(tr, nullptr);
    ASSERT_EQ(tr->keyframes.size(), 2u);
    EXPECT_EQ(tr->keyframes[0].frame, 10);   // unmoved
    EXPECT_EQ(tr->keyframes[1].frame, 85);   // 60 + 25
}

TEST_F(RippleTimeTest, InsertInsideClip_UndoRestoresKeyframes) {
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

    auto* tr = registry.get<AnimatedProperties>(a).getTrack(AnimatableProperty::Opacity);
    ASSERT_NE(tr, nullptr);
    ASSERT_EQ(tr->keyframes.size(), 2u);
    EXPECT_EQ(tr->keyframes[0].frame, 10);
    EXPECT_FLOAT_EQ(tr->keyframes[0].value, 0.25f);
    EXPECT_EQ(tr->keyframes[1].frame, 60);
    EXPECT_FLOAT_EQ(tr->keyframes[1].value, 0.75f);
}

// --- Insert: layer-only (Signal) entity participates ------------------------

TEST_F(RippleTimeTest, InsertShiftsLayerOnlyEntities) {
    auto sig = addSignalLayer(300, 50);   // entirely after insert point
    auto rec = timeline->rippleInsertTime(100, 40);
    ASSERT_TRUE(rec.success);
    EXPECT_EQ(layerOf(sig).startFrame, 340);   // 300 + 40

    timeline->undoRippleInsertTime(rec);
    EXPECT_EQ(layerOf(sig).startFrame, 300);
}

// --- Insert: cue + section shift --------------------------------------------

TEST_F(RippleTimeTest, InsertShiftsCuesAndSections) {
    timeline->addCueTag(CueTag{1.0, 50, "before"});
    timeline->addCueTag(CueTag{2.0, 250, "after"});
    timeline->addSectionBreak(60, 0xFF000000u, 0.0);
    timeline->addSectionBreak(300, 0xFF000000u, 0.0);

    auto rec = timeline->rippleInsertTime(100, 40);
    ASSERT_TRUE(rec.success);

    // Cue before insert untouched; cue after shifts +40.
    EXPECT_EQ(timeline->findCueTag(1.0)->frame, 50);
    EXPECT_EQ(timeline->findCueTag(2.0)->frame, 290);

    // Section at 60 untouched; section at 300 shifts to 340.
    const auto& secs = timeline->getSections();
    ASSERT_EQ(secs.size(), 2u);
    EXPECT_EQ(secs[0].breakFrame, 60);
    EXPECT_EQ(secs[1].breakFrame, 340);

    timeline->undoRippleInsertTime(rec);
    EXPECT_EQ(timeline->findCueTag(2.0)->frame, 250);
    EXPECT_EQ(timeline->getSections()[1].breakFrame, 300);
}

// ============================================================================
// DELETE
// ============================================================================

// --- Delete: clean shift case (range in a gap) ------------------------------

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

// --- Delete: fully-inside entity is snapshot-deleted ------------------------

TEST_F(RippleTimeTest, DeleteFullyInside_RemovesEntity) {
    auto a = addClip(60, 30);   // entirely inside [50, 100)
    auto rec = timeline->rippleDeleteTime(50, 100);
    ASSERT_TRUE(rec.success);
    ASSERT_EQ(rec.deleted.size(), 1u);

    // Entity destroyed and removed from the track.
    EXPECT_FALSE(registry.valid(a));
    EXPECT_TRUE(registry.get<TimelineTrack>(track).layers.empty());

    // Undo re-creates it (fresh entity id) at the original placement.
    timeline->undoRippleDeleteTime(rec);
    const auto& layers = registry.get<TimelineTrack>(track).layers;
    ASSERT_EQ(layers.size(), 1u);
    EXPECT_EQ(clipOf(layers[0]).startFrame, 60);
    EXPECT_EQ(clipOf(layers[0]).duration, 30);
}

// --- Delete: straddles range start (tail trimmed) ---------------------------

TEST_F(RippleTimeTest, DeleteStraddlesStart_TrimsTail) {
    auto a = addClip(0, 100);   // [0, 100); range [60, 200) cuts the tail
    auto rec = timeline->rippleDeleteTime(60, 200);
    ASSERT_TRUE(rec.success);
    ASSERT_EQ(rec.resized.size(), 1u);

    EXPECT_EQ(clipOf(a).startFrame, 0);
    EXPECT_EQ(clipOf(a).duration, 60);   // rangeStart - start

    timeline->undoRippleDeleteTime(rec);
    EXPECT_EQ(clipOf(a).duration, 100);
}

// --- Delete: straddles range end (head trimmed + media slip) ----------------

TEST_F(RippleTimeTest, DeleteStraddlesEnd_TrimsHeadAndSlipsMedia) {
    auto a = addClip(100, 100, /*mediaStart=*/10);  // [100, 200), in-point 10
    // range [50, 150): removes the head [100,150). Survivor: 50 frames of tail.
    auto rec = timeline->rippleDeleteTime(50, 150);
    ASSERT_TRUE(rec.success);
    ASSERT_EQ(rec.resized.size(), 1u);

    EXPECT_EQ(clipOf(a).startFrame, 50);    // collapses to rangeStart
    EXPECT_EQ(clipOf(a).duration, 50);      // oldEnd - rangeEnd = 200 - 150
    // mediaStart slips by (rangeEnd - oldStart) = 150 - 100 = 50 → 10 + 50 = 60.
    EXPECT_EQ(clipOf(a).mediaStartFrame, 60);

    timeline->undoRippleDeleteTime(rec);
    EXPECT_EQ(clipOf(a).startFrame, 100);
    EXPECT_EQ(clipOf(a).duration, 100);
    EXPECT_EQ(clipOf(a).mediaStartFrame, 10);
}

// --- Delete: spans the whole range (shortened + keyframe window removed) -----

TEST_F(RippleTimeTest, DeleteSpansRange_ShortensAndDropsKeyframeWindow) {
    auto a = addClip(0, 200);   // [0, 200)
    {
        auto& ap = registry.emplace<AnimatedProperties>(a);
        auto& tr = ap.getOrCreateTrack(AnimatableProperty::Opacity);
        tr.addKeyframe(20, 0.1f);    // before deleted window — stays
        tr.addKeyframe(80, 0.5f);    // inside [60,120) clip-rel — removed
        tr.addKeyframe(160, 0.9f);   // after window — shifts -60
    }
    // range [60, 120): removeDur 60. Clip survives, shortened to 140.
    auto rec = timeline->rippleDeleteTime(60, 120);
    ASSERT_TRUE(rec.success);
    ASSERT_EQ(rec.resized.size(), 1u);
    ASSERT_EQ(rec.keyframes.size(), 1u);

    EXPECT_EQ(clipOf(a).startFrame, 0);
    EXPECT_EQ(clipOf(a).duration, 140);   // 200 - 60

    auto* tr = registry.get<AnimatedProperties>(a).getTrack(AnimatableProperty::Opacity);
    ASSERT_NE(tr, nullptr);
    ASSERT_EQ(tr->keyframes.size(), 2u);
    EXPECT_EQ(tr->keyframes[0].frame, 20);    // unmoved
    EXPECT_EQ(tr->keyframes[1].frame, 100);   // 160 - 60

    timeline->undoRippleDeleteTime(rec);
    EXPECT_EQ(clipOf(a).duration, 200);
    auto* tr2 = registry.get<AnimatedProperties>(a).getTrack(AnimatableProperty::Opacity);
    ASSERT_EQ(tr2->keyframes.size(), 3u);
    EXPECT_EQ(tr2->keyframes[0].frame, 20);
    EXPECT_EQ(tr2->keyframes[1].frame, 80);
    EXPECT_EQ(tr2->keyframes[2].frame, 160);
}

// --- Delete: cue + section shift / removal ----------------------------------

TEST_F(RippleTimeTest, DeleteShiftsAndRemovesCuesAndSections) {
    timeline->addCueTag(CueTag{1.0, 30,  "before"});  // before range — stays
    timeline->addCueTag(CueTag{2.0, 120, "inside"});  // inside — removed
    timeline->addCueTag(CueTag{3.0, 300, "after"});   // after — shifts -100
    timeline->addSectionBreak(40,  0xFF000000u, 0.0);  // before — stays
    timeline->addSectionBreak(150, 0xFF000000u, 0.0);  // inside [100,200) — removed
    timeline->addSectionBreak(350, 0xFF000000u, 0.0);  // after — shifts -100

    auto rec = timeline->rippleDeleteTime(100, 200);   // removeDur 100
    ASSERT_TRUE(rec.success);

    EXPECT_EQ(timeline->findCueTag(1.0)->frame, 30);
    EXPECT_EQ(timeline->findCueTag(2.0), nullptr);     // removed
    EXPECT_EQ(timeline->findCueTag(3.0)->frame, 200);  // 300 - 100

    const auto& secs = timeline->getSections();
    ASSERT_EQ(secs.size(), 2u);
    EXPECT_EQ(secs[0].breakFrame, 40);
    EXPECT_EQ(secs[1].breakFrame, 250);                // 350 - 100

    timeline->undoRippleDeleteTime(rec);
    EXPECT_EQ(timeline->findCueTag(2.0)->frame, 120);  // restored
    EXPECT_EQ(timeline->findCueTag(3.0)->frame, 300);
    const auto& secs2 = timeline->getSections();
    ASSERT_EQ(secs2.size(), 3u);
    EXPECT_EQ(secs2[0].breakFrame, 40);
    EXPECT_EQ(secs2[1].breakFrame, 150);
    EXPECT_EQ(secs2[2].breakFrame, 350);
}

// --- Delete: aborts when a fully-contained entity can't be snapshotted ------

TEST_F(RippleTimeTest, DeleteAbortsOnContainedSignalLayer) {
    auto clip = addClip(300, 50);              // after the range; would shift
    auto sig  = addSignalLayer(60, 30);        // fully inside [50, 100) — can't snapshot

    auto rec = timeline->rippleDeleteTime(50, 100);
    EXPECT_FALSE(rec.success);

    // Nothing mutated — the abort happens before any writes.
    EXPECT_EQ(clipOf(clip).startFrame, 300);
    EXPECT_EQ(layerOf(sig).startFrame, 60);
    EXPECT_TRUE(registry.valid(sig));
}
