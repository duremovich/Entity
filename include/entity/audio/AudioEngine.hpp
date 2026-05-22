#pragma once
#include "entity/audio/AudioMixer.hpp"
#include "entity/audio/AudioRateSource.hpp"
#include <atomic>
#include <memory>

namespace entity {

class AudioDevice;

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // useLoopback=true builds a LoopbackDevice (tests / headless);
    // false builds a WasapiDevice. Returns false if the device fails to
    // open — callers should fall back to silent operation, not crash.
    bool start(bool useLoopback);
    void stop();
    bool running() const;

    AudioMixer&      mixer()      { return m_mixer; }
    AudioRateSource* rateSource() { return m_rateSource.get(); }
    AudioDevice*     device()     { return m_device.get(); }
    int sampleRate() const;

    void  setMasterGain(float g) { m_masterGain.store(g); }
    float masterGain() const     { return m_masterGain.load(); }
    void  setMasterMute(bool m)  { m_masterMute.store(m); }
    bool  masterMute() const     { return m_masterMute.load(); }

private:
    void fill(float* out, uint32_t frames);   // the device FillCallback

    std::unique_ptr<AudioDevice>     m_device;
    AudioMixer                       m_mixer;
    std::unique_ptr<AudioRateSource> m_rateSource;
    std::atomic<float>               m_masterGain{1.0f};
    std::atomic<bool>                m_masterMute{false};
};

} // namespace entity
