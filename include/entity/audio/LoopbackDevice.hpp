#pragma once
#include "entity/audio/AudioDevice.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace entity {

class LoopbackDevice : public AudioDevice {
public:
    ~LoopbackDevice() override { stop(); }

    bool start(int requestedSampleRate, int channels, FillCallback fill) override;
    void stop() override;
    bool running() const override { return m_running.load(); }
    int  sampleRate() const override { return m_sampleRate; }
    int  channelCount() const override { return m_channels; }
    uint64_t framesPlayed() const override { return m_framesPlayed.load(); }

    // Test inspection: a copy of every interleaved float frame produced.
    std::vector<float> capturedCopy() const;
    void clearCapture();

private:
    void run();

    FillCallback          m_fill;
    int                   m_sampleRate{48000};
    int                   m_channels{2};
    std::atomic<bool>     m_running{false};
    std::atomic<uint64_t> m_framesPlayed{0};
    std::thread           m_thread;
    mutable std::mutex    m_captureMutex;
    std::vector<float>    m_capture;
};

} // namespace entity
