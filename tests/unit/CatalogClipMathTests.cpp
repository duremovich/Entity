#include <gtest/gtest.h>

#include "entity/director/CatalogClipMath.hpp"

#include <cstdint>

using namespace entity;

// ---------------------------------------------------------------------------
// CatalogClipMath unit tests (issue #74).
//
// mapToMediaFrameFromCatalogEx is the registry-free mapping the show thread
// uses both for rendering (via the frame-only wrapper in buildRenderFrame)
// and for steering decode workers during editor stalls. These tests pin the
// wrap math, the PingPong parity flag, the tail-hold flag, and the
// totalMediaFrames fallback for clips whose mediaOutFrame is still the
// unresolved -1 sentinel.
// ---------------------------------------------------------------------------

namespace {

// A 100-timeline-frame clip over source frames [0, 49] (sourceLength = 50),
// clip fps == timeline fps == 30 so the frame-rate ratio is 1.
bus::ClipCatalogEntry makeEntry(PlaybackMode mode,
                                FrameNumber startFrame = 0,
                                FrameNumber duration = 100,
                                FrameNumber mediaStartFrame = 0,
                                FrameNumber mediaOutFrame = 49) {
    bus::ClipCatalogEntry e;
    e.entity          = 1;
    e.startFrame      = startFrame;
    e.duration        = duration;
    e.mediaStartFrame = mediaStartFrame;
    e.mediaOutFrame   = mediaOutFrame;
    e.framerate       = 30.0;
    e.playbackMode    = static_cast<int>(mode);
    e.sectionBehavior = static_cast<int>(SectionBehavior::Normal);
    return e;
}

constexpr double kTlFps = 30.0;
constexpr std::int64_t kNowNs = 0;

} // namespace

TEST(CatalogClipMath, NaturalForwardMapsOneToOne) {
    const auto e = makeEntry(PlaybackMode::Loop);
    const auto r = mapToMediaFrameFromCatalogEx(e, 25, kTlFps, kNowNs);
    EXPECT_EQ(r.mediaFrame, 25);
    EXPECT_FALSE(r.pingPongReverse);
    EXPECT_FALSE(r.inTailHold);
}

TEST(CatalogClipMath, NaturalLoopWraps) {
    // sourceLength = 50; timeline frame 75 -> sourceLocal 75 -> 75 % 50 = 25.
    const auto e = makeEntry(PlaybackMode::Loop);
    const auto r = mapToMediaFrameFromCatalogEx(e, 75, kTlFps, kNowNs);
    EXPECT_EQ(r.mediaFrame, 25);
    EXPECT_FALSE(r.pingPongReverse);
    EXPECT_FALSE(r.inTailHold);
}

TEST(CatalogClipMath, NaturalFreezeClampsToLastSourceFrame) {
    const auto e = makeEntry(PlaybackMode::Freeze);
    const auto r = mapToMediaFrameFromCatalogEx(e, 75, kTlFps, kNowNs);
    EXPECT_EQ(r.mediaFrame, 49);
    EXPECT_FALSE(r.pingPongReverse);
    EXPECT_FALSE(r.inTailHold);
}

TEST(CatalogClipMath, NaturalPingPongParity) {
    const auto e = makeEntry(PlaybackMode::PingPong);
    // Cycle 0 (frames 0..49): forward.
    const auto fwd = mapToMediaFrameFromCatalogEx(e, 30, kTlFps, kNowNs);
    EXPECT_EQ(fwd.mediaFrame, 30);
    EXPECT_FALSE(fwd.pingPongReverse);
    // Cycle 1 (frames 50..99): reverse. sourceLocal 60 -> pos 10 -> 49 - 10.
    const auto rev = mapToMediaFrameFromCatalogEx(e, 60, kTlFps, kNowNs);
    EXPECT_EQ(rev.mediaFrame, 39);
    EXPECT_TRUE(rev.pingPongReverse);
}

TEST(CatalogClipMath, MediaStartOffsetApplies) {
    // Source window [10, 39], sourceLength = 30.
    const auto e = makeEntry(PlaybackMode::Loop, 0, 100, 10, 39);
    const auto r = mapToMediaFrameFromCatalogEx(e, 5, kTlFps, kNowNs);
    EXPECT_EQ(r.mediaFrame, 15);
    // Wrap: sourceLocal 35 -> 35 % 30 = 5 -> media 15.
    const auto w = mapToMediaFrameFromCatalogEx(e, 35, kTlFps, kNowNs);
    EXPECT_EQ(w.mediaFrame, 15);
}

TEST(CatalogClipMath, PastRealEndClampsAndFlagsTailHold) {
    // No phase, no extension: realEnd = 100. Frame 120 clamps to 99 ->
    // sourceLocal 99 -> Loop wrap 99 % 50 = 49.
    const auto e = makeEntry(PlaybackMode::Loop);
    const auto r = mapToMediaFrameFromCatalogEx(e, 120, kTlFps, kNowNs);
    EXPECT_EQ(r.mediaFrame, 49);
    EXPECT_TRUE(r.inTailHold);
}

TEST(CatalogClipMath, TailHoldPhaseShortCircuits) {
    auto e = makeEntry(PlaybackMode::Loop);
    e.hasPhase                 = true;
    e.phase_tailHoldMediaFrame = 42;
    const auto r = mapToMediaFrameFromCatalogEx(e, 120, kTlFps, kNowNs);
    EXPECT_EQ(r.mediaFrame, 42);
    EXPECT_TRUE(r.inTailHold);
    // Before realEnd the hold frame must NOT short-circuit.
    const auto live = mapToMediaFrameFromCatalogEx(e, 25, kTlFps, kNowNs);
    EXPECT_EQ(live.mediaFrame, 25);
    EXPECT_FALSE(live.inTailHold);
}

TEST(CatalogClipMath, PostBreakAnchorSteersMapping) {
    auto e = makeEntry(PlaybackMode::Loop);
    e.hasPhase                   = true;
    e.phase_postBreakMediaAnchor = 20;
    e.phase_anchorTimelineFrame  = 60;
    // At the anchor frame the mapping resumes from the anchored media frame.
    const auto at = mapToMediaFrameFromCatalogEx(e, 60, kTlFps, kNowNs);
    EXPECT_EQ(at.mediaFrame, 20);
    // 10 timeline frames later (ratio 1) -> media 30.
    const auto later = mapToMediaFrameFromCatalogEx(e, 70, kTlFps, kNowNs);
    EXPECT_EQ(later.mediaFrame, 30);
    // Before the anchor frame the natural path applies (stale-anchor guard).
    const auto before = mapToMediaFrameFromCatalogEx(e, 30, kTlFps, kNowNs);
    EXPECT_EQ(before.mediaFrame, 30);
}

TEST(CatalogClipMath, ContinuationWallClockAnchorDrivesPhase) {
    auto e = makeEntry(PlaybackMode::Loop);
    e.hasPhase                       = true;
    e.phase_inContinuation           = true;
    e.phase_continuationStartTimeNs  = 1'000'000'000;  // 1 s
    e.phase_continuationSeedFrames   = 10.0;
    // 2 s after the anchor at 30 fps: phase = 10 + 60 = 70 -> 70 % 50 = 20.
    const std::int64_t nowNs = 3'000'000'000;
    const auto r = mapToMediaFrameFromCatalogEx(e, 0, kTlFps, nowNs);
    EXPECT_EQ(r.mediaFrame, 20);
    EXPECT_FALSE(r.inTailHold);
}

TEST(CatalogClipMath, ContinuationPingPongParity) {
    auto e = makeEntry(PlaybackMode::PingPong);
    e.hasPhase                      = true;
    e.phase_inContinuation          = true;
    e.phase_continuationStartTimeNs = 1'000'000'000;
    e.phase_continuationSeedFrames  = 0.0;
    // 2 s -> phase 60 -> cycle 1 (reverse), pos 10 -> 49 - 10 = 39.
    const auto r = mapToMediaFrameFromCatalogEx(e, 0, kTlFps, 3'000'000'000);
    EXPECT_EQ(r.mediaFrame, 39);
    EXPECT_TRUE(r.pingPongReverse);
}

TEST(CatalogClipMath, ContinuationFreezeClampsInsteadOfWrapping) {
    auto e = makeEntry(PlaybackMode::Freeze);
    e.hasPhase                      = true;
    e.phase_inContinuation          = true;
    e.phase_continuationStartTimeNs = 1'000'000'000;
    e.phase_continuationSeedFrames  = 0.0;
    const auto r = mapToMediaFrameFromCatalogEx(e, 0, kTlFps, 5'000'000'000);
    EXPECT_EQ(r.mediaFrame, 49);
}

TEST(CatalogClipMath, TotalMediaFramesBackstopsUnresolvedOutPoint) {
    // mediaOutFrame = -1 (media not yet resolved). Pre-#74 the catalog
    // dropped totalMediaFrames, effectivePlaybackLength collapsed to 1 and
    // every frame pinned to mediaStartFrame. With the baked field the Loop
    // wrap uses the real source length.
    auto e = makeEntry(PlaybackMode::Loop, 0, 100, 0, -1);
    e.totalMediaFrames = 50;
    const auto r = mapToMediaFrameFromCatalogEx(e, 75, kTlFps, kNowNs);
    EXPECT_EQ(r.mediaFrame, 25);

    // Regression shape check: without the field the mapping degrades to the
    // 1-frame pin (documents the pre-#74 behavior the bake now avoids).
    auto legacy = makeEntry(PlaybackMode::Loop, 0, 100, 0, -1);
    legacy.totalMediaFrames = 0;
    const auto pin = mapToMediaFrameFromCatalogEx(legacy, 75, kTlFps, kNowNs);
    EXPECT_EQ(pin.mediaFrame, 0);
}

TEST(CatalogClipMath, WrapperMatchesExFrame) {
    const auto e = makeEntry(PlaybackMode::PingPong);
    for (FrameNumber f : {0, 25, 60, 99, 120}) {
        EXPECT_EQ(mapToMediaFrameFromCatalog(e, f, kTlFps, kNowNs),
                  mapToMediaFrameFromCatalogEx(e, f, kTlFps, kNowNs).mediaFrame)
            << "frame " << f;
    }
}

TEST(CatalogClipMath, ClipFromCatalogCopiesSchedulingFields) {
    auto e = makeEntry(PlaybackMode::PingPong, 10, 90, 5, 44);
    e.totalMediaFrames = 200;
    const Clip c = clipFromCatalog(e);
    EXPECT_EQ(c.startFrame, 10);
    EXPECT_EQ(c.duration, 90);
    EXPECT_EQ(c.mediaStartFrame, 5);
    EXPECT_EQ(c.mediaOutFrame, 44);
    EXPECT_EQ(c.totalMediaFrames, 200);
    EXPECT_EQ(c.playbackMode, PlaybackMode::PingPong);
    EXPECT_EQ(c.sectionBehavior, SectionBehavior::Normal);
    EXPECT_EQ(effectivePlaybackLength(c), 40);
}
