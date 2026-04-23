#include <gtest/gtest.h>
#include "entity/timeline/Timeline.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <entt/entt.hpp>

using namespace entity;

// Regression gate for the "edit at keyframe inserts a new nearby keyframe"
// bug. Timeline::seekToFrame(N) must round-trip cleanly through
// getCurrentFrame() so that PropertyWindow's slider commit lands on the same
// frame number the keyframe was originally stored at, even for fps ratios
// that produce non-integer microseconds-per-frame.
class KeyframeRoundtripTest : public ::testing::TestWithParam<std::pair<double, double>> {};

TEST_P(KeyframeRoundtripTest, SeekToFrameRoundTrips) {
    auto [timelineFps, clipFps] = GetParam();
    (void)clipFps;  // clip fps is unused for the round-trip itself

    entt::registry registry;
    Timeline timeline(registry);
    timeline.setFrameRate(timelineFps);

    for (FrameNumber f : {0LL, 1LL, 2LL, 7LL, 100LL, 999LL, 12345LL}) {
        timeline.seekToFrame(f);
        EXPECT_EQ(timeline.getCurrentFrame(), f)
            << "Round-trip failed for fps=" << timelineFps << " frame=" << f;
    }
}

TEST_P(KeyframeRoundtripTest, EditingAtExistingKeyframeUpdatesInPlace) {
    auto [timelineFps, clipFps] = GetParam();
    (void)clipFps;

    entt::registry registry;
    Timeline timeline(registry);
    timeline.setFrameRate(timelineFps);

    const FrameNumber clipStart = 47;     // non-zero offset to catch sign/space mistakes
    const FrameNumber kfClipFrame = 100;  // clip-local

    KeyframeTrack track;
    track.property = AnimatableProperty::Opacity;
    track.addKeyframe(kfClipFrame, 0.5f);
    ASSERT_EQ(track.keyframes.size(), 1u);

    // Simulate "user clicks next-keyframe arrow" -> seek to absolute frame.
    timeline.seekToFrame(clipStart + kfClipFrame);

    // Simulate "user drags the slider" -> PropertyWindow recomputes clip-local
    // frame the same way getCurrentClipFrame() does after the fix, then writes
    // the new value through addKeyframe. Pre-fix this could land on
    // kfClipFrame - 1 due to truncation drift.
    FrameNumber observedClipFrame = timeline.getCurrentFrame() - clipStart;
    EXPECT_EQ(observedClipFrame, kfClipFrame);

    track.addKeyframe(observedClipFrame, 0.75f);

    EXPECT_EQ(track.keyframes.size(), 1u)
        << "Keyframe count grew at fps=" << timelineFps
        << " — slider edit at existing keyframe inserted a new one instead of updating.";
    EXPECT_FLOAT_EQ(track.keyframes[0].value, 0.75f);
    EXPECT_EQ(track.keyframes[0].frame, kfClipFrame);
}

INSTANTIATE_TEST_SUITE_P(
    AcrossFrameRates,
    KeyframeRoundtripTest,
    ::testing::Values(
        std::make_pair(30.0, 30.0),       // baseline
        std::make_pair(60.0, 30.0),       // mixed-fps, integer ratio
        std::make_pair(30.0, 23.976),     // film-on-tv, non-integer source
        std::make_pair(23.976, 30.0),     // tv-on-film
        std::make_pair(29.97, 60.0)       // NTSC drop-frame timeline
    )
);
