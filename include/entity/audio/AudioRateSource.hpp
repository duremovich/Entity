#pragma once
#include "entity/director/RateSource.hpp"
#include <atomic>
#include <cstdint>

namespace entity {

class AudioDevice;

// RateSource backed by the audio device's sample clock. now() advances
// with audible playback; unhealthy when the device is stopped or its
// frame counter stalls (underrun) — the authority then falls back to the
// system clock.
class AudioRateSource : public RateSource {
public:
    explicit AudioRateSource(AudioDevice* device) : m_device(device) {}

    double now() const override;
    bool   healthy() const override;
    const char* name() const override { return "AudioRateSource"; }

private:
    AudioDevice* m_device;
    // Stall detection across consecutive healthy() calls.
    mutable std::atomic<uint64_t> m_lastFrames{0};
    mutable std::atomic<int>      m_stallStrikes{0};
};

} // namespace entity
