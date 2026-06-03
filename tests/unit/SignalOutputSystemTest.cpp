// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
#include <gtest/gtest.h>

#include "entity/bus/Message.hpp"
#include "entity/components/AnimatedProperties.hpp"  // AnimatableProperty::SignalValue
#include "entity/systems/SignalOutputSystem.hpp"

using namespace entity;

namespace {

bus::SignalLayerSnapshot makeMomentary(std::uint64_t entity,
                                        FrameNumber start, FrameNumber duration = 60,
                                        const std::string& address = "/test") {
    bus::SignalLayerSnapshot sl;
    sl.entity     = entity;
    sl.startFrame = start;
    sl.duration   = duration;
    sl.mode       = 0; // Momentary
    sl.transport  = 0;
    sl.valueSource = 0;
    sl.address    = address;
    sl.srcMin = 0.0f; sl.srcMax = 1.0f;
    sl.outMin = 0.0f; sl.outMax = 1.0f;
    sl.outType = static_cast<int>(bus::SignalArg::Type::Float);
    sl.valueBoundArgIndex = -1;
    return sl;
}

bus::SignalLayerSnapshot makeContinuous(std::uint64_t entity,
                                         FrameNumber start, FrameNumber duration,
                                         const std::string& address = "/test") {
    bus::SignalLayerSnapshot sl;
    sl.entity     = entity;
    sl.startFrame = start;
    sl.duration   = duration;
    sl.mode       = 1; // Continuous
    sl.transport  = 0;
    sl.valueSource = 0;
    sl.address    = address;
    sl.srcMin = 0.0f; sl.srcMax = 1.0f;
    sl.outMin = 0.0f; sl.outMax = 1.0f;
    sl.outType = static_cast<int>(bus::SignalArg::Type::Float);
    sl.valueBoundArgIndex = -1;
    return sl;
}

} // namespace

// ===========================================================================
// MOMENTARY
// ===========================================================================

TEST(SignalOutputSystem, MomentaryFiresOnceOnForwardCross) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    auto sl = makeMomentary(1u, 100);
    // prev=99, cur=100: forward crossing of startFrame=100.
    sys.evaluate({sl}, 99, 100, true, false, emits);
    EXPECT_EQ(emits.size(), 1u) << "forward crossing must fire once";
    emits.clear();

    // Next tick still past start: must NOT re-fire.
    sys.evaluate({sl}, 100, 101, true, false, emits);
    EXPECT_TRUE(emits.empty()) << "no re-fire while past startFrame";
}

TEST(SignalOutputSystem, MomentaryNoFireOnScrub) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    auto sl = makeMomentary(2u, 50);
    // Move playhead over startFrame while paused (playing=false).
    sys.evaluate({sl}, 49, 50, false, false, emits);
    EXPECT_TRUE(emits.empty()) << "scrub (playing=false) must not fire";
    sys.evaluate({sl}, 50, 51, false, false, emits);
    EXPECT_TRUE(emits.empty());
}

TEST(SignalOutputSystem, MomentaryRearmsAfterRewind) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    auto sl = makeMomentary(3u, 100);

    // First forward crossing: fires.
    sys.evaluate({sl}, 99, 100, true, false, emits);
    EXPECT_EQ(emits.size(), 1u) << "first crossing must fire";
    emits.clear();

    // Rewind to before start: re-arms (curFrame < startFrame).
    sys.evaluate({sl}, 100, 80, true, false, emits);
    EXPECT_TRUE(emits.empty()) << "rewind itself must not emit";

    // Second forward crossing: fires again.
    sys.evaluate({sl}, 99, 100, true, false, emits);
    EXPECT_EQ(emits.size(), 1u) << "crossing after re-arm must fire again";
}

TEST(SignalOutputSystem, MomentaryAtBreakWaitsForResume) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    auto sl = makeMomentary(4u, 200);

    // Parked at break: atBreak=true -> suppress, do NOT consume the arm.
    sys.evaluate({sl}, 199, 200, true, true, emits);
    EXPECT_TRUE(emits.empty()) << "atBreak must suppress fire";
    sys.evaluate({sl}, 200, 200, true, true, emits);
    EXPECT_TRUE(emits.empty());

    // Rewind to before start (e.g. GO resumes from earlier).
    sys.evaluate({sl}, 200, 150, true, false, emits);
    EXPECT_TRUE(emits.empty()) << "rewind must not fire";

    // Real forward crossing with atBreak=false: fires exactly once (==1u).
    sys.evaluate({sl}, 199, 200, true, false, emits);
    EXPECT_EQ(emits.size(), 1u) << "first real crossing after resume must fire exactly once";
    emits.clear();

    // No re-fire next tick.
    sys.evaluate({sl}, 200, 201, true, false, emits);
    EXPECT_TRUE(emits.empty());
}

// Multi-frame skip: prev=start-1, cur=start+2 still fires exactly once.
// This is the case the Phase 4 ±2 bake window enables (layer is in the
// snapshot even when curFrame skips past startFrame in one tick).
TEST(SignalOutputSystem, MomentaryMultiFrameForwardCross) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    auto sl = makeMomentary(5u, 100);
    // Headless loop runs at thousands of fps; single tick can skip many frames.
    sys.evaluate({sl}, 99, 102, true, false, emits);
    EXPECT_EQ(emits.size(), 1u) << "multi-frame skip must still fire exactly once";
}

// Backward entry: prev > cur across start -> no fire (forward-only detector).
TEST(SignalOutputSystem, MomentaryNoFireOnBackwardEntry) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    auto sl = makeMomentary(6u, 100);
    // Backward: prev=105, cur=98. prevFrame < startFrame is FALSE -> no crossing.
    sys.evaluate({sl}, 105, 98, true, false, emits);
    EXPECT_TRUE(emits.empty()) << "backward motion across startFrame must not fire";
}

// ===========================================================================
// CONTINUOUS
// ===========================================================================

TEST(SignalOutputSystem, ContinuousEmitsEveryTick) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    auto sl = makeContinuous(10u, 0, 60, "/cont");

    sys.evaluate({sl}, 0, 10, true, false, emits);
    ASSERT_EQ(emits.size(), 1u);
    EXPECT_EQ(emits[0].address, "/cont");
    emits.clear();

    sys.evaluate({sl}, 10, 20, true, false, emits);
    EXPECT_EQ(emits.size(), 1u);
    emits.clear();

    // Rate-limit: same curFrame must not double-emit.
    sys.evaluate({sl}, 20, 20, true, false, emits);
    EXPECT_TRUE(emits.empty()) << "rate-limit: same curFrame must not re-emit";
}

TEST(SignalOutputSystem, ContinuousNoStreamWhilePaused) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    auto sl = makeContinuous(11u, 0, 60);
    sys.evaluate({sl}, 0, 30, false, false, emits);
    EXPECT_TRUE(emits.empty()) << "continuous must not stream while paused";
}

TEST(SignalOutputSystem, ContinuousAtBreakSuppresses) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    auto sl = makeContinuous(12u, 0, 60);
    sys.evaluate({sl}, 0, 30, true, true, emits);
    EXPECT_TRUE(emits.empty()) << "continuous must not emit at section break";
}

// Progress: frame 50/100 -> 0.5 -> [0,255] -> 127 (Int truncation).
TEST(SignalOutputSystem, ContinuousProgressMapsAndStreams) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    bus::SignalLayerSnapshot sl = makeContinuous(13u, 0, 100);
    sl.valueBoundArgIndex = 0;
    sl.outMin = 0.0f; sl.outMax = 255.0f;
    sl.outType = static_cast<int>(bus::SignalArg::Type::Int);
    bus::SignalArg a; a.type = bus::SignalArg::Type::Int;
    sl.args.push_back(a);

    // progress 0.5 -> 0.5 * 255 = 127.5 -> truncate to 127.
    sys.evaluate({sl}, 49, 50, true, false, emits);
    ASSERT_EQ(emits.size(), 1u);
    ASSERT_EQ(emits[0].args.size(), 1u);
    EXPECT_EQ(emits[0].args[0].type, bus::SignalArg::Type::Int);
    EXPECT_EQ(emits[0].args[0].i, 127);
}

// Keyframe linear interp: local frame 25 between kf0=0.0 and kf50=1.0 -> 0.5.
TEST(SignalOutputSystem, ContinuousKeyframeLinearInterp) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    const int signalValueProp = static_cast<int>(AnimatableProperty::SignalValue);

    bus::SignalLayerSnapshot sl = makeContinuous(14u, 10, 100);
    sl.valueSource = 1; // Keyframe
    sl.valueBoundArgIndex = 0;
    bus::SignalArg a; a.type = bus::SignalArg::Type::Float;
    sl.args.push_back(a);

    bus::BakedTrack track;
    track.property = signalValueProp;
    track.enabled  = true;
    bus::BakedKeyframe k0; k0.frame = 0;  k0.value = 0.0f;
    bus::BakedKeyframe k1; k1.frame = 50; k1.value = 1.0f;
    track.keyframes = {k0, k1};
    sl.tracks.push_back(track);

    // Timeline frame 35 -> local 25 -> midpoint -> 0.5.
    sys.evaluate({sl}, 34, 35, true, false, emits);
    ASSERT_EQ(emits.size(), 1u);
    ASSERT_EQ(emits[0].args.size(), 1u);
    EXPECT_NEAR(emits[0].args[0].f, 0.5f, 0.001f);
}

// Literal args pass through unmodified on a momentary layer.
TEST(SignalOutputSystem, LiteralArgsPassThrough) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    auto sl = makeMomentary(15u, 0, 60, "/cue");
    bus::SignalArg ai; ai.type = bus::SignalArg::Type::Int;    ai.i = 42;
    bus::SignalArg af; af.type = bus::SignalArg::Type::Float;  af.f = 3.14f;
    bus::SignalArg as; as.type = bus::SignalArg::Type::String; as.s = "hello";
    sl.args = {ai, af, as};

    sys.evaluate({sl}, -1, 0, true, false, emits);
    ASSERT_EQ(emits.size(), 1u);
    ASSERT_EQ(emits[0].args.size(), 3u);
    EXPECT_EQ(emits[0].args[0].i, 42);
    EXPECT_NEAR(emits[0].args[1].f, 3.14f, 0.001f);
    EXPECT_EQ(emits[0].args[2].s, "hello");
}

// Multiple bound args: FIRST (valueBoundArgIndex=0) wins; arg 1 stays literal.
TEST(SignalOutputSystem, MultipleBoundArgFirstWins) {
    SignalOutputSystem sys;
    std::vector<bus::SignalEmit> emits;

    bus::SignalLayerSnapshot sl = makeContinuous(16u, 0, 100);
    sl.valueBoundArgIndex = 0;
    sl.outMin = 0.0f; sl.outMax = 1.0f;
    sl.outType = static_cast<int>(bus::SignalArg::Type::Float);

    bus::SignalArg a0; a0.type = bus::SignalArg::Type::Float; a0.f = 999.0f;
    bus::SignalArg a1; a1.type = bus::SignalArg::Type::Float; a1.f = 7.0f;
    sl.args = {a0, a1};

    // progress at frame 50/100 = 0.5 -> mapped to 0.5.
    sys.evaluate({sl}, 49, 50, true, false, emits);
    ASSERT_EQ(emits.size(), 1u);
    ASSERT_EQ(emits[0].args.size(), 2u);
    EXPECT_NEAR(emits[0].args[0].f, 0.5f, 0.001f)
        << "index 0 (valueBoundArgIndex) must carry mapped value";
    EXPECT_NEAR(emits[0].args[1].f, 7.0f, 0.001f)
        << "index 1 must pass through as literal";
}
