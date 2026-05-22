#include "entity/audio/AudioEngine.hpp"
#include "entity/audio/WasapiDevice.hpp"
#include "entity/audio/LoopbackDevice.hpp"
#include "entity/profile/Tracy.hpp"

namespace entity {

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(bool useLoopback) {
    if (m_device) return true;
    if (useLoopback) m_device = std::make_unique<LoopbackDevice>();
    else             m_device = std::make_unique<WasapiDevice>();
    m_rateSource = std::make_unique<AudioRateSource>(m_device.get());
    const bool ok = m_device->start(48000, 2,
        [this](float* out, uint32_t frames) { fill(out, frames); });
    if (!ok) { m_device.reset(); m_rateSource.reset(); }
    return ok;
}

void AudioEngine::stop() {
    if (m_device) m_device->stop();
    m_device.reset();
    m_rateSource.reset();
}

bool AudioEngine::running() const { return m_device && m_device->running(); }
int  AudioEngine::sampleRate() const { return m_device ? m_device->sampleRate() : 48000; }

void AudioEngine::fill(float* out, uint32_t frames) {
    ZoneScopedN("AudioMix");
    m_mixer.mix(out, frames);
    const float g = m_masterMute.load() ? 0.0f : m_masterGain.load();
    if (g != 1.0f) for (uint32_t i = 0; i < frames * 2; ++i) out[i] *= g;
}

} // namespace entity
