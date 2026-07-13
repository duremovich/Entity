#include <gtest/gtest.h>
#include "entity/components/AnimatedProperties.hpp"

#include <cmath>

using namespace entity;

// Regression gate for "changing a keyframe's value reverts it from Easy Ease to
// Linear". KeyframeTrack::addKeyframe doubles as the update path for an existing
// keyframe, and it used to overwrite `interpolation` with the caller's default
// (Linear) on every value edit. Easing must now survive a value-only write, and
// only an explicit interpolation argument may overwrite it.
namespace {

KeyframeTrack makeEasedTrack(FrameNumber frame, float value) {
    KeyframeTrack track;
    track.property = AnimatableProperty::Opacity;
    track.addKeyframe(frame, value, InterpolationType::EaseInOut);
    Keyframe* kf = track.getKeyframeAt(frame);
    EXPECT_NE(kf, nullptr);
    kf->easeIn  = 0.25f;   // deliberately NOT the 0.42/0.58 defaults, so a
    kf->easeOut = 0.75f;   // silent reset-to-default is detectable
    return track;
}

}  // namespace

TEST(KeyframeEasing, ValueEditPreservesInterpolationAndHandles) {
    KeyframeTrack track = makeEasedTrack(100, 0.5f);

    track.addKeyframe(100, 0.9f);  // value-only edit — the UI's drag path

    const Keyframe* kf = track.getKeyframeAt(100);
    ASSERT_NE(kf, nullptr);
    EXPECT_EQ(track.keyframes.size(), 1u);
    EXPECT_FLOAT_EQ(kf->value, 0.9f);
    EXPECT_EQ(kf->interpolation, InterpolationType::EaseInOut);
    EXPECT_FLOAT_EQ(kf->easeIn, 0.25f);
    EXPECT_FLOAT_EQ(kf->easeOut, 0.75f);
}

TEST(KeyframeEasing, ExplicitInterpolationStillOverwrites) {
    KeyframeTrack track = makeEasedTrack(100, 0.5f);

    track.addKeyframe(100, 0.9f, InterpolationType::Step);

    const Keyframe* kf = track.getKeyframeAt(100);
    ASSERT_NE(kf, nullptr);
    EXPECT_EQ(kf->interpolation, InterpolationType::Step);
}

TEST(KeyframeEasing, NewKeyframeDefaultsToLinear) {
    KeyframeTrack track;
    track.property = AnimatableProperty::Opacity;
    track.addKeyframe(10, 1.0f);

    const Keyframe* kf = track.getKeyframeAt(10);
    ASSERT_NE(kf, nullptr);
    EXPECT_EQ(kf->interpolation, InterpolationType::Linear);
}

// cubicBezier used to ignore its easeIn/easeOut arguments entirely and always
// evaluate to smoothstep (3t^2 - 2t^3), which made the bezier-tangent sliders in
// the keyframe context menu decorative. These pin the corrected behavior.
TEST(KeyframeEasing, CubicBezierHonorsControlPoints) {
    // A curve biased hard toward a slow start must sit BELOW the linear ramp at
    // the midpoint; one biased toward a fast start must sit above it.
    const float slowStart = KeyframeTrack::cubicBezier(0.5f, 0.9f, 1.0f);
    const float fastStart = KeyframeTrack::cubicBezier(0.5f, 0.0f, 0.1f);

    EXPECT_LT(slowStart, 0.5f);
    EXPECT_GT(fastStart, 0.5f);
    EXPECT_GT(fastStart, slowStart);

    // Distinct handles must produce distinct curves — the old implementation
    // returned the same value regardless.
    EXPECT_NE(slowStart, fastStart);
}

TEST(KeyframeEasing, CubicBezierPinsEndpointsAndIsMonotonic) {
    struct Handles { float in, out; };
    const Handles cases[] = {
        {0.42f, 0.58f},  // AE Easy Ease default
        {0.0f,  1.0f},   // degenerate: derivative vanishes at both ends
        {0.0f,  0.0f},   // degenerate: Newton stalls, bisection must carry it
        {1.0f,  1.0f},
        {0.25f, 0.75f},
    };

    for (const auto& h : cases) {
        EXPECT_NEAR(KeyframeTrack::cubicBezier(0.0f, h.in, h.out), 0.0f, 1e-3f);
        EXPECT_NEAR(KeyframeTrack::cubicBezier(1.0f, h.in, h.out), 1.0f, 1e-3f);

        float prev = -1.0f;
        for (int i = 0; i <= 50; ++i) {
            const float x = static_cast<float>(i) / 50.0f;
            const float y = KeyframeTrack::cubicBezier(x, h.in, h.out);
            EXPECT_GE(y, prev - 1e-3f)
                << "non-monotonic at x=" << x << " handles=" << h.in << "/" << h.out;
            EXPECT_GE(y, -1e-3f);
            EXPECT_LE(y, 1.0f + 1e-3f);
            prev = y;
        }
    }
}

// The eased value must actually be routed through the handles by interpolate(),
// not just be reachable via cubicBezier() directly.
TEST(KeyframeEasing, InterpolateUsesIncomingKeyframeHandles) {
    Keyframe a;
    a.frame = 0;
    a.value = 0.0f;
    a.interpolation = InterpolationType::Linear;

    Keyframe b;
    b.frame = 10;
    b.value = 1.0f;
    b.interpolation = InterpolationType::EaseInOut;

    b.easeIn = 0.9f; b.easeOut = 1.0f;
    const float slow = KeyframeTrack::interpolate(a, b, 0.5f);

    b.easeIn = 0.0f; b.easeOut = 0.1f;
    const float fast = KeyframeTrack::interpolate(a, b, 0.5f);

    EXPECT_GT(fast, slow);
}
