#include <gtest/gtest.h>

#include "entity/components/Clip.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/timeline/Timeline.hpp"

#include <entt/entt.hpp>

using namespace entity;

namespace {

// Helper to fill in just the Clip fields the time authority cares about.
// Clip is move-only (RAII over FFmpeg contexts) so we mutate via reference
// after emplacing into the registry.
void fillClip(Clip& c,
              FrameNumber start,
              FrameNumber duration,
              FrameNumber totalMediaFrames,
              double clipFps,
              PlaybackMode mode = PlaybackMode::Freeze) {
    c.startFrame       = start;
    c.duration         = duration;
    c.mediaStartFrame  = 0;
    c.totalMediaFrames = totalMediaFrames;
    c.framerate        = clipFps;
    c.playbackMode     = mode;
}

} // namespace

class PlaybackTimeAuthorityTest : public ::testing::Test {
protected:
    entt::registry registry;
    Timeline timeline{registry};
    PlaybackTimeAuthority auth{registry, &timeline};
};

// --- isClipActiveAtFrame --------------------------------------------------

TEST_F(PlaybackTimeAuthorityTest, IsClipActiveAtFrame_HalfOpenInterval) {
    // The clip occupies [start, start + duration) -- start is included,
    // start + duration is the first non-active frame. This is the same
    // invariant Timeline / DecodeSystem rely on.
    Clip c;
    fillClip(c, /*start*/10, /*duration*/5, /*total*/5, 30.0);
    EXPECT_FALSE(auth.isClipActiveAtFrame(c, 9));
    EXPECT_TRUE (auth.isClipActiveAtFrame(c, 10));
    EXPECT_TRUE (auth.isClipActiveAtFrame(c, 14));
    EXPECT_FALSE(auth.isClipActiveAtFrame(c, 15));
}

TEST_F(PlaybackTimeAuthorityTest, IsClipActiveAtFrame_ZeroDurationIsAlwaysInactive) {
    Clip c;
    fillClip(c, 5, 0, 0, 30.0);
    EXPECT_FALSE(auth.isClipActiveAtFrame(c, 5));
}

TEST_F(PlaybackTimeAuthorityTest, IsClipActiveAtFrame_TrailingEdge_IncludesBreakFrameWithZeroFade) {
    // Regression: a clip whose end aligns exactly with a section break used
    // to drop out of the active set the moment the playhead reached the break
    // when fadeSeconds == 0. sectionFadeTailFrames now always returns >= 1
    // for break-aligned ends, so the clip stays visible for the at-break park
    // frame and only drops on the next frame (instant cut after GO).
    timeline.setFrameRate(30.0);
    timeline.addSectionBreak(/*breakFrame*/60, /*color*/0u, /*fadeSeconds*/0.0);
    Clip c;
    fillClip(c, /*start*/0, /*duration*/60, /*total*/60, 30.0);

    EXPECT_TRUE (auth.isClipActiveAtFrame(c, 59));  // inside clip, normal
    EXPECT_TRUE (auth.isClipActiveAtFrame(c, 60));  // at break - one-frame hold
    EXPECT_FALSE(auth.isClipActiveAtFrame(c, 61));  // post-GO, instant cut
}

TEST_F(PlaybackTimeAuthorityTest, IsClipActiveAtFrame_TrailingEdge_PreservesFadeTailWindow) {
    // With fadeSeconds > 0, the tail window covers ceil(fadeSeconds * fps)
    // frames -- unchanged by the Fix 1 minimum-1 rule (max(1, ceil(...)) ==
    // ceil(...) whenever ceil(...) >= 1, which it is for any positive ramp).
    timeline.setFrameRate(30.0);
    timeline.addSectionBreak(60, 0u, /*fadeSeconds*/0.5);  // 15 frames at 30fps
    Clip c;
    fillClip(c, 0, 60, 60, 30.0);

    EXPECT_TRUE (auth.isClipActiveAtFrame(c, 60));  // at break
    EXPECT_TRUE (auth.isClipActiveAtFrame(c, 74));  // last frame in fade tail
    EXPECT_FALSE(auth.isClipActiveAtFrame(c, 75));  // tail closed
}

TEST_F(PlaybackTimeAuthorityTest, ComputeSectionFadeMultiplier_TrailingEdge_HoldsAtBreakWithZeroFade) {
    // At the at-break park frame the multiplier must be 1.0 even when
    // fadeSeconds == 0. The fade-ramp branches are skipped (fadeSeconds gate)
    // and the at-break visibility gate only fires for clips that START at
    // the break, so the multiplier defaults to 1.0 -- which is exactly the
    // "held visible while parked" behavior we want.
    timeline.setFrameRate(30.0);
    timeline.addSectionBreak(60, 0u, /*fadeSeconds*/0.0);
    Clip c;
    fillClip(c, 0, 60, 60, 30.0);
    timeline.seekToFrame(60);

    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 1.0f, 1e-4f);
}

// --- mapToMediaFrame ------------------------------------------------------

TEST_F(PlaybackTimeAuthorityTest, MapToMediaFrame_SameFrameRate_IsIdentityFromOffset) {
    timeline.setFrameRate(30.0);
    Clip c;
    fillClip(c, /*start*/0, /*duration*/16, /*total*/16, 30.0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 0),  0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 5),  5);
    EXPECT_EQ(auth.mapToMediaFrame(c, 15), 15);
}

TEST_F(PlaybackTimeAuthorityTest, MapToMediaFrame_NonZeroStartFrameOffsetsCorrectly) {
    timeline.setFrameRate(30.0);
    Clip c;
    fillClip(c, /*start*/100, /*duration*/16, /*total*/16, 30.0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 100), 0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 105), 5);
}

TEST_F(PlaybackTimeAuthorityTest, MapToMediaFrame_MixedFps_FloorsRatio) {
    // Mirrors integration_mixed_fps: 24fps clip on a 30fps timeline.
    // tl 10 -> floor(10 * 24/30) = src 8.
    timeline.setFrameRate(30.0);
    Clip c;
    fillClip(c, /*start*/0, /*duration*/20, /*total*/16, 24.0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 0),  0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 10), 8);
}

TEST_F(PlaybackTimeAuthorityTest, MapToMediaFrame_FreezeClampsToLastFrame) {
    timeline.setFrameRate(30.0);
    Clip c;
    fillClip(c, /*start*/0, /*duration*/5, /*total*/5, 30.0,
             PlaybackMode::Freeze);
    EXPECT_EQ(auth.mapToMediaFrame(c, 10), 4);  // past end -> last frame held
}

TEST_F(PlaybackTimeAuthorityTest, MapToMediaFrame_LoopWrapsModulo) {
    timeline.setFrameRate(30.0);
    Clip c;
    fillClip(c, /*start*/0, /*duration*/100, /*total*/8, 30.0,
             PlaybackMode::Loop);
    EXPECT_EQ(auth.mapToMediaFrame(c, 0),  0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 7),  7);
    EXPECT_EQ(auth.mapToMediaFrame(c, 8),  0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 15), 7);
    EXPECT_EQ(auth.mapToMediaFrame(c, 24), 0);
}

TEST_F(PlaybackTimeAuthorityTest, MapToMediaFrame_PingPongMirrorsOnOddCycle) {
    // Mirrors integration_ping_pong: 16-frame clip stretched to 64
    // timeline frames. tl 24 lands in reverse phase at index
    // (15 - (24 % 16)) = 15 - 8 = 7.
    timeline.setFrameRate(30.0);
    Clip c;
    fillClip(c, /*start*/0, /*duration*/64, /*total*/16, 30.0,
             PlaybackMode::PingPong);
    EXPECT_EQ(auth.mapToMediaFrame(c, 0),  0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 8),  8);
    EXPECT_EQ(auth.mapToMediaFrame(c, 15), 15);
    EXPECT_EQ(auth.mapToMediaFrame(c, 16), 15);  // start of reverse phase
    EXPECT_EQ(auth.mapToMediaFrame(c, 24), 7);
    EXPECT_EQ(auth.mapToMediaFrame(c, 31), 0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 32), 0);   // forward phase resumes
}

// Fix 5 (2026-05-23 follow-up) — trimmed-clip tail held-frame regression.
// A clip whose authored timeline duration is shorter than its trimmed
// source range (e.g. a long MP4 cut down to a 3000-frame slot) used to
// return `mediaStartFrame + sourceLength - 1` past clipEnd — the trimmed
// source's last frame, which the decoder never reached. That asked the
// FrameCache for a frame it would never have, stalling the seek-sync gate
// for its full 3-second preroll timeout on every Section GO with
// fadeSeconds > 0. The held frame is now the media frame the clip
// displayed at its last authored timeline frame (clipEnd - 1) with the
// usual wrap math, NOT the trimmed source end.
TEST_F(PlaybackTimeAuthorityTest,
       MapToMediaFrame_TrimmedClipPastEnd_HoldsAtLastPlayedFrame) {
    timeline.setFrameRate(30.0);
    Clip c;
    // 3000-frame timeline window over a 19036-frame source — reproduces
    // the operator log (mediaFrame=19035 vs lastReq=4710 before the fix).
    fillClip(c, /*start*/0, /*duration*/3000, /*total*/19036, 30.0,
             PlaybackMode::Freeze);
    c.mediaOutFrame = 19035;
    // clipEnd = 3000. Tail frames 3000, 3001, 3010, ... must all map to
    // mediaStartFrame + (clip.duration - 1) = 2999, NOT 19035.
    EXPECT_EQ(auth.mapToMediaFrame(c, 3000), 2999);
    EXPECT_EQ(auth.mapToMediaFrame(c, 3001), 2999);
    EXPECT_EQ(auth.mapToMediaFrame(c, 3149), 2999);
}

// Loop clip in the tail without continuation — the clip authored 250
// timeline frames over a 100-frame source so it wraps 2.5× naturally.
// Past clipEnd, the held frame is what was on screen at clipEnd - 1
// (timeline frame 249, sourceLocalFrame 249, Loop-wrap to 49) — NOT
// the trimmed source's last frame (99).
TEST_F(PlaybackTimeAuthorityTest,
       MapToMediaFrame_LoopClipPastEnd_HoldsAtLastWrappedFrame) {
    timeline.setFrameRate(30.0);
    Clip c;
    fillClip(c, /*start*/0, /*duration*/250, /*total*/100, 30.0,
             PlaybackMode::Loop);
    // Pre-tail still wraps as before.
    EXPECT_EQ(auth.mapToMediaFrame(c, 0),   0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 99),  99);
    EXPECT_EQ(auth.mapToMediaFrame(c, 100), 0);
    EXPECT_EQ(auth.mapToMediaFrame(c, 249), 49);   // last authored frame
    // Past clipEnd (250) — the held frame is what 249 mapped to (49),
    // not the trimmed source's last frame (99).
    EXPECT_EQ(auth.mapToMediaFrame(c, 250), 49);
    EXPECT_EQ(auth.mapToMediaFrame(c, 260), 49);
}

// Backward-compat — Freeze clip whose authored duration matches its
// source range still holds at the source's last frame past the end.
// This was already covered by MapToMediaFrame_FreezeClampsToLastFrame
// above; this case adds explicit coverage of the post-end frames the
// section-fade tail uses (clipEnd, clipEnd + tailFrames - 1) so a
// regression in the wrap math at exact clipEnd-1 shows up here.
TEST_F(PlaybackTimeAuthorityTest,
       MapToMediaFrame_FreezeUntrimmedPastEnd_StillReturnsSourceEnd) {
    timeline.setFrameRate(30.0);
    Clip c;
    fillClip(c, /*start*/0, /*duration*/5, /*total*/5, 30.0,
             PlaybackMode::Freeze);
    // duration == sourceLength == 5: clipEnd-1=4 maps to source frame 4,
    // which is also sourceLength-1. No behavior change pre/post fix.
    EXPECT_EQ(auth.mapToMediaFrame(c, 4),  4);
    EXPECT_EQ(auth.mapToMediaFrame(c, 5),  4);  // at clipEnd
    EXPECT_EQ(auth.mapToMediaFrame(c, 10), 4);
}

// --- buildActiveSet -------------------------------------------------------

TEST_F(PlaybackTimeAuthorityTest, BuildActiveSet_OnlyIncludesAllocatedActiveClips) {
    timeline.setFrameRate(30.0);
    timeline.seekToFrame(5);

    // Active + allocated: included
    auto includedEntity = registry.create();
    auto& clipA = registry.emplace<Clip>(includedEntity);
    fillClip(clipA, 0, 10, 10, 30.0);
    auto& texA = registry.emplace<VideoTexture>(includedEntity);
    texA.descriptorSlot = 7;

    // Active but unallocated: skipped
    auto unallocatedEntity = registry.create();
    auto& clipB = registry.emplace<Clip>(unallocatedEntity);
    fillClip(clipB, 0, 10, 10, 30.0);
    registry.emplace<VideoTexture>(unallocatedEntity);  // descriptorSlot stays UINT32_MAX

    // Allocated but inactive (clip ends at frame 4, current frame is 5)
    auto inactiveEntity = registry.create();
    auto& clipC = registry.emplace<Clip>(inactiveEntity);
    fillClip(clipC, 0, 4, 4, 30.0);
    auto& texC = registry.emplace<VideoTexture>(inactiveEntity);
    texC.descriptorSlot = 9;

    std::vector<ActiveClip> active;
    auth.buildActiveSet(active);

    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].entity, includedEntity);
    EXPECT_EQ(active[0].descriptorSlot, 7u);
    EXPECT_EQ(active[0].mediaFrame, 5);
    EXPECT_TRUE(active[0].ocioOverride.empty());
}

TEST_F(PlaybackTimeAuthorityTest, BuildActiveSet_RespectsMixedFpsAtCurrentFrame) {
    // Same setup as integration_mixed_fps -- a 24fps clip on a 30fps
    // timeline at tl frame 10 should report sourceFrame 8 in the active
    // set tuple.
    timeline.setFrameRate(30.0);
    timeline.seekToFrame(10);

    auto e = registry.create();
    auto& clip = registry.emplace<Clip>(e);
    fillClip(clip, 0, 20, 16, 24.0);
    auto& tex = registry.emplace<VideoTexture>(e);
    tex.descriptorSlot = 3;

    std::vector<ActiveClip> active;
    auth.buildActiveSet(active);

    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].entity, e);
    EXPECT_EQ(active[0].mediaFrame, 8);
}

TEST_F(PlaybackTimeAuthorityTest, BuildActiveSet_ClearsCallerVectorBeforeAppending) {
    // Caller may reuse a long-lived vector across ticks -- buildActiveSet
    // must clear it (not append) so stale entries from a previous tick
    // don't bleed through.
    timeline.setFrameRate(30.0);
    timeline.seekToFrame(0);

    std::vector<ActiveClip> active;
    ActiveClip stale;
    stale.entity = static_cast<entt::entity>(99);
    stale.descriptorSlot = 42;
    active.push_back(std::move(stale));
    ASSERT_EQ(active.size(), 1u);

    auth.buildActiveSet(active);
    EXPECT_TRUE(active.empty());
}

// --- Timing ---------------------------------------------------------------

TEST_F(PlaybackTimeAuthorityTest, Timing_StartTimingResetsCounters) {
    auth.incrementFrameCount();
    auth.incrementFrameCount();
    EXPECT_EQ(auth.getFrameCount(), 2u);

    auth.startTiming();
    EXPECT_EQ(auth.getFrameCount(), 0u);
    EXPECT_DOUBLE_EQ(auth.getDeltaTime(), 0.0);
    EXPECT_DOUBLE_EQ(auth.getElapsedTime(), 0.0);
}
