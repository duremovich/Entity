#pragma once
#include "entity/audio/AudioDecoder.hpp"
#include "entity/audio/AudioRingBuffer.hpp"
#include "entity/audio/AudioMixer.hpp"
#include "entity/components/Clip.hpp"   // PlaybackMode
#include <atomic>
#include <cstdint>
#include <limits>
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

    // Written every steering tick (editor update() / show-thread stall
    // fallback), read by the decode thread's wrap logic — atomic since
    // issue #74 (was a plain field racing the decode thread).
    std::atomic<PlaybackMode> playbackMode{PlaybackMode::Freeze};

    // Discontinuity-seek state, moved here from AudioSystem's per-entity map
    // (issue #74) so the editor tick and the show-thread stall fallback share
    // it race-free. lastExpectedSample is the clip-local output-rate sample
    // the steering path computed on its previous tick; the sentinel means
    // "no tick has steered this worker yet" (fresh worker → initial seek).
    // lastExpectedSampleNs timestamps that computation so the discontinuity
    // threshold can scale with the actual gap between steering ticks instead
    // of assuming back-to-back ticks (a >50 ms editor stall is exactly such
    // a gap). seekCount counts real issued seeks — test observability
    // (AssertAudioWorkerSeekCountAtMost).
    static constexpr int64_t kNoExpectedSample =
        std::numeric_limits<int64_t>::min();
    std::atomic<int64_t>  lastExpectedSample{kNoExpectedSample};
    std::atomic<int64_t>  lastExpectedSampleNs{0};
    std::atomic<uint32_t> seekCount{0};

    // Cross-thread control.
    std::atomic<bool>    running{false};
    // Set as the decode thread's very last act (every exit path). AudioSystem's
    // reap step waits on this before join() so a retired worker is reaped
    // without blocking the editor tick on an in-flight decode. Mirrors
    // DecodeWorker::finished.
    std::atomic<bool>    finished{false};
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
