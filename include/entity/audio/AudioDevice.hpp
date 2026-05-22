#pragma once
#include <cstdint>
#include <functional>

namespace entity {

// Abstracts the OS audio backend. The test seam (LoopbackDevice) and the
// future cross-platform / device-picker seam. The device pulls audio by
// calling the FillCallback from its own realtime thread.
class AudioDevice {
public:
    // out: interleaved float32, `frames` * channelCount() floats. The
    // callback must fill the whole block and must not block.
    using FillCallback = std::function<void(float* out, uint32_t frames)>;

    virtual ~AudioDevice() = default;

    // Opens the device at (or near) the requested format and starts the
    // realtime callback. Returns false if the device could not open.
    virtual bool start(int requestedSampleRate, int channels,
                       FillCallback fill) = 0;
    virtual void stop() = 0;
    virtual bool running() const = 0;

    virtual int sampleRate() const = 0;
    virtual int channelCount() const = 0;

    // Total audio frames the device has presented since start(). This is
    // the master clock AudioRateSource reads. Monotonic while running.
    virtual uint64_t framesPlayed() const = 0;
};

} // namespace entity
