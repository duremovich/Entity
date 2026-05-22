#pragma once
#include "entity/audio/AudioRingBuffer.hpp"
#include <atomic>
#include <array>
#include <vector>

namespace entity {

// One thing the mixer pulls from. Owned by an AudioDecodeWorker (or, in
// tests, by a synthetic producer). All fields are atomic / lock-free so
// the realtime mix callback never blocks and never reads the registry.
struct MixSource {
    AudioRingBuffer*   ring{nullptr};
    std::atomic<float> gain{1.0f};
    std::atomic<bool>  mute{false};
    std::atomic<bool>  solo{false};
    std::atomic<bool>  active{false};  // false => contributes silence
};

// Sums registered MixSources into the device buffer. mix() runs on the
// device's realtime thread; register/unregister run on the editor thread.
class AudioMixer {
public:
    static constexpr size_t MAX_SOURCES = 64;

    // Editor-thread: claim/release a slot. registerSource returns false
    // if all slots are full.
    bool registerSource(MixSource* src);
    void unregisterSource(MixSource* src);

    // Realtime-thread: sum into `out` (interleaved stereo float32).
    void mix(float* out, uint32_t frames);

private:
    std::array<std::atomic<MixSource*>, MAX_SOURCES> m_slots{};
    // Realtime mix scratch buffer. Owned by the device's realtime thread
    // (mix() is its sole accessor). Preallocated rather than allocated
    // per callback — a heap allocation inside the realtime callback can
    // stall on the heap lock and miss the device deadline (audible glitch).
    std::vector<float> m_scratch;
};

} // namespace entity
