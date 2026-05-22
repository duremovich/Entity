#include "entity/core/SeekSyncController.hpp"
#include "entity/timeline/Timeline.hpp"
#include <entt/entt.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

using namespace entity;

// ============================================================================
// Test fixture — a minimal Timeline wired with a fresh registry
// ============================================================================

class SeekSyncControllerTest : public ::testing::Test {
protected:
    entt::registry registry;
    Timeline       timeline{registry};

    // Helper: pump tick() N times and return the gate state after each tick.
    // Returns true if the gate is clear (released) after all N ticks.
    bool pumpTicks(SeekSyncController& ctrl, int n) {
        for (int i = 0; i < n; ++i)
            ctrl.tick(&timeline);
        return !timeline.isSeekSyncGated();
    }
};

// ============================================================================
// Test: gate not set — tick() is a no-op
// ============================================================================

TEST_F(SeekSyncControllerTest, NoOpWhenGateNotSet) {
    // Both predicates always return false — would block if gate were active.
    SeekSyncController ctrl(
        []{ return false; },
        []{ return false; }
    );

    // Gate starts clear; tick() must not touch it.
    ASSERT_FALSE(timeline.isSeekSyncGated());
    ctrl.tick(&timeline);
    EXPECT_FALSE(timeline.isSeekSyncGated())
        << "tick() must be a no-op when the gate is not engaged";
}

// ============================================================================
// Test: ready on the first tick — gate released immediately
// ============================================================================

TEST_F(SeekSyncControllerTest, ReleasesOnFirstTickWhenReady) {
    SeekSyncController ctrl(
        []{ return true; },
        []{ return true; }
    );

    timeline.play();                           // engages the gate
    ASSERT_TRUE(timeline.isSeekSyncGated());

    ctrl.tick(&timeline);

    EXPECT_FALSE(timeline.isSeekSyncGated())
        << "gate should be released on the first tick when all predicates are ready";
    // Playback state must still be Playing (gate release must not pause).
    EXPECT_EQ(timeline.getPlaybackState(), PlaybackState::Playing);
}

// ============================================================================
// Test: ready after N ticks
// ============================================================================

TEST_F(SeekSyncControllerTest, ReleasesAfterNTicks) {
    int callCount = 0;
    const int readyAfter = 5;

    SeekSyncController ctrl(
        [&]{ return (++callCount >= readyAfter); },
        nullptr   // no audio predicate — counts as always ready
    );

    timeline.play();
    ASSERT_TRUE(timeline.isSeekSyncGated());

    // Ticks 1..4 — not ready yet.
    for (int i = 0; i < readyAfter - 1; ++i) {
        ctrl.tick(&timeline);
        EXPECT_TRUE(timeline.isSeekSyncGated())
            << "gate should still be held after tick " << (i + 1);
    }

    // Tick 5 — ready.
    ctrl.tick(&timeline);
    EXPECT_FALSE(timeline.isSeekSyncGated())
        << "gate should be released after the Nth tick";
}

// ============================================================================
// Test: only video predicate needed (null audio counts as ready)
// ============================================================================

TEST_F(SeekSyncControllerTest, NullAudioPredicateCountsAsReady) {
    SeekSyncController ctrl(
        []{ return true; },
        nullptr
    );
    timeline.play();
    ctrl.tick(&timeline);
    EXPECT_FALSE(timeline.isSeekSyncGated());
}

// ============================================================================
// Test: only audio predicate needed (null video counts as ready)
// ============================================================================

TEST_F(SeekSyncControllerTest, NullVideoPredicateCountsAsReady) {
    SeekSyncController ctrl(
        nullptr,
        []{ return true; }
    );
    timeline.play();
    ctrl.tick(&timeline);
    EXPECT_FALSE(timeline.isSeekSyncGated());
}

// ============================================================================
// Test: never-ready -> timeout releases the gate
// ============================================================================

TEST_F(SeekSyncControllerTest, TimeoutReleasesGate) {
    // Use a controller with a very short timeout by passing predicates that
    // always return false.  We can't override kPrerollTimeoutMs (it's a
    // constexpr constant), so we sleep past it.
    SeekSyncController ctrl(
        []{ return false; },
        []{ return false; }
    );

    timeline.play();
    ASSERT_TRUE(timeline.isSeekSyncGated());

    // Sleep past kPrerollTimeoutMs (3000 ms). This test is slow by design —
    // it is the only test that exercises the real timeout path.
    // It is excluded from fast-path runs via TIMEOUT 10 in CMakeLists.
    std::this_thread::sleep_for(
        std::chrono::milliseconds(SeekSyncController::kPrerollTimeoutMs + 200));

    ctrl.tick(&timeline);   // single tick after the timeout period
    EXPECT_FALSE(timeline.isSeekSyncGated())
        << "gate must be released after kPrerollTimeoutMs regardless of readiness";
}

// ============================================================================
// Test: pause during preroll clears the gate (Timeline::pause clears it)
// ============================================================================

TEST_F(SeekSyncControllerTest, PauseDuringPrerollClearsGate) {
    // Predicate never returns true — simulates a stalled decoder.
    SeekSyncController ctrl(
        []{ return false; },
        []{ return false; }
    );

    timeline.play();
    ASSERT_TRUE(timeline.isSeekSyncGated());

    // Tick once — gate is still held.
    ctrl.tick(&timeline);
    EXPECT_TRUE(timeline.isSeekSyncGated());

    // User presses Pause while the gate is active.
    timeline.pause();
    EXPECT_FALSE(timeline.isSeekSyncGated())
        << "Timeline::pause() must clear the gate";

    // Subsequent ticks must not re-engage the gate.
    ctrl.tick(&timeline);
    ctrl.tick(&timeline);
    EXPECT_FALSE(timeline.isSeekSyncGated())
        << "gate must stay clear after pause; tick() must not re-engage it";
}

// ============================================================================
// Test: stop during preroll clears the gate
// ============================================================================

TEST_F(SeekSyncControllerTest, StopDuringPrerollClearsGate) {
    SeekSyncController ctrl(
        []{ return false; },
        nullptr
    );

    timeline.play();
    ASSERT_TRUE(timeline.isSeekSyncGated());

    timeline.stop();
    EXPECT_FALSE(timeline.isSeekSyncGated())
        << "Timeline::stop() must clear the gate";

    ctrl.tick(&timeline);
    EXPECT_FALSE(timeline.isSeekSyncGated());
}

// ============================================================================
// Test: seek during preroll clears the gate
// ============================================================================

TEST_F(SeekSyncControllerTest, SeekDuringPrerollClearsGate) {
    SeekSyncController ctrl(
        []{ return false; },
        nullptr
    );

    timeline.play();
    ASSERT_TRUE(timeline.isSeekSyncGated());

    timeline.seekToFrame(0);
    EXPECT_FALSE(timeline.isSeekSyncGated())
        << "Timeline::seek() must clear the gate";

    ctrl.tick(&timeline);
    EXPECT_FALSE(timeline.isSeekSyncGated());
}

// ============================================================================
// Test: re-engage after release works correctly
// ============================================================================

TEST_F(SeekSyncControllerTest, ReEngageAfterReleaseWorks) {
    int readyCount = 0;
    SeekSyncController ctrl(
        [&]{ return (readyCount > 0); },
        nullptr
    );

    // First play — not ready; one tick, still gated.
    timeline.play();
    ctrl.tick(&timeline);
    EXPECT_TRUE(timeline.isSeekSyncGated());

    // Mark ready; next tick releases.
    readyCount = 1;
    ctrl.tick(&timeline);
    EXPECT_FALSE(timeline.isSeekSyncGated());

    // Pause + re-seek + play again.
    timeline.pause();
    timeline.seekToFrame(30);
    readyCount = 0;

    timeline.play();
    ASSERT_TRUE(timeline.isSeekSyncGated());

    ctrl.tick(&timeline);
    EXPECT_TRUE(timeline.isSeekSyncGated())   // not ready yet
        << "second engage should gate again";

    readyCount = 1;
    ctrl.tick(&timeline);
    EXPECT_FALSE(timeline.isSeekSyncGated())
        << "second engage should release once ready";
}

// ============================================================================
// Test: null Timeline pointer — tick() must not crash
// ============================================================================

TEST_F(SeekSyncControllerTest, NullTimelineNoCrash) {
    SeekSyncController ctrl([]{ return true; }, nullptr);
    EXPECT_NO_FATAL_FAILURE(ctrl.tick(nullptr));
}
