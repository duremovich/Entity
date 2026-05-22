#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/director/RateSource.hpp"
#include <gtest/gtest.h>
#include <entt/entt.hpp>
#include <chrono>
#include <thread>

using entity::RateSource;
using entity::PlaybackTimeAuthority;

namespace {

struct FakeRateSource : RateSource {
    double  fakeNow{0.0};
    bool    fakeHealthy{true};

    double      now()     const override { return fakeNow; }
    bool        healthy() const override { return fakeHealthy; }
    const char* name()    const override { return "FakeRateSource"; }
};

} // namespace

class RateSourceSelectorTest : public ::testing::Test {
protected:
    entt::registry reg;
    // PlaybackTimeAuthority requires a non-null Timeline* only for snapshot/
    // map-to-media-frame paths — timing tests don't need a real Timeline.
    PlaybackTimeAuthority pta{reg, nullptr};
};

TEST_F(RateSourceSelectorTest, HealthyAudioSourceIsSelected) {
    FakeRateSource audio;
    audio.fakeNow     = 1.0;
    audio.fakeHealthy = true;
    pta.setAudioRateSource(&audio);

    pta.startTiming();

    // Advance the audio clock by 0.1 s.
    audio.fakeNow = 1.1;
    pta.updateTiming();

    EXPECT_NEAR(pta.getDeltaTime(), 0.1, 1e-9);
}

TEST_F(RateSourceSelectorTest, UnhealthyAudioSourceFallsBackToSystem) {
    FakeRateSource audio;
    audio.fakeNow     = 1.0;
    audio.fakeHealthy = true;
    pta.setAudioRateSource(&audio);

    pta.startTiming();

    // Audio ticks normally for one update.
    audio.fakeNow = 1.1;
    pta.updateTiming();
    EXPECT_NEAR(pta.getDeltaTime(), 0.1, 1e-9);

    // Audio goes unhealthy — selector switches to system clock.
    audio.fakeHealthy = false;
    // On the switch frame dt must be clamped to 0 (re-anchor tick).
    pta.updateTiming();
    EXPECT_DOUBLE_EQ(pta.getDeltaTime(), 0.0);

    // Subsequent ticks must track the system clock and produce positive dt.
    // We can't control SystemRateSource::now() directly, but we can call
    // updateTiming() again after a short real sleep and assert dt > 0,
    // confirming the fallback source is being queried (not stuck at 0).
    // Use a 5ms sleep — SystemRateSource uses steady_clock, so this should
    // reliably produce a positive delta without being so large it flakes.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pta.updateTiming();
    EXPECT_GT(pta.getDeltaTime(), 0.0);
}

TEST_F(RateSourceSelectorTest, DeltaTimeNeverNegative) {
    FakeRateSource audio;
    audio.fakeNow     = 10.0;
    audio.fakeHealthy = true;
    pta.setAudioRateSource(&audio);

    pta.startTiming();

    // Simulate a backward clock jump (should not happen in practice, but
    // the implementation clamps to 0 rather than producing negative dt).
    audio.fakeNow = 9.5;
    pta.updateTiming();

    EXPECT_GE(pta.getDeltaTime(), 0.0);
}

TEST_F(RateSourceSelectorTest, NoAudioSourceUsesSystemClock) {
    // No setAudioRateSource call — falls back to SystemRateSource which
    // is always healthy.  We can't control the system clock but we can
    // verify startTiming + updateTiming don't crash and dt >= 0.
    pta.startTiming();
    pta.updateTiming();
    EXPECT_GE(pta.getDeltaTime(), 0.0);
}
