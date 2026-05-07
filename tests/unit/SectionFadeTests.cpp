// Phase D — section fade envelope unit tests. Exercises
// PlaybackTimeAuthority::computeSectionFadeMultiplier directly, against a
// Timeline with section breaks set up via Timeline::addSectionBreak.
//
// The helper is pure: given the timeline's current frame plus the clip's
// start/end, it scans the section vector for breaks aligned (±1 timeline
// frame) with either edge and returns the ramp value. The cases here cover:
//   1. No aligned breaks      -> 1.0
//   2. fadeSeconds == 0       -> 1.0 (break exists but is inert)
//   3. Fade-in mid-ramp       -> proportional ramp
//   4. Fade-out mid-ramp      -> proportional ramp
//   5. Snap tolerance         -> ±1 frame counts; ±2 frames does not
//   6. Both ends aligned      -> min-combine of both ramps
//   7. Outside fade window    -> 1.0 (clip plays normally between fades)

#include <gtest/gtest.h>

#include "entity/components/Clip.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/timeline/Timeline.hpp"

#include <entt/entt.hpp>

using namespace entity;

namespace {

// 30 fps timeline -> 1 frame == 33333.33... microseconds. Helper to
// compute the exact microsecond value Timeline::frameToTime would yield
// for a given frame, so addSectionBreak lands at the same frame when
// queried via timeToFrame.
Timecode toTimelineMicros(double frame, double fps) {
    return static_cast<Timecode>(std::round((frame / fps) * 1000000.0));
}

void fillClip(Clip& c,
              FrameNumber start,
              FrameNumber duration,
              double clipFps = 30.0) {
    c.startFrame       = start;
    c.duration         = duration;
    c.mediaStartFrame  = 0;
    c.totalMediaFrames = duration;
    c.framerate        = clipFps;
    c.playbackMode     = PlaybackMode::Freeze;
}

} // namespace

class SectionFadeTest : public ::testing::Test {
protected:
    entt::registry registry;
    Timeline timeline{registry};
    PlaybackTimeAuthority auth{registry, &timeline};

    void SetUp() override {
        timeline.setFrameRate(30.0);
    }

    // Helper: park the timeline at `frame` so computeSectionFadeMultiplier
    // sees the right currentFrame.
    void seekToFrame(FrameNumber frame) {
        timeline.seekToFrame(frame);
    }
};

// 1. No aligned break -> identity multiplier.
TEST_F(SectionFadeTest, NoAlignedBreak_ReturnsOne) {
    Clip c;
    fillClip(c, /*start*/0, /*duration*/60);

    // Add a break far away from the clip's edges with non-zero fade.
    timeline.addSectionBreak(toTimelineMicros(120, 30.0), "FarBreak",
                             0xFF6090C8, 1.0);

    seekToFrame(30);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 2. Aligned break with fadeSeconds == 0 -> inert (no envelope).
TEST_F(SectionFadeTest, ZeroFadeSeconds_NoEnvelope) {
    Clip c;
    fillClip(c, /*start*/30, /*duration*/60);

    // Break exactly at clip start, but fadeSeconds = 0.
    timeline.addSectionBreak(toTimelineMicros(30, 30.0), "ZeroFade",
                             0xFF6090C8, 0.0);

    seekToFrame(30);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 3. Fade-in mid-ramp.
TEST_F(SectionFadeTest, FadeIn_MidRamp_ProportionalValue) {
    Clip c;
    fillClip(c, /*start*/30, /*duration*/120);

    // Break at clip's start, 1.0 second fade -> 30 frames at 30fps.
    timeline.addSectionBreak(toTimelineMicros(30, 30.0), "FadeIn",
                             0xFF6090C8, 1.0);

    // Frame 30 = clip start, t = 0/30 -> 0.0.
    seekToFrame(30);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 0.0f, 1e-5f);

    // Frame 45 = mid-fade-in, t = 15/30 -> 0.5.
    seekToFrame(45);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 0.5f, 1e-5f);

    // Frame 59 = last frame inside the fade window, t = 29/30 ~= 0.967.
    seekToFrame(59);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 29.0f / 30.0f, 1e-5f);

    // Frame 60 = first frame outside the fade window -> 1.0.
    seekToFrame(60);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 4. Fade-out mid-ramp.
TEST_F(SectionFadeTest, FadeOut_MidRamp_ProportionalValue) {
    Clip c;
    fillClip(c, /*start*/0, /*duration*/60);
    // clipEnd = 60.

    // Break at clip end, 0.5s fade -> 15 frames at 30fps.
    // Fade-out window: (clipEnd - 15, clipEnd] = (45, 60].
    timeline.addSectionBreak(toTimelineMicros(60, 30.0), "FadeOut",
                             0xFF6090C8, 0.5);

    // Frame 45 = just outside the open lower edge -> 1.0.
    seekToFrame(45);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);

    // Frame 50 = mid fade-out: (60 - 50)/15 = 10/15 ~= 0.667.
    seekToFrame(50);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 10.0f / 15.0f, 1e-5f);

    // Frame 59 = late in fade-out: (60 - 59)/15 = 1/15 ~= 0.067.
    seekToFrame(59);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 1.0f / 15.0f, 1e-5f);

    // Frame 60 = clip end (still inside (45, 60]): (60 - 60)/15 = 0.
    seekToFrame(60);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 0.0f, 1e-5f);
}

// 5a. Snap tolerance: clip start ±1 frame from break still aligns.
TEST_F(SectionFadeTest, SnapTolerance_OneFrameOff_StillAligns) {
    Clip c;
    // Clip starts at frame 31, break at frame 30 -> diff = 1, within ±1.
    fillClip(c, /*start*/31, /*duration*/120);
    timeline.addSectionBreak(toTimelineMicros(30, 30.0), "Snap1",
                             0xFF6090C8, 1.0);

    // Frame 31 = clip start + 0; ramp t = 0/30 -> 0.0.
    seekToFrame(31);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 0.0f, 1e-5f);

    seekToFrame(46);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 15.0f / 30.0f, 1e-5f);
}

// 5b. Snap tolerance: 2 frames off does NOT align (no envelope).
TEST_F(SectionFadeTest, SnapTolerance_TwoFramesOff_NoEnvelope) {
    Clip c;
    fillClip(c, /*start*/32, /*duration*/120);
    timeline.addSectionBreak(toTimelineMicros(30, 30.0), "Snap2",
                             0xFF6090C8, 1.0);

    // Frame 32 (clip start). With no alignment, multiplier should be 1.0
    // even though the timeline frame is right at the clip boundary.
    seekToFrame(32);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 6. Both ends aligned: min-combine of fade-in and fade-out.
TEST_F(SectionFadeTest, BothEndsAligned_MinCombine) {
    // Clip 30-frames long, both ends on breaks with 0.5s fade (15 frames).
    // Fades would overlap entirely (fadeIn covers [0, 15), fadeOut covers
    // (15, 30]). At frame 15 (the apex), neither ramp's window is open
    // -> identity 1.0.
    Clip c;
    fillClip(c, /*start*/30, /*duration*/30);

    timeline.addSectionBreak(toTimelineMicros(30, 30.0), "StartBreak",
                             0xFF6090C8, 0.5);
    timeline.addSectionBreak(toTimelineMicros(60, 30.0), "EndBreak",
                             0xFF7060C8, 0.5);

    // Frame 30: t = 0/15 fade-in -> 0.0. Fade-out window is (45, 60], so
    // not yet inside.
    seekToFrame(30);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 0.0f, 1e-5f);

    // Frame 37 (mid fade-in): t = 7/15 fade-in. Fade-out window not yet open.
    seekToFrame(37);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 7.0f / 15.0f, 1e-5f);

    // Frame 45: outside both fade windows -> 1.0.
    seekToFrame(45);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);

    // Frame 50: mid fade-out, t = 10/15 -> 10/15 ~= 0.667. Fade-in is gone.
    seekToFrame(50);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 10.0f / 15.0f, 1e-5f);

    // Frame 60: end of fade-out, t = 0/15 -> 0.0.
    seekToFrame(60);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 0.0f, 1e-5f);
}

// 6b. Both ends aligned, fades overlap: the min(fadeIn, fadeOut) wins so
// a clip shorter than fadeIn + fadeOut never shows above the smaller ramp.
TEST_F(SectionFadeTest, BothEndsAligned_OverlappingFades_MinCombine) {
    // Clip 10 frames, fade 1.0s each end (30 frames each); they overlap
    // entirely. At any point inside the clip, both windows are open.
    Clip c;
    fillClip(c, /*start*/30, /*duration*/10);

    timeline.addSectionBreak(toTimelineMicros(30, 30.0), "StartBreak",
                             0xFF6090C8, 1.0);
    timeline.addSectionBreak(toTimelineMicros(40, 30.0), "EndBreak",
                             0xFF7060C8, 1.0);

    // At frame 35: fade-in t = 5/30, fade-out t = 5/30 -> min = 5/30.
    seekToFrame(35);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 5.0f / 30.0f, 1e-5f);

    // At frame 31: fade-in t = 1/30, fade-out t = 9/30 -> min = 1/30.
    seekToFrame(31);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 1.0f / 30.0f, 1e-5f);
}

// 7. Outside any fade window mid-clip -> 1.0.
TEST_F(SectionFadeTest, OutsideFadeWindow_ReturnsOne) {
    Clip c;
    fillClip(c, /*start*/30, /*duration*/120);

    timeline.addSectionBreak(toTimelineMicros(30, 30.0), "FadeIn",
                             0xFF6090C8, 0.5);   // 15-frame fade-in
    timeline.addSectionBreak(toTimelineMicros(150, 30.0), "FadeOut",
                             0xFF7060C8, 0.5);   // 15-frame fade-out

    // Frame 75 is well past fade-in (45+) and well before fade-out
    // (135+). Result must be identity.
    seekToFrame(75);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 8. Empty Timeline section list short-circuit.
TEST_F(SectionFadeTest, NoSections_ReturnsOne) {
    Clip c;
    fillClip(c, /*start*/0, /*duration*/60);
    seekToFrame(15);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}
