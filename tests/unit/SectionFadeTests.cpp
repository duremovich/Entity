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
    timeline.addSectionBreak(toTimelineMicros(120, 30.0), 0xFF6090C8, 1.0);

    seekToFrame(30);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 2. Aligned break with fadeSeconds == 0 -> inert (no envelope).
TEST_F(SectionFadeTest, ZeroFadeSeconds_NoEnvelope) {
    Clip c;
    fillClip(c, /*start*/30, /*duration*/60);

    // Break exactly at clip start, but fadeSeconds = 0.
    timeline.addSectionBreak(toTimelineMicros(30, 30.0), 0xFF6090C8, 0.0);

    seekToFrame(30);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 3. Fade-in mid-ramp.
TEST_F(SectionFadeTest, FadeIn_MidRamp_ProportionalValue) {
    Clip c;
    fillClip(c, /*start*/30, /*duration*/120);

    // Break at clip's start, 1.0 second fade -> 30 frames at 30fps.
    timeline.addSectionBreak(toTimelineMicros(30, 30.0), 0xFF6090C8, 1.0);

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

// 4. Fade-out mid-ramp. Phase 6 — held tail post-clipEnd. Window is
// [clipEnd, clipEnd + fadeFrames).
TEST_F(SectionFadeTest, FadeOut_MidRamp_ProportionalValue) {
    Clip c;
    fillClip(c, /*start*/0, /*duration*/60);
    // clipEnd = 60.

    // Break at clip end, 0.5s fade -> 15 frames at 30fps.
    // New fade-out window: [60, 75).
    timeline.addSectionBreak(toTimelineMicros(60, 30.0), 0xFF6090C8, 0.5);

    // Frame 45 = well inside the clip, no envelope -> 1.0.
    seekToFrame(45);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);

    // Frame 60 = clip end: held at full alpha, t = 1 - 0/15 = 1.0.
    seekToFrame(60);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 1.0f, 1e-5f);

    // Frame 65 = mid fade-out: t = 1 - 5/15 = 10/15 ~= 0.667.
    seekToFrame(65);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 10.0f / 15.0f, 1e-5f);

    // Frame 74 = last frame inside the open-upper window:
    // t = 1 - 14/15 = 1/15 ~= 0.067.
    seekToFrame(74);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 1.0f / 15.0f, 1e-5f);

    // Frame 75 = upper edge (open), window closed -> identity 1.0.
    seekToFrame(75);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 5a. Snap tolerance: clip start ±1 frame from break still aligns.
TEST_F(SectionFadeTest, SnapTolerance_OneFrameOff_StillAligns) {
    Clip c;
    // Clip starts at frame 31, break at frame 30 -> diff = 1, within ±1.
    fillClip(c, /*start*/31, /*duration*/120);
    timeline.addSectionBreak(toTimelineMicros(30, 30.0), 0xFF6090C8, 1.0);

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
    timeline.addSectionBreak(toTimelineMicros(30, 30.0), 0xFF6090C8, 1.0);

    // Frame 32 (clip start). With no alignment, multiplier should be 1.0
    // even though the timeline frame is right at the clip boundary.
    seekToFrame(32);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 6. Both ends aligned: min-combine of fade-in and fade-out.
// Phase 6 — fade-in window stays at clip start; fade-out window moves
// past clip end. Windows can no longer overlap on the same clip.
TEST_F(SectionFadeTest, BothEndsAligned_MinCombine) {
    // Clip 30-frames long, both ends on breaks with 0.5s fade (15 frames).
    // Fade-in window: [30, 45). Fade-out window: [60, 75).
    Clip c;
    fillClip(c, /*start*/30, /*duration*/30);

    timeline.addSectionBreak(toTimelineMicros(30, 30.0), 0xFF6090C8, 0.5);
    timeline.addSectionBreak(toTimelineMicros(60, 30.0), 0xFF7060C8, 0.5);

    // Frame 30: t = 0/15 fade-in -> 0.0. Fade-out window not yet open.
    seekToFrame(30);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 0.0f, 1e-5f);

    // Frame 37 (mid fade-in): t = 7/15. Fade-out window not yet open.
    seekToFrame(37);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 7.0f / 15.0f, 1e-5f);

    // Frame 45: fade-in window just closed; fade-out not yet open -> 1.0.
    seekToFrame(45);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);

    // Frame 60: clip end, held at full alpha, t = 1 - 0/15 = 1.0.
    seekToFrame(60);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 1.0f, 1e-5f);

    // Frame 65: mid fade-out, t = 1 - 5/15 = 10/15.
    seekToFrame(65);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 10.0f / 15.0f, 1e-5f);

    // Frame 74: late in tail, t = 1 - 14/15 = 1/15.
    seekToFrame(74);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 1.0f / 15.0f, 1e-5f);
}

// 6b. Both ends aligned, short clip: fade-in still constrains the in-clip
// span; fade-out window now sits entirely past clipEnd, so the only window
// open mid-clip is the fade-in.
TEST_F(SectionFadeTest, BothEndsAligned_ShortClip_FadeInDominates) {
    // Clip 10 frames, fade 1.0s each end (30 frames each).
    // Fade-in window: [30, 60). Fade-out window: [40, 70).
    // Inside the clip [30, 40) only fade-in is open; the fade-out window
    // overlaps the post-end held tail.
    Clip c;
    fillClip(c, /*start*/30, /*duration*/10);

    timeline.addSectionBreak(toTimelineMicros(30, 30.0), 0xFF6090C8, 1.0);
    timeline.addSectionBreak(toTimelineMicros(40, 30.0), 0xFF7060C8, 1.0);

    // At frame 35: fade-in t = 5/30. Fade-out not yet open.
    seekToFrame(35);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 5.0f / 30.0f, 1e-5f);

    // At frame 31: fade-in t = 1/30. Fade-out not yet open.
    seekToFrame(31);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 1.0f / 30.0f, 1e-5f);

    // At frame 40 (clipEnd, inside both fade-in's window [30,60) and the
    // start of fade-out [40,70)): fade-in t = 10/30, fade-out t = 1.0
    // (held at start of tail). min = 10/30.
    seekToFrame(40);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 10.0f / 30.0f, 1e-5f);

    // At frame 55: past fade-in window (closed at 60? no, fade-in window
    // is [30, 60) so 55 is inside it: t = 25/30). Fade-out open at 55:
    // t = 1 - 15/30 = 15/30. min = 15/30.
    seekToFrame(55);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 15.0f / 30.0f, 1e-5f);
}

// 7. Outside any fade window mid-clip -> 1.0.
TEST_F(SectionFadeTest, OutsideFadeWindow_ReturnsOne) {
    Clip c;
    fillClip(c, /*start*/30, /*duration*/120);

    timeline.addSectionBreak(toTimelineMicros(30, 30.0), 0xFF6090C8, 0.5);   // fade-in window [30, 45)
    timeline.addSectionBreak(toTimelineMicros(150, 30.0), 0xFF7060C8, 0.5);  // fade-out window [150, 165)

    // Frame 75 is well past fade-in close (45) and well before fade-out
    // open (150). Result must be identity.
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
