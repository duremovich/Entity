#include "entity/audio/AudioMixer.hpp"
#include <cstring>
#include <vector>

namespace entity {

bool AudioMixer::registerSource(MixSource* src) {
    for (auto& slot : m_slots) {
        MixSource* expected = nullptr;
        if (slot.compare_exchange_strong(expected, src,
                                         std::memory_order_acq_rel)) {
            return true;
        }
    }
    return false; // MAX_SOURCES exceeded
}

void AudioMixer::unregisterSource(MixSource* src) {
    for (auto& slot : m_slots) {
        MixSource* s = slot.load(std::memory_order_acquire);
        if (s == src) { slot.store(nullptr, std::memory_order_release); return; }
    }
}

void AudioMixer::mix(float* out, uint32_t frames) {
    std::memset(out, 0, frames * 2 * sizeof(float)); // stereo

    // Solo pass: if any source is soloed, non-soloed sources are silent.
    bool anySolo = false;
    for (auto& slot : m_slots) {
        MixSource* s = slot.load(std::memory_order_acquire);
        if (s && s->active.load(std::memory_order_relaxed)
              && s->solo.load(std::memory_order_relaxed)) { anySolo = true; break; }
    }

    // Preallocated scratch — resize only grows it (a few times at startup,
    // never in steady state), so the realtime callback does no heap work.
    if (m_scratch.size() < static_cast<size_t>(frames) * 2)
        m_scratch.resize(static_cast<size_t>(frames) * 2);
    for (auto& slot : m_slots) {
        MixSource* s = slot.load(std::memory_order_acquire);
        if (!s || !s->ring) continue;
        if (!s->active.load(std::memory_order_relaxed)) continue;
        if (s->mute.load(std::memory_order_relaxed)) continue;
        if (anySolo && !s->solo.load(std::memory_order_relaxed)) continue;

        s->ring->read(m_scratch.data(), frames); // zero-fills shortfall
        const float g = s->gain.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames * 2; ++i) out[i] += m_scratch[i] * g;
    }
}

} // namespace entity
