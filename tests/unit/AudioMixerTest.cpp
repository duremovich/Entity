#include "entity/audio/AudioMixer.hpp"
#include "entity/audio/AudioRingBuffer.hpp"
#include <gtest/gtest.h>
#include <vector>
#include <cmath>

using entity::AudioMixer;
using entity::AudioRingBuffer;
using entity::MixSource;

namespace {

// Fill a 2-channel ring with `frames` copies of (left, right).
void fillRing(AudioRingBuffer& ring, float left, float right, uint32_t frames) {
    std::vector<float> buf(frames * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        buf[i * 2]     = left;
        buf[i * 2 + 1] = right;
    }
    ring.write(buf.data(), frames);
}

// Sum absolute values across an interleaved stereo output buffer.
float sumAbs(const std::vector<float>& buf) {
    float s = 0.0f;
    for (float x : buf) s += std::fabs(x);
    return s;
}

} // namespace

TEST(AudioMixer, TwoActiveSources_SummedWithGain) {
    AudioRingBuffer r1(256, 2), r2(256, 2);
    fillRing(r1, 0.5f, 0.5f, 64);
    fillRing(r2, 0.25f, 0.25f, 64);

    MixSource s1, s2;
    s1.ring = &r1; s1.gain.store(1.0f); s1.active.store(true);
    s2.ring = &r2; s2.gain.store(1.0f); s2.active.store(true);

    AudioMixer mixer;
    ASSERT_TRUE(mixer.registerSource(&s1));
    ASSERT_TRUE(mixer.registerSource(&s2));

    std::vector<float> out(64 * 2, 0.0f);
    mixer.mix(out.data(), 64);

    // Expected: 0.5 + 0.25 = 0.75 per sample (both channels).
    EXPECT_NEAR(out[0], 0.75f, 0.001f);
    EXPECT_NEAR(out[1], 0.75f, 0.001f);
}

TEST(AudioMixer, GainScalesOutput) {
    AudioRingBuffer r1(256, 2);
    fillRing(r1, 1.0f, 1.0f, 64);

    MixSource s1;
    s1.ring = &r1; s1.gain.store(0.5f); s1.active.store(true);

    AudioMixer mixer;
    ASSERT_TRUE(mixer.registerSource(&s1));

    std::vector<float> out(64 * 2, 0.0f);
    mixer.mix(out.data(), 64);

    EXPECT_NEAR(out[0], 0.5f, 0.001f);
}

TEST(AudioMixer, MuteProducesSilence) {
    AudioRingBuffer r1(256, 2);
    fillRing(r1, 1.0f, 1.0f, 64);

    MixSource s1;
    s1.ring = &r1; s1.gain.store(1.0f); s1.mute.store(true); s1.active.store(true);

    AudioMixer mixer;
    ASSERT_TRUE(mixer.registerSource(&s1));

    std::vector<float> out(64 * 2, 0.0f);
    mixer.mix(out.data(), 64);

    EXPECT_FLOAT_EQ(sumAbs(out), 0.0f);
}

TEST(AudioMixer, SoloSilencesNonSoloSource) {
    AudioRingBuffer r1(256, 2), r2(256, 2);
    fillRing(r1, 1.0f, 1.0f, 64);
    fillRing(r2, 1.0f, 1.0f, 64);

    MixSource s1, s2;
    s1.ring = &r1; s1.gain.store(1.0f); s1.solo.store(true);  s1.active.store(true);
    s2.ring = &r2; s2.gain.store(1.0f); s2.solo.store(false); s2.active.store(true);

    AudioMixer mixer;
    ASSERT_TRUE(mixer.registerSource(&s1));
    ASSERT_TRUE(mixer.registerSource(&s2));

    std::vector<float> out(64 * 2, 0.0f);
    mixer.mix(out.data(), 64);

    // Solo source should be heard (1.0), non-solo silenced => total = 1.0.
    EXPECT_NEAR(out[0], 1.0f, 0.001f);
}

TEST(AudioMixer, InactiveSourceContributesSilence) {
    AudioRingBuffer r1(256, 2);
    fillRing(r1, 1.0f, 1.0f, 64);

    MixSource s1;
    s1.ring = &r1; s1.gain.store(1.0f); s1.active.store(false);

    AudioMixer mixer;
    ASSERT_TRUE(mixer.registerSource(&s1));

    std::vector<float> out(64 * 2, 0.0f);
    mixer.mix(out.data(), 64);

    EXPECT_FLOAT_EQ(sumAbs(out), 0.0f);
}

TEST(AudioMixer, UnregisterStopsContribution) {
    AudioRingBuffer r1(256, 2);
    fillRing(r1, 1.0f, 1.0f, 64);

    MixSource s1;
    s1.ring = &r1; s1.gain.store(1.0f); s1.active.store(true);

    AudioMixer mixer;
    ASSERT_TRUE(mixer.registerSource(&s1));
    mixer.unregisterSource(&s1);

    // Re-fill so it isn't drained.
    fillRing(r1, 1.0f, 1.0f, 64);

    std::vector<float> out(64 * 2, 0.0f);
    mixer.mix(out.data(), 64);

    EXPECT_FLOAT_EQ(sumAbs(out), 0.0f);
}
