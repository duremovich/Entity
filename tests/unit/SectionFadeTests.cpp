// Phase D — section fade envelope unit tests. Exercises
// PlaybackTimeAuthority::computeSectionFadeMultiplier directly, against a
// Timeline with section breaks set up via Timeline::addSectionBreak.
//
// Section breaks are frame-native: addSectionBreak takes an integer timeline
// frame. The helper is pure — given the timeline's current frame plus the
// clip's start/end, it scans the section vector for breaks aligned exactly
// with either edge and returns the ramp value. The cases here cover:
//   1. No aligned breaks      -> 1.0
//   2. fadeSeconds == 0       -> 1.0 (break exists but is inert)
//   3. Fade-in mid-ramp       -> proportional ramp
//   4. Fade-out mid-ramp      -> proportional ramp
//   5. Alignment is exact     -> clip start ON the break aligns; off does not
//   6. Both ends aligned      -> min-combine of both ramps
//   7. Outside fade window    -> 1.0 (clip plays normally between fades)
//   8. Empty section list     -> 1.0
//   9. At-break visibility gate -> a clip starting on a parked break is hidden

#include <gtest/gtest.h>

#include "entity/components/Clip.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/timeline/Timeline.hpp"

#include <entt/entt.hpp>

using namespace entity;

namespace {

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
    timeline.addSectionBreak(120, 0xFF6090C8, 1.0);

    seekToFrame(30);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 2. Aligned break with fadeSeconds == 0 -> inert (no envelope).
TEST_F(SectionFadeTest, ZeroFadeSeconds_NoEnvelope) {
    Clip c;
    fillClip(c, /*start*/30, /*duration*/60);

    // Break exactly at clip start, but fadeSeconds = 0.
    timeline.addSectionBreak(30, 0xFF6090C8, 0.0);

    seekToFrame(30);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// 3. Fade-in mid-ramp.
TEST_F(SectionFadeTest, FadeIn_MidRamp_ProportionalValue) {
    Clip c;
    fillClip(c, /*start*/30, /*duration*/120);

    // Break at clip's start, 1.0 second fade -> 30 frames at 30fps.
    timeline.addSectionBreak(30, 0xFF6090C8, 1.0);

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
    timeline.addSectionBreak(60, 0xFF6090C8, 0.5);

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

// 5a. Exact alignment: clip start sits exactly on the break -> envelope.
// Frame-native section storage means alignment is an exact integer match;
// the pre-migration ±1 snap tolerance is gone.
TEST_F(SectionFadeTest, ExactStartAlignment_GetsEnvelope) {
    Clip c;
    fillClip(c, /*start*/30, /*duration*/120);
    timeline.addSectionBreak(30, 0xFF6090C8, 1.0);

    seekToFrame(30);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 0.0f, 1e-5f);

    seekToFrame(45);
    EXPECT_NEAR(auth.computeSectionFadeMultiplier(c), 15.0f / 30.0f, 1e-5f);
}

// 5b. One frame off the break does NOT align -> no envelope.
TEST_F(SectionFadeTest, OneFrameOff_NoEnvelope) {
    Clip c;
    // Clip starts at frame 31, break at frame 30 -> not exactly aligned.
    fillClip(c, /*start*/31, /*duration*/120);
    timeline.addSectionBreak(30, 0xFF6090C8, 1.0);

    seekToFrame(31);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);

    seekToFrame(46);
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

    timeline.addSectionBreak(30, 0xFF6090C8, 0.5);
    timeline.addSectionBreak(60, 0xFF7060C8, 0.5);

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

    timeline.addSectionBreak(30, 0xFF6090C8, 1.0);
    timeline.addSectionBreak(40, 0xFF7060C8, 1.0);

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

    timeline.addSectionBreak(30, 0xFF6090C8, 0.5);    // fade-in window [30, 45)
    timeline.addSectionBreak(150, 0xFF7060C8, 0.5);   // fade-out window [150, 165)

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

// 9. At-break visibility gate. A clip whose startFrame coincides with a
// section break must be hidden (multiplier 0) while the playhead is parked
// at that break — "things after the break shouldn't start until GO".
// Regression for the microsecond-storage truncation bug: frame 250 round-
// trips through frameToTime as 8333333us, which a truncating us->frame
// conversion read back as 249, so the gate's exact breakFrame==currentFrame
// test missed and the clip rendered at full opacity.
TEST_F(SectionFadeTest, AtBreakGate_ClipStartsOnBreakFrame_HiddenUntilGo) {
    Clip c;
    fillClip(c, /*start*/250, /*duration*/150);

    timeline.addSectionBreak(250, 0xFF6090C8, 0.0);

    seekToFrame(250);
    timeline.setSectionAtBreak(true);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 0.0f);
}

// 9b. Same gate at another truncation-prone frame — proves the bug was a
// systematic conversion fault, not specific to frame 250.
TEST_F(SectionFadeTest, AtBreakGate_ClipStartsOnBreakFrame_SecondFrame) {
    Clip c;
    fillClip(c, /*start*/7, /*duration*/120);

    timeline.addSectionBreak(7, 0xFF6090C8, 0.0);

    seekToFrame(7);
    timeline.setSectionAtBreak(true);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 0.0f);
}

// 9c. A clip one frame past a parked break is NOT gated — it is a distinct
// clip, not "the clip that starts on the break".
TEST_F(SectionFadeTest, AtBreakGate_ClipOneFrameAfterBreak_NotGated) {
    Clip c;
    fillClip(c, /*start*/251, /*duration*/150);

    timeline.addSectionBreak(250, 0xFF6090C8, 0.0);

    seekToFrame(250);
    timeline.setSectionAtBreak(true);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(c), 1.0f);
}

// --- Layer-agnostic (start, end) overload ---------------------------------
//
// The overload exists so generative layers (Text, Muncher) honor section
// fades the same way clips do. The Clip overload is a trampoline onto this,
// so the entire SectionFadeTest suite above also covers it. These cases
// focus on the things the generative path cares about:
//   - parity with the Clip overload for equivalent inputs
//   - trailing-edge hold at break with fadeSeconds == 0 (Fix 1 symmetry)
//   - at-break visibility gate fires for leading-edge layers

// 10. Parity: the (start, end) overload matches the Clip trampoline for
// matching inputs across fade-in, fade-out, and identity cases.
TEST_F(SectionFadeTest, Overload_MatchesClipTrampolineAcrossWindow) {
    Clip c;
    fillClip(c, /*start*/30, /*duration*/30);
    const FrameNumber start = c.startFrame;
    const FrameNumber end   = c.startFrame + c.duration;

    timeline.addSectionBreak(30, 0xFF6090C8, 0.5);  // fade-in [30, 45)
    timeline.addSectionBreak(60, 0xFF7060C8, 0.5);  // fade-out [60, 75)

    for (FrameNumber f : {15, 30, 37, 45, 59, 60, 65, 74, 80}) {
        seekToFrame(f);
        const float viaClip   = auth.computeSectionFadeMultiplier(c);
        const float viaParams = auth.computeSectionFadeMultiplier(start, end);
        EXPECT_FLOAT_EQ(viaClip, viaParams) << "frame=" << f;
    }
}

// 11. Trailing-edge generative with fadeSeconds == 0 — the Fix 1 symmetry
// case applied to the layer overload. The multiplier at the break frame
// is 1.0 because the fade-out branch is guarded on fadeSeconds > 0.
// Combined with the snapshot-filter extension in buildSceneSnapshot (which
// uses sectionFadeTailFrames, ≥ 1 for break-aligned ends), trailing-edge
// generatives now hold visible during the at-break park.
TEST_F(SectionFadeTest, Overload_TrailingEdge_HoldsAtBreakWithZeroFade) {
    timeline.addSectionBreak(60, 0xFF6090C8, /*fadeSeconds*/0.0);
    seekToFrame(60);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(/*start*/0, /*end*/60),
                    1.0f);
}

// 12. Leading-edge generative at a parked break is gated to 0 — same as
// the clip gate at SectionFadeTest.AtBreakGate_ClipStartsOnBreakFrame above.
// This is what makes a Text layer queued at a break wait invisible until GO
// instead of popping on.
TEST_F(SectionFadeTest, Overload_LeadingEdge_AtBreakGateHidesUntilGo) {
    timeline.addSectionBreak(250, 0xFF6090C8, 0.0);
    seekToFrame(250);
    timeline.setSectionAtBreak(true);
    EXPECT_FLOAT_EQ(auth.computeSectionFadeMultiplier(/*start*/250, /*end*/400),
                    0.0f);
}
