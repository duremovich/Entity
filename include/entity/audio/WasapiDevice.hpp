#pragma once
#include "entity/audio/AudioDevice.hpp"
#include <atomic>
#include <future>
#include <thread>

namespace entity {

// Shared-mode event-driven WASAPI render client. All Windows SDK headers
// (<audioclient.h>, <mmdeviceapi.h>, <avrt.h>) are confined to the .cpp
// via the Impl pimpl. Uses IAudioClient (not IAudioClient3 — that is for
// InitializeSharedAudioStream low-latency mode which is not needed here).
//
// COM apartment note: ALL COM work (CoInitializeEx, device enumeration,
// client init, CoUninitialize) runs on the render thread, never on the
// editor/main thread. This avoids apartment-model collisions with other
// COM users on the editor thread (e.g. TextRasterizer's STA WIC factory).
class WasapiDevice : public AudioDevice {
public:
    ~WasapiDevice() override { stop(); }
    bool start(int requestedSampleRate, int channels, FillCallback fill) override;
    void stop() override;
    bool running() const override { return m_running.load(); }
    int  sampleRate() const override { return m_sampleRate; }
    int  channelCount() const override { return m_channels; }
    uint64_t framesPlayed() const override;

private:
    void run(std::promise<bool> initPromise);  // render thread body

    FillCallback      m_fill;
    int               m_sampleRate{48000};
    int               m_channels{2};
    std::atomic<bool> m_running{false};
    std::thread       m_thread;
    void*             m_renderEvent{nullptr};  // HANDLE (opaque in header)
    struct Impl;
    Impl* m_impl{nullptr};
};

} // namespace entity
