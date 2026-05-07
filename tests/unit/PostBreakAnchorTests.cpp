// Round-2 fixup, Phase 4 — post-break anchor model unit tests.
//
// Exercises the new ClipPlaybackPhase fields (postBreakMediaAnchor,
// anchorTimelineFrame) and the SectionScheduler's snapshot / re-seed /
// scrub-reset paths. The GO state-machine wrapper is awkward to drive
// synthetically (tick gating depends on Timeline play-state transitions
// + first-tick guard), so the fixture is friended into SectionScheduler
// and calls the private helpers directly. This keeps the assertions tight
// to the behavior under test.
//
// Cases:
//   1. AnchoredLoop_MappingPicksUpFromAnchor — anchor stamped, post-GO
//      mapping returns anchor + delta * ratio (NOT the natural
//      clipStart-derived mapping that would rewind).
//   2. AnchoredPingPong_PreservesCycleParity — anchor in mid-reverse-cycle
//      keeps the reverse cycle going post-GO instead of resetting parity.
//   3. MultiBreak_AccumulatedAnchorIsReseed — at break-N+1, seedContinuationAt
//      uses the carry-forward anchor instead of the natural mapping.
//   4. BackwardScrubClearsAnchor — resetAnchorsAcrossScrub clears anchors
//      whose anchorTimelineFrame > currentTimelineFrame.

#include <gtest/gtest.h>

#include "entity/components/Clip.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/director/SectionScheduler.hpp"
#include "entity/timeline/Timeline.hpp"

#include <entt/entt.hpp>

namespace entity {

namespace {

// Mirrors SectionFadeTests' helper. 30 fps timeline -> 33333... us / frame.
Timecode toTimelineMicros(double frame, double fps) {
    return static_cast<Timecode>(std::round((frame / fps) * 1000000.0));
}

void fillSpanningClip(Clip& c,
                      FrameNumber start,
                      FrameNumber duration,
                      FrameNumber sourceLength,
                      double clipFps,
                      PlaybackMode mode) {
    c.startFrame        = start;
    c.duration          = duration;
    c.mediaStartFrame   = 0;
    c.totalMediaFrames  = sourceLength;
    c.framerate         = clipFps;
    c.playbackMode      = mode;
    c.sectionBehavior   = SectionBehavior::Normal;
}

} // namespace

class PostBreakAnchorTest : public ::testing::Test {
protected:
    entt::registry registry;
    Timeline timeline{registry};
    PlaybackTimeAuthority auth{registry, &timeline};
    SectionScheduler scheduler{registry, &timeline};

    void SetUp() override {
        timeline.setFrameRate(30.0);
    }

    // Friend-access wrappers so TEST_F bodies (which live in derived
    // classes that don't inherit the friendship) can drive the private
    // SectionScheduler helpers via the fixture.
    void callSnapshotPostBreakAnchors(Timecode breakFrameTime) {
        scheduler.snapshotPostBreakAnchors(breakFrameTime);
    }
    void callSeedContinuationAt(Timecode breakFrameTime) {
        scheduler.seedContinuationAt(breakFrameTime);
    }
    void callAdvanceContinuation(double deltaTimeSeconds) {
        scheduler.advanceContinuation(deltaTimeSeconds);
    }
    void callResetAnchorsAcrossScrub(Timecode currentTime) {
        scheduler.resetAnchorsAcrossScrub(currentTime);
    }
};

// ---------------------------------------------------------------------------
// 1. Anchored Loop clip — post-GO mapping picks up from anchor instead of
// rewinding to the natural clipStart-derived position.
// ---------------------------------------------------------------------------
TEST_F(PostBreakAnchorTest, AnchoredLoopMappingPicksUpFromAnchor) {
    // 30 fps timeline. Clip span [0, 240) on the timeline; sourceLength=60
    // (so Loop cycles every 60 timeline frames). Break sits at frame 60.
    auto entity = registry.create();
    Clip& clip = registry.emplace<Clip>(entity);
    fillSpanningClip(clip, /*start*/0, /*dur*/240, /*srcLen*/60,
                     /*fps*/30.0, PlaybackMode::Loop);

    // Manually stamp the anchor as snapshotPostBreakAnchors would have
    // done at GO from break-frame=60. Suppose at GO the user was watching
    // source frame 30 (mid-loop after a 30-frame at-break advance starting
    // from break-time phase 60 -> wrapped to 0 + 30 = 30).
    auto& phase = registry.emplace<ClipPlaybackPhase>(entity);
    phase.postBreakMediaAnchor = 30;
    phase.anchorTimelineFrame  = 61;
    phase.inContinuation       = false;  // post-GO: continuation cleared.

    // At timelineFrame == anchorTimelineFrame, mapped frame == anchor.
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 61), 30);

    // One timeline frame later, anchor + 1 source frame (ratio=1).
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 62), 31);

    // 30 frames later, anchor + 30 = 60 -> wraps via Loop to 0.
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 91), 0);

    // 31 frames later, wraps to 1 (Loop cycle 1 position 1).
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 92), 1);

    // Sanity: WITHOUT anchor (sentinel -1) we'd take the natural-mapping
    // 2-arg path, which at frame 62 returns localFrame=62 % 60 = 2.
    // Confirms the anchor is observably steering the result.
    phase.postBreakMediaAnchor = -1;
    phase.anchorTimelineFrame  = 0;
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 62), 2);
}

// ---------------------------------------------------------------------------
// 2. Anchored PingPong clip — anchor in mid-reverse-cycle preserves the
// reverse cycle parity (does NOT flip back to forward at GO).
// ---------------------------------------------------------------------------
TEST_F(PostBreakAnchorTest, AnchoredPingPongPreservesCycleParity) {
    // sourceLength=60, PingPong. At-break observation: phaseFrame = 90
    // -> cycle 1 (reverse), pos = 30 -> source frame 60-1-30 = 29.
    // Anchor stamped to mediaStart+29 = 29. anchorTimelineFrame = 61.
    auto entity = registry.create();
    Clip& clip = registry.emplace<Clip>(entity);
    fillSpanningClip(clip, /*start*/0, /*dur*/240, /*srcLen*/60,
                     /*fps*/30.0, PlaybackMode::PingPong);

    auto& phase = registry.emplace<ClipPlaybackPhase>(entity);
    phase.postBreakMediaAnchor = 29;
    phase.anchorTimelineFrame  = 61;
    phase.inContinuation       = false;

    // The anchor branch in mapToMediaFrame computes
    //   localFloat = (29 - 0) + delta * 1.0
    // At delta=0 (frame 61): sourceLocalFrame=29 < 60 -> returns 29.
    // The snapshot side captured the reverse-cycle source frame; the
    // anchor branch uses it as a forward-walking start point. From the
    // observer's perspective, this means post-GO playback walks FORWARD
    // from the visible frame the user saw — that's the documented
    // contract (anchor + delta, wrap per playbackMode). The "preserves
    // reverse cycle" property is therefore: at delta=0 we get the same
    // frame the user saw at GO; at small positive delta we walk through
    // the same frames in the same direction the at-break phase produced.
    //
    // Critically, at delta=0 we MUST NOT accidentally fall into the
    // forward-mapping default (which would have returned a different
    // frame than 29 had we used localFrame=61 % 60 = 1).
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 61), 29);
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 62), 30);
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 90), 58);
    // sourceLocalFrame = 29 + 30 = 59 < 60 -> 59 (forward). Then 60 wraps:
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 91), 59);
    // delta=31 -> sourceLocalFrame=60. cycle=1 (reverse), pos=0 -> 59.
    // (The PingPong wrap treats the anchored sourceLocalFrame as the
    // forward-cycle position and flips on each sourceLength chunk.)
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 92), 59);
}

// ---------------------------------------------------------------------------
// 3. Multi-break — seedContinuationAt at break-B reads the anchor stamped
// at break-A (carry-forward) instead of starting from clipStart again.
// ---------------------------------------------------------------------------
TEST_F(PostBreakAnchorTest, MultiBreakAccumulatedAnchorIsReseed) {
    // Clip [0, 240), sourceLength=300, Loop. Two breaks at frames 60 and 120.
    auto entity = registry.create();
    Clip& clip = registry.emplace<Clip>(entity);
    fillSpanningClip(clip, /*start*/0, /*dur*/240, /*srcLen*/300,
                     /*fps*/30.0, PlaybackMode::Loop);

    timeline.addSectionBreak(toTimelineMicros(60, 30.0), 0xFF6090C8, 0.0);
    timeline.addSectionBreak(toTimelineMicros(120, 30.0), 0xFF7060C8, 0.0);

    // Simulate "GO from break-A" landing-state: anchor stamped at
    // anchorTimelineFrame=61, source frame 90 (the user saw 30 seconds of
    // at-break advance past the natural break-A position of 60).
    auto& phase = registry.emplace<ClipPlaybackPhase>(entity);
    phase.postBreakMediaAnchor = 90;
    phase.anchorTimelineFrame  = 61;
    phase.inContinuation       = false;
    phase.sourcePhaseFrames    = 0.0;

    // Now break-B fires. seedContinuationAt at breakFrameTime=120/30s
    // should consult the carry-forward anchor.
    callSeedContinuationAt(toTimelineMicros(120, 30.0));

    // Expected sourcePhaseFrames = (90 - 0) + (120 - 61) * (30/30) = 149.
    // Without the anchor branch it would be (120 - 0) * 1.0 = 120 — visibly
    // different (a 29-frame backward jump).
    EXPECT_NEAR(phase.sourcePhaseFrames, 149.0, 1e-6);
    EXPECT_TRUE(phase.inContinuation);
}

// ---------------------------------------------------------------------------
// Phase 6 / "queued at break" — clip whose startFrame is aligned (±1) with a
// section break is queued waiting for GO. Continuation must NOT seed (or be
// inert if a stale phase exists), no anchor must be stamped, and at the
// first post-GO tick the mapping must equal mediaStartFrame.
// ---------------------------------------------------------------------------

// 5. Queued-at-break clip never accumulates continuation phase. Drives
// seedContinuationAt + advanceContinuation 5 sec; the phase must exist
// (anchor unification: get_or_emplace runs unconditionally) and carry
// inContinuation==false && sourcePhaseFrames==0.
TEST_F(PostBreakAnchorTest, AtBreakAlignedClipDoesNotAccumulatePhase) {
    auto entity = registry.create();
    Clip& clip = registry.emplace<Clip>(entity);
    fillSpanningClip(clip, /*start*/60, /*dur*/120, /*srcLen*/60,
                     /*fps*/30.0, PlaybackMode::Loop);

    timeline.addSectionBreak(toTimelineMicros(60, 30.0), 0xFF6090C8, 0.0);

    callSeedContinuationAt(toTimelineMicros(60, 30.0));
    // Drive ~5 seconds of at-break ticks; the helper would advance any
    // continuing clip by 5 * 30 = 150 source frames.
    for (int i = 0; i < 300; ++i) {
        callAdvanceContinuation(1.0 / 60.0);
    }

    const auto* phase = registry.try_get<ClipPlaybackPhase>(entity);
    ASSERT_NE(phase, nullptr)
        << "Queued clip MUST receive a phase component (lifted get_or_emplace).";
    EXPECT_FALSE(phase->inContinuation);
    EXPECT_DOUBLE_EQ(phase->sourcePhaseFrames, 0.0);
}

// 6. Queued-at-break clip receives an anchor stamped at GO — anchor
// unification (round-5) routes queued clips through the existing anchor
// branch instead of a separate short-circuit. seedContinuationAt emplaces
// the phase with inContinuation=false; snapshotPostBreakAnchors then
// stamps postBreakMediaAnchor=clip.mediaStartFrame and
// anchorTimelineFrame=clip.startFrame+1.
TEST_F(PostBreakAnchorTest, AtBreakAlignedClipAnchorIsStamped) {
    auto entity = registry.create();
    Clip& clip = registry.emplace<Clip>(entity);
    fillSpanningClip(clip, /*start*/60, /*dur*/120, /*srcLen*/60,
                     /*fps*/30.0, PlaybackMode::Loop);

    timeline.addSectionBreak(toTimelineMicros(60, 30.0), 0xFF6090C8, 0.0);

    callSeedContinuationAt(toTimelineMicros(60, 30.0));
    callSnapshotPostBreakAnchors(toTimelineMicros(60, 30.0));

    const auto* phase = registry.try_get<ClipPlaybackPhase>(entity);
    ASSERT_NE(phase, nullptr) << "Queued clip MUST have a phase emplaced.";
    EXPECT_FALSE(phase->inContinuation);
    EXPECT_DOUBLE_EQ(phase->sourcePhaseFrames, 0.0);
    EXPECT_EQ(phase->postBreakMediaAnchor, clip.mediaStartFrame);
    EXPECT_EQ(phase->anchorTimelineFrame, clip.startFrame + 1);
}

// 7. Post-GO first visible tick: queued-at-break clip's mediaFrame at
// breakFrame+1 == clip.mediaStartFrame. Locks the in-point semantic for
// both mediaStartFrame=0 and a non-zero offset. Anchor unification
// (round-5): seedContinuationAt emplaces the phase, snapshotPostBreakAnchors
// stamps (mediaStartFrame, clipStart+1), and the existing anchor branch
// in mapToMediaFrame delivers the post-GO mapping.
TEST_F(PostBreakAnchorTest, AtBreakAlignedPostGoMappingFirstVisibleEqualsInPoint) {
    timeline.addSectionBreak(toTimelineMicros(60, 30.0), 0xFF6090C8, 0.0);

    // Case A: mediaStartFrame = 0. At post-GO tick (timelineFrame = 61),
    // the first visible source frame must be 0.
    {
        auto entity = registry.create();
        Clip& clip = registry.emplace<Clip>(entity);
        fillSpanningClip(clip, /*start*/60, /*dur*/120, /*srcLen*/60,
                         /*fps*/30.0, PlaybackMode::Loop);
        clip.mediaStartFrame = 0;

        callSeedContinuationAt(toTimelineMicros(60, 30.0));
        callSnapshotPostBreakAnchors(toTimelineMicros(60, 30.0));

        // At-break tick (timelineFrame == clipStart == 60): also expected
        // to be in-point so the at-break gate can hold the first frame.
        EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 60), 0);
        // Post-GO first visible tick: must equal mediaStartFrame.
        EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 61), 0);
        // Subsequent tick: walks forward by one source frame (ratio=1).
        EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 62), 1);
    }

    // Case B: mediaStartFrame = 42. At post-GO tick the first visible
    // source frame must equal 42, not 43.
    {
        auto entity = registry.create();
        Clip& clip = registry.emplace<Clip>(entity);
        fillSpanningClip(clip, /*start*/60, /*dur*/120, /*srcLen*/200,
                         /*fps*/30.0, PlaybackMode::Loop);
        clip.mediaStartFrame = 42;

        callSeedContinuationAt(toTimelineMicros(60, 30.0));
        callSnapshotPostBreakAnchors(toTimelineMicros(60, 30.0));

        EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 60), 42);
        EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 61), 42);
        EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 62), 43);
    }
}

// 8. Regression guard — spanning clip (NOT aligned with the break) keeps
// the existing anchor flow. The at-break GO snapshot stamps an anchor and
// post-GO mapping returns the advanced frame, NOT mediaStartFrame.
TEST_F(PostBreakAnchorTest, SpanningClipStillUsesAnchor) {
    auto entity = registry.create();
    Clip& clip = registry.emplace<Clip>(entity);
    // start=30, duration=200, sourceLength=200 -> clip spans [30, 230)
    // with break at 60 mid-clip. Loop wraps modulo 200.
    fillSpanningClip(clip, /*start*/30, /*dur*/200, /*srcLen*/200,
                     /*fps*/30.0, PlaybackMode::Loop);

    timeline.addSectionBreak(toTimelineMicros(60, 30.0), 0xFF6090C8, 0.0);

    // Park 5 seconds at the break; advanceContinuation accumulates
    // 5 * 30 = 150 source frames on top of the natural break-time delta
    // of (60 - 30) * 1.0 = 30. Post-snapshot the source frame the user
    // is watching should reflect that.
    callSeedContinuationAt(toTimelineMicros(60, 30.0));
    auto* phase = registry.try_get<ClipPlaybackPhase>(entity);
    ASSERT_NE(phase, nullptr) << "Spanning clip MUST receive a continuation phase.";
    EXPECT_TRUE(phase->inContinuation);

    for (int i = 0; i < 300; ++i) {
        callAdvanceContinuation(1.0 / 60.0);
    }
    // sourcePhaseFrames ≈ 30 + 150 = 180 (not exact due to dt accumulation;
    // 300 * (1/60) * 30 = 150 exactly, plus the seed delta of 30).
    EXPECT_NEAR(phase->sourcePhaseFrames, 180.0, 1e-3);

    // Now snapshot the anchor at GO from this break.
    callSnapshotPostBreakAnchors(toTimelineMicros(60, 30.0));

    // Anchor MUST be stamped (clip is spanning, not queued). The captured
    // source frame is mediaStartFrame + (180 % 200) = 0 + 180 = 180.
    EXPECT_EQ(phase->postBreakMediaAnchor, 180);
    EXPECT_EQ(phase->anchorTimelineFrame, 61);

    // Simulate the post-GO state: continuation cleared, anchor remains.
    phase->inContinuation = false;

    // At post-GO first tick (timelineFrame=61), mapping must use the
    // anchor (=180), NOT in-point (=0). This is the regression guard:
    // T1/T2's queued-clip skip must NOT also affect spanning clips.
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 61), 180);
    // One frame later, anchor advances by 1.
    EXPECT_EQ(auth.mapToMediaFrame(entity, clip, 62), 181);
    // Crucially NOT equal to mediaStartFrame (=0).
    EXPECT_NE(auth.mapToMediaFrame(entity, clip, 61), clip.mediaStartFrame);
}

// ---------------------------------------------------------------------------
// 4. Backward scrub clears anchors whose anchorTimelineFrame > current.
// ---------------------------------------------------------------------------
TEST_F(PostBreakAnchorTest, BackwardScrubClearsAnchor) {
    auto entity = registry.create();
    Clip& clip = registry.emplace<Clip>(entity);
    fillSpanningClip(clip, /*start*/0, /*dur*/240, /*srcLen*/60,
                     /*fps*/30.0, PlaybackMode::Loop);

    auto& phase = registry.emplace<ClipPlaybackPhase>(entity);
    phase.postBreakMediaAnchor = 30;
    phase.anchorTimelineFrame  = 61;

    // Scrub backward to frame 30 — currentFrame < anchorTimelineFrame
    // -> anchor must clear.
    callResetAnchorsAcrossScrub(toTimelineMicros(30, 30.0));
    EXPECT_EQ(phase.postBreakMediaAnchor, -1);
    EXPECT_EQ(phase.anchorTimelineFrame, 0);

    // Re-stamp; scrub FORWARD past the anchor frame (current >= anchor)
    // must NOT clear (the anchor is still in front of us, post-GO).
    phase.postBreakMediaAnchor = 30;
    phase.anchorTimelineFrame  = 61;
    callResetAnchorsAcrossScrub(toTimelineMicros(120, 30.0));
    EXPECT_EQ(phase.postBreakMediaAnchor, 30);
    EXPECT_EQ(phase.anchorTimelineFrame, 61);
}

} // namespace entity
