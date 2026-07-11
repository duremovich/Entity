#include <gtest/gtest.h>

#include "entity/director/CatalogClipMath.hpp"
#include "entity/precomp/PrecompLibrary.hpp"
#include "entity/timeline/PlaybackWrap.hpp"
#include "entity/timeline/PrecompMath.hpp"
#include "entity/timeline/Timeline.hpp"

#include <entt/entt.hpp>

#include <cstdint>
#include <string>

using namespace entity;

// ---------------------------------------------------------------------------
// Precomp Phase A math tests (issue #101, ADR-0029 Decision 3).
//
// Part 1 pins the shared wrapLocalFrame primitive, including a parity sweep
// against mapToMediaFrameFromCatalogEx — the guard that the four-site wrap
// migration preserved behavior. Part 2 pins mapOuterToInnerFrame: window,
// speed, trim, fps mismatch, and the ADR edge cases.
// ---------------------------------------------------------------------------

namespace {

constexpr FrameNumber kLen = 50;

// Baseline instance: outer window [100, 160), no trim, 50-frame definition,
// definition fps == outer fps == 30, speed 1.
PrecompInstanceParams makeParams(PlaybackMode mode,
                                 FrameNumber start = 100,
                                 FrameNumber duration = 60,
                                 FrameNumber innerStart = 0,
                                 FrameNumber defDuration = 50,
                                 double defFps = 30.0,
                                 double speed = 1.0) {
    PrecompInstanceParams p;
    p.instanceStartFrame  = start;
    p.instanceDuration    = duration;
    p.innerStartFrame     = innerStart;
    p.definitionDuration  = defDuration;
    p.definitionFrameRate = defFps;
    p.speed               = speed;
    p.playbackMode        = mode;
    return p;
}

constexpr double kOuterFps = 30.0;

// Catalog entry over source frames [10, 59] (sourceLength = 50), clip fps ==
// timeline fps == 30, duration 200 so realEnd never clamps in the sweep range.
bus::ClipCatalogEntry makeSweepEntry(PlaybackMode mode) {
    bus::ClipCatalogEntry e;
    e.entity          = 1;
    e.startFrame      = 0;
    e.duration        = 200;
    e.mediaStartFrame = 10;
    e.mediaOutFrame   = 59;
    e.framerate       = 30.0;
    e.playbackMode    = static_cast<int>(mode);
    e.sectionBehavior = static_cast<int>(SectionBehavior::Normal);
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
// wrapLocalFrame primitive
// ---------------------------------------------------------------------------

TEST(PlaybackWrap, PassthroughBelowLength) {
    for (auto mode : {PlaybackMode::Freeze, PlaybackMode::Loop,
                      PlaybackMode::PingPong}) {
        const auto first = wrapLocalFrame(mode, kLen, 0);
        EXPECT_EQ(first.frame, 0);
        EXPECT_FALSE(first.reverse);
        const auto last = wrapLocalFrame(mode, kLen, kLen - 1);
        EXPECT_EQ(last.frame, kLen - 1);
        EXPECT_FALSE(last.reverse);
    }
}

TEST(PlaybackWrap, FreezeClampsPastEnd) {
    const auto atEnd = wrapLocalFrame(PlaybackMode::Freeze, kLen, kLen);
    EXPECT_EQ(atEnd.frame, kLen - 1);
    EXPECT_FALSE(atEnd.reverse);
    const auto far = wrapLocalFrame(PlaybackMode::Freeze, kLen, 3 * kLen);
    EXPECT_EQ(far.frame, kLen - 1);
    EXPECT_FALSE(far.reverse);
}

TEST(PlaybackWrap, LoopModulo) {
    EXPECT_EQ(wrapLocalFrame(PlaybackMode::Loop, kLen, kLen).frame, 0);
    EXPECT_EQ(wrapLocalFrame(PlaybackMode::Loop, kLen, kLen + 7).frame, 7);
    EXPECT_EQ(wrapLocalFrame(PlaybackMode::Loop, kLen, 2 * kLen).frame, 0);
    EXPECT_FALSE(wrapLocalFrame(PlaybackMode::Loop, kLen, kLen + 7).reverse);
}

TEST(PlaybackWrap, PingPongParityAndReflection) {
    // Cycle 0: forward.
    const auto fwd = wrapLocalFrame(PlaybackMode::PingPong, kLen, 30);
    EXPECT_EQ(fwd.frame, 30);
    EXPECT_FALSE(fwd.reverse);
    // Reflection boundary: first frame of cycle 1 is the last source frame.
    const auto boundary = wrapLocalFrame(PlaybackMode::PingPong, kLen, kLen);
    EXPECT_EQ(boundary.frame, kLen - 1);
    EXPECT_TRUE(boundary.reverse);
    // Cycle 1: reverse. pos 10 -> len - 1 - 10.
    const auto rev = wrapLocalFrame(PlaybackMode::PingPong, kLen, kLen + 10);
    EXPECT_EQ(rev.frame, kLen - 1 - 10);
    EXPECT_TRUE(rev.reverse);
    // Cycle 2: forward again.
    const auto fwd2 = wrapLocalFrame(PlaybackMode::PingPong, kLen, 2 * kLen + 10);
    EXPECT_EQ(fwd2.frame, 10);
    EXPECT_FALSE(fwd2.reverse);
}

TEST(PlaybackWrap, ZeroAndNegativeLengthDefensive) {
    for (auto mode : {PlaybackMode::Freeze, PlaybackMode::Loop,
                      PlaybackMode::PingPong}) {
        const auto zero = wrapLocalFrame(mode, 0, 25);
        EXPECT_EQ(zero.frame, 0);
        EXPECT_FALSE(zero.reverse);
        const auto neg = wrapLocalFrame(mode, -5, 25);
        EXPECT_EQ(neg.frame, 0);
        EXPECT_FALSE(neg.reverse);
    }
}

// The migration guard: the catalog math (one of the four migrated sites)
// must agree with the primitive frame-for-frame across all three modes.
TEST(PlaybackWrap, ParityWithCatalogClipMath) {
    for (auto mode : {PlaybackMode::Freeze, PlaybackMode::Loop,
                      PlaybackMode::PingPong}) {
        const auto e = makeSweepEntry(mode);
        for (FrameNumber f = 0; f < 150; ++f) {
            const auto catalog = mapToMediaFrameFromCatalogEx(e, f, 30.0, 0);
            const auto w = wrapLocalFrame(mode, kLen, f);
            EXPECT_EQ(catalog.mediaFrame, 10 + w.frame)
                << "mode=" << static_cast<int>(mode) << " frame=" << f;
            EXPECT_EQ(catalog.pingPongReverse, w.reverse)
                << "mode=" << static_cast<int>(mode) << " frame=" << f;
        }
    }
}

// ---------------------------------------------------------------------------
// mapOuterToInnerFrame
// ---------------------------------------------------------------------------

TEST(PrecompMath, WindowBoundaries) {
    const auto p = makeParams(PlaybackMode::Freeze);
    EXPECT_FALSE(mapOuterToInnerFrame(p, 99, kOuterFps).active);
    const auto first = mapOuterToInnerFrame(p, 100, kOuterFps);
    EXPECT_TRUE(first.active);
    EXPECT_EQ(first.innerFrame, 0);
    EXPECT_TRUE(mapOuterToInnerFrame(p, 159, kOuterFps).active);
    EXPECT_FALSE(mapOuterToInnerFrame(p, 160, kOuterFps).active);
}

TEST(PrecompMath, IdentityMappingSpeed1) {
    const auto p = makeParams(PlaybackMode::Freeze);
    for (FrameNumber k = 0; k < 50; ++k) {
        const auto r = mapOuterToInnerFrame(p, 100 + k, kOuterFps);
        EXPECT_TRUE(r.active);
        EXPECT_EQ(r.innerFrame, k) << "k=" << k;
        EXPECT_FALSE(r.pingPongReverse);
    }
}

TEST(PrecompMath, SpeedHalf) {
    const auto p = makeParams(PlaybackMode::Freeze, 100, 60, 0, 50, 30.0, 0.5);
    // floor(k * 0.5): odd k floors down.
    EXPECT_EQ(mapOuterToInnerFrame(p, 100 + 4, kOuterFps).innerFrame, 2);
    EXPECT_EQ(mapOuterToInnerFrame(p, 100 + 5, kOuterFps).innerFrame, 2);
    EXPECT_EQ(mapOuterToInnerFrame(p, 100 + 6, kOuterFps).innerFrame, 3);
}

TEST(PrecompMath, SpeedDouble) {
    const auto p = makeParams(PlaybackMode::Loop, 100, 60, 0, 50, 30.0, 2.0);
    EXPECT_EQ(mapOuterToInnerFrame(p, 100 + 10, kOuterFps).innerFrame, 20);
    // k=30 -> sourceLocal 60 -> Loop wraps to 10.
    EXPECT_EQ(mapOuterToInnerFrame(p, 100 + 30, kOuterFps).innerFrame, 10);
}

TEST(PrecompMath, SpeedClampedBothEnds) {
    // Below the floor: 0.001 behaves exactly as 0.01.
    const auto low   = makeParams(PlaybackMode::Freeze, 100, 60, 0, 50, 30.0, 0.001);
    const auto floor = makeParams(PlaybackMode::Freeze, 100, 60, 0, 50, 30.0, 0.01);
    for (FrameNumber k : {0, 25, 59}) {
        EXPECT_EQ(mapOuterToInnerFrame(low, 100 + k, kOuterFps).innerFrame,
                  mapOuterToInnerFrame(floor, 100 + k, kOuterFps).innerFrame)
            << "k=" << k;
    }
    // Above the ceiling: 1000 behaves exactly as 100. k=1 -> sourceLocal 100:
    // Loop wraps 100 % 50 = 0; Freeze clamps to 49.
    const auto fastLoop = makeParams(PlaybackMode::Loop, 100, 60, 0, 50, 30.0, 1000.0);
    EXPECT_EQ(mapOuterToInnerFrame(fastLoop, 101, kOuterFps).innerFrame, 0);
    const auto fastFreeze = makeParams(PlaybackMode::Freeze, 100, 60, 0, 50, 30.0, 1000.0);
    EXPECT_EQ(mapOuterToInnerFrame(fastFreeze, 101, kOuterFps).innerFrame, 49);
}

TEST(PrecompMath, TrimInnerStart) {
    // innerStart 10 over a 50-frame definition: playLen = 40.
    const auto loop = makeParams(PlaybackMode::Loop, 100, 60, 10, 50);
    EXPECT_EQ(mapOuterToInnerFrame(loop, 100, kOuterFps).innerFrame, 10);
    // k=40 -> wrapped 0 -> innerFrame 10 again.
    EXPECT_EQ(mapOuterToInnerFrame(loop, 100 + 40, kOuterFps).innerFrame, 10);
    // Freeze past end holds at innerStart + playLen - 1 = 49 (ADR D3).
    const auto freeze = makeParams(PlaybackMode::Freeze, 100, 60, 10, 50);
    EXPECT_EQ(mapOuterToInnerFrame(freeze, 100 + 45, kOuterFps).innerFrame, 49);
}

TEST(PrecompMath, FpsMismatch24on30) {
    // 24-fps definition on a 30-fps timeline: ratio 0.8.
    const auto p = makeParams(PlaybackMode::Freeze, 100, 60, 0, 50, 24.0);
    EXPECT_EQ(mapOuterToInnerFrame(p, 100 + 5, kOuterFps).innerFrame, 4);  // floor(4.0)
    EXPECT_EQ(mapOuterToInnerFrame(p, 100 + 7, kOuterFps).innerFrame, 5);  // floor(5.6)
}

TEST(PrecompMath, ZeroPlayLenInactive) {
    // innerStart == definitionDuration -> playLen 0.
    const auto atEnd = makeParams(PlaybackMode::Loop, 100, 60, 50, 50);
    EXPECT_FALSE(mapOuterToInnerFrame(atEnd, 110, kOuterFps).active);
    // Zero-length definition.
    const auto zeroDef = makeParams(PlaybackMode::Loop, 100, 60, 0, 0);
    EXPECT_FALSE(mapOuterToInnerFrame(zeroDef, 110, kOuterFps).active);
    // Trim past the definition end -> playLen negative.
    const auto pastEnd = makeParams(PlaybackMode::Loop, 100, 60, 60, 50);
    EXPECT_FALSE(mapOuterToInnerFrame(pastEnd, 110, kOuterFps).active);
}

TEST(PrecompMath, ZeroInstanceDurationInactive) {
    const auto p = makeParams(PlaybackMode::Loop, 100, 0);
    EXPECT_FALSE(mapOuterToInnerFrame(p, 100, kOuterFps).active);
}

TEST(PrecompMath, FreezePastEndHolds) {
    const auto p = makeParams(PlaybackMode::Freeze);
    for (FrameNumber k = 50; k < 60; ++k) {
        const auto r = mapOuterToInnerFrame(p, 100 + k, kOuterFps);
        EXPECT_TRUE(r.active);
        EXPECT_EQ(r.innerFrame, 49) << "k=" << k;
        EXPECT_FALSE(r.pingPongReverse);
    }
}

// ADR D3: instance-level PingPong is frame-identical to clip-level PingPong
// because it IS the same primitive.
TEST(PrecompMath, PingPongReflectionMatchesWrapPrimitive) {
    const FrameNumber innerStart = 10;
    const FrameNumber playLen = 40;
    // Instance duration long enough to cover three PingPong cycles.
    const auto p = makeParams(PlaybackMode::PingPong, 100, 3 * playLen, innerStart, 50);
    for (FrameNumber k = 0; k < 3 * playLen; ++k) {
        const auto r = mapOuterToInnerFrame(p, 100 + k, kOuterFps);
        const auto w = wrapLocalFrame(PlaybackMode::PingPong, playLen, k);
        ASSERT_TRUE(r.active) << "k=" << k;
        EXPECT_EQ(r.innerFrame, innerStart + w.frame) << "k=" << k;
        EXPECT_EQ(r.pingPongReverse, w.reverse) << "k=" << k;
    }
}

// ---------------------------------------------------------------------------
// PrecompLibrary skeleton
// ---------------------------------------------------------------------------

TEST(PrecompLibrary, BasicLifecycle) {
    entt::registry registry;
    PrecompLibrary lib(registry);

    auto* a = lib.createDefinition("Intro", 1280, 720, 24.0, 240);
    auto* b = lib.createDefinition("Outro", 1920, 1080, 30.0, 300);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(lib.count(), 2u);

    // Distinct, prefixed, stable ids.
    EXPECT_NE(a->id, b->id);
    EXPECT_EQ(a->id.rfind("precomp-", 0), 0u);
    EXPECT_EQ(a->id.size(), std::string("precomp-").size() + 16);

    // Lookup round-trips; unknown id misses.
    EXPECT_EQ(lib.findDefinition(a->id), a);
    EXPECT_EQ(lib.findDefinition("precomp-bogus"), nullptr);

    // touch bumps the version; unknown id reports false.
    EXPECT_EQ(a->version, 1u);
    EXPECT_TRUE(lib.touch(a->id));
    EXPECT_EQ(a->version, 2u);
    EXPECT_FALSE(lib.touch("precomp-bogus"));

    // Phase A canDelete stub: exists.
    EXPECT_TRUE(lib.canDelete(a->id));
    EXPECT_FALSE(lib.canDelete("precomp-bogus"));

    // The definition timeline exists, carries the requested rate, and the
    // Timecode duration round-trips back to the requested frame count.
    ASSERT_NE(a->timeline, nullptr);
    EXPECT_DOUBLE_EQ(a->timeline->getFrameRate(), 24.0);
    EXPECT_EQ(a->timeline->timeToFrame(a->timeline->getDuration()), 240);

    // Parameter sanitization: non-positive fps falls back to 30, negative
    // duration clamps to 0.
    auto* c = lib.createDefinition("Bad", 64, 64, 0.0, -5);
    ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(c->frameRate, 30.0);
    EXPECT_EQ(c->durationFrames, 0);
}
