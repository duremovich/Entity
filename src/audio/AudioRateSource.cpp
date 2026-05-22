#include "entity/audio/AudioRateSource.hpp"
#include "entity/audio/AudioDevice.hpp"

namespace entity {

double AudioRateSource::now() const {
    if (!m_device || m_device->sampleRate() <= 0) return 0.0;
    return double(m_device->framesPlayed()) / double(m_device->sampleRate());
}

bool AudioRateSource::healthy() const {
    if (!m_device || !m_device->running()) return false;
    const uint64_t f = m_device->framesPlayed();
    const uint64_t prev = m_lastFrames.exchange(f);
    // Three consecutive non-advancing samples => treat as stalled.
    if (f <= prev) {
        return m_stallStrikes.fetch_add(1) < 2;
    }
    m_stallStrikes.store(0);
    return true;
}

} // namespace entity
