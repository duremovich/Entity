// Phase C — unit tests for the entity-aware mapToMediaFrame continuation
// branch. Once a ClipPlaybackPhase is attached to an entity and `inContinuation`
// is true, mapToMediaFrame derives the source-media frame from
// `sourcePhaseFrames` instead of the (frozen) timeline-frame delta. These
// tests exercise the Freeze cap, Loop wrap, and PingPong reflection branches
// directly, plus the regression guard that an attached-but-inactive phase
// component does not change behavior.

#include <gtest/gtest.h>

#include "entity/components/Clip.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/timeline/Timeline.hpp"

#include <entt/entt.hpp>

using namespace entity;

namespace {

void fillClip(Clip& c,
              FrameNumber start,
              FrameNumber duration,
              FrameNumber totalMediaFrames,
              double clipFps,
              PlaybackMode mode) {
    c.startFrame       = start;
    c.duration         = duration;
    c.mediaStartFrame  = 0;
    c.totalMediaFrames = totalMediaFrames;
    c.framerate        = clipFps;
    c.playbackMode     = mode;
}

} // namespace

class ClipPlaybackPhaseTest : public ::testing::Test {
protected:
    entt::registry registry;
    Timeline timeline{registry};
    PlaybackTimeAuthority auth{registry, &timeline};

    // Helpers: create a fully-set-up clip + continuation phase. Returns the
    // entity so the caller can mutate the phase if needed.
    entt::entity makeClipInContinuation(FrameNumber duration,
                                        FrameNumber sourceLength,
                                        double clipFps,
                                        PlaybackMode mode,
                                        double sourcePhaseFrames) {
        auto e = registry.create();
        auto& c = registry.emplace<Clip>(e);
        fillClip(c, /*start*/0, duration, sourceLength, clipFps, mode);
        auto& phase = registry.emplace<ClipPlaybackPhase>(e);
        phase.sourcePhaseFrames = sourcePhaseFrames;
        phase.inContinuation = true;
        return e;
    }
};

// --- Freeze cap -----------------------------------------------------------

TEST_F(ClipPlaybackPhaseTest, ContinuationFreeze_ClampsToLastFrame) {
    timeline.setFrameRate(30.0);
    auto e = makeClipInContinuation(/*duration*/100, /*sourceLength*/10,
                                    /*fps*/30.0, PlaybackMode::Freeze,
                                    /*phase*/100.0);
    const auto& clip = registry.get<Clip>(e);
    // Phase well past source length collapses to mediaStartFrame + (10 - 1).
    EXPECT_EQ(auth.mapToMediaFrame(e, clip, /*timelineFrame ignored*/0),
              clip.mediaStartFrame + 9);
}

// --- Loop wrap ------------------------------------------------------------

TEST_F(ClipPlaybackPhaseTest, ContinuationLoop_WrapsModulo) {
    timeline.setFrameRate(30.0);
    auto e = makeClipInContinuation(/*duration*/100, /*sourceLength*/10,
                                    /*fps*/30.0, PlaybackMode::Loop,
                                    /*phase*/37.0);
    const auto& clip = registry.get<Clip>(e);
    // 37 % 10 = 7
    EXPECT_EQ(auth.mapToMediaFrame(e, clip, 0),
              clip.mediaStartFrame + 7);
}

// --- PingPong: cycles 0 (forward), 1 (reverse), 2 (forward) ---------------

TEST_F(ClipPlaybackPhaseTest, ContinuationPingPong_ForwardCycle0) {
    timeline.setFrameRate(30.0);
    auto e = makeClipInContinuation(/*duration*/100, /*sourceLength*/10,
                                    /*fps*/30.0, PlaybackMode::PingPong,
                                    /*phase*/5.0);
    const auto& clip = registry.get<Clip>(e);
    // cycle=0 (even -> forward), pos=5 -> mediaStartFrame + 5
    EXPECT_EQ(auth.mapToMediaFrame(e, clip, 0),
              clip.mediaStartFrame + 5);
}

TEST_F(ClipPlaybackPhaseTest, ContinuationPingPong_ReverseCycle1) {
    timeline.setFrameRate(30.0);
    auto e = makeClipInContinuation(/*duration*/100, /*sourceLength*/10,
                                    /*fps*/30.0, PlaybackMode::PingPong,
                                    /*phase*/15.0);
    const auto& clip = registry.get<Clip>(e);
    // cycle=1 (odd -> reverse), pos=5 -> mediaStartFrame + (10-1-5) = +4
    EXPECT_EQ(auth.mapToMediaFrame(e, clip, 0),
              clip.mediaStartFrame + 4);
}

TEST_F(ClipPlaybackPhaseTest, ContinuationPingPong_ForwardCycle2) {
    timeline.setFrameRate(30.0);
    auto e = makeClipInContinuation(/*duration*/100, /*sourceLength*/10,
                                    /*fps*/30.0, PlaybackMode::PingPong,
                                    /*phase*/25.0);
    const auto& clip = registry.get<Clip>(e);
    // cycle=2 (even -> forward), pos=5 -> mediaStartFrame + 5
    EXPECT_EQ(auth.mapToMediaFrame(e, clip, 0),
              clip.mediaStartFrame + 5);
}

// --- Regression: attached but inactive phase falls through ---------------

TEST_F(ClipPlaybackPhaseTest, InactivePhaseFallsThroughToTimelinePath) {
    timeline.setFrameRate(30.0);

    // Create a clip with an attached-but-inactive phase. The continuation
    // path must NOT fire -- the result must match the two-arg overload exactly.
    auto e = registry.create();
    auto& c = registry.emplace<Clip>(e);
    fillClip(c, /*start*/0, /*duration*/16, /*sourceLength*/16,
             /*fps*/30.0, PlaybackMode::Loop);
    auto& phase = registry.emplace<ClipPlaybackPhase>(e);
    phase.sourcePhaseFrames = 999.0;  // would map to a wrong frame if used
    phase.inContinuation = false;     // but inactive, so ignored

    // Three-arg overload should match the two-arg overload exactly.
    EXPECT_EQ(auth.mapToMediaFrame(e, c, 5),
              auth.mapToMediaFrame(c, 5));
    EXPECT_EQ(auth.mapToMediaFrame(e, c, 5), 5);
}

// --- Regression: no component at all also falls through ------------------

TEST_F(ClipPlaybackPhaseTest, NoPhaseComponentFallsThroughToTimelinePath) {
    timeline.setFrameRate(30.0);

    auto e = registry.create();
    auto& c = registry.emplace<Clip>(e);
    fillClip(c, /*start*/0, /*duration*/16, /*sourceLength*/16,
             /*fps*/30.0, PlaybackMode::Loop);

    // No ClipPlaybackPhase -- entity-aware overload must match the pure overload.
    EXPECT_EQ(auth.mapToMediaFrame(e, c, 5),
              auth.mapToMediaFrame(c, 5));
}
