#pragma once
#include "entity/audio/AudioDecoder.hpp"
#include "entity/audio/AudioRingBuffer.hpp"
#include "entity/audio/AudioMixer.hpp"
#include "entity/components/Clip.hpp"   // PlaybackMode
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace entity {

// Per-clip audio decode worker. Created/destroyed by AudioSystem on the
// editor thread; the decode thread runs independently.
struct AudioDecodeWorker {
    AudioDecoder    decoder;
    AudioRingBuffer ring;                 // 2 ch; sized in the ctor
    MixSource       mixSource;            // registered with AudioMixer

    // Identity / config (set before the thread starts).
    std::string  filepath;
    int          targetSampleRate{48000};
    int64_t      inPointSample{0};        // clip in-point, output-rate samples
    int64_t      outPointSample{0};       // clip out-point (exclusive)
    PlaybackMode playbackMode{PlaybackMode::Freeze};

    // Cross-thread control.
    std::atomic<bool>    running{false};
    std::atomic<bool>    seekPending{false};
    std::atomic<int64_t> seekTarget{0};   // clip-local output-rate sample
    std::atomic<bool>    initialized{false};
    std::atomic<bool>    initFailed{false};

    std::thread thread;

    explicit AudioDecodeWorker(int sampleRate)
        : ring(sampleRate, /*channels=*/2),   // 1 s of headroom
          targetSampleRate(sampleRate) {}
};

// Decode-thread entry point.
void audioDecodeThreadFunc(std::shared_ptr<AudioDecodeWorker> worker);

} // namespace entity
