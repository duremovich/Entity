#include "entity/audio/AudioDecodeWorker.hpp"
#include "entity/profile/Tracy.hpp"
#include <algorithm>
#include <chrono>
#include <vector>

namespace entity {

namespace {
void reverseStereo(std::vector<float>& buf) {
    const size_t frames = buf.size() / 2;
    for (size_t i = 0; i < frames / 2; ++i) {
        std::swap(buf[i * 2],     buf[(frames - 1 - i) * 2]);
        std::swap(buf[i * 2 + 1], buf[(frames - 1 - i) * 2 + 1]);
    }
}
} // namespace

void audioDecodeThreadFunc(std::shared_ptr<AudioDecodeWorker> w) {
    tracy::SetThreadName("AudioDecode");

    // Mark `finished` as the thread's very last act on EVERY exit path
    // (open-failure early return, normal loop exit, exception). AudioSystem's
    // reap step gates join() on this so the editor tick never blocks on an
    // in-flight decode. Mirrors DecodeSystem::decodeThreadFunc's guard.
    struct FinishedGuard {
        AudioDecodeWorker* w;
        ~FinishedGuard() { w->finished.store(true, std::memory_order_release); }
    } finishedGuard{w.get()};

    try {
        if (!w->decoder.open(w->filepath, w->targetSampleRate)) {
            w->initFailed.store(true);
            return;
        }
        w->initialized.store(true);

        int64_t cursor = 0;
        bool reverse = false;
        std::vector<float> chunk;

        while (w->running.load()) {
            if (w->seekPending.exchange(false)) {
                cursor  = std::clamp<int64_t>(w->seekTarget.load(), 0,
                              w->outPointSample - w->inPointSample);
                reverse = false;
                w->ring.clear();
                w->decoder.seekToOutputSample(w->inPointSample + cursor);
            }

            // Backpressure: if the ring is well-filled, idle briefly.
            if (w->ring.availableFrames() > size_t(w->targetSampleRate / 2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            constexpr size_t kChunk = 4096;
            const int64_t span = w->outPointSample - w->inPointSample;

            // Reverse phase: re-seek before every chunk so we walk the span
            // backward. cursor counts frames emitted in this direction.
            // Chunk N reads from [span - cursor - kChunk, span - cursor],
            // reversed in memory, giving continuous backward playback.
            if (reverse) {
                const int64_t readStart =
                    w->inPointSample + std::max<int64_t>(0, span - cursor - int64_t(kChunk));
                w->decoder.seekToOutputSample(readStart);
            }

            chunk.clear();
            w->decoder.decodeChunk(chunk, kChunk);

            // End-of-span handling per PlaybackMode. playbackMode is atomic
            // (steering threads re-store it every tick); one relaxed load
            // per wrap decision keeps both checks on the same value.
            if (chunk.empty() || cursor >= span) {
                const PlaybackMode mode =
                    w->playbackMode.load(std::memory_order_relaxed);
                if (mode == PlaybackMode::Loop) {
                    if (chunk.empty())
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    cursor = 0;
                    w->decoder.seekToOutputSample(w->inPointSample);
                    continue;
                }
                if (mode == PlaybackMode::PingPong) {
                    if (chunk.empty())
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    reverse = !reverse;
                    cursor  = 0;
                    if (!reverse) {
                        // Switching to forward: seek to inPoint.
                        w->decoder.seekToOutputSample(w->inPointSample);
                    }
                    // Switching to reverse: seek happens at top of next
                    // iteration via the reverse-phase re-seek block above.
                    continue;
                }
                // Freeze: nothing more to produce; idle.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (reverse) reverseStereo(chunk);
            size_t written = 0;
            while (written < chunk.size() / 2 && w->running.load()) {
                written += w->ring.write(&chunk[written * 2],
                                         chunk.size() / 2 - written);
                if (written < chunk.size() / 2)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            cursor += int64_t(chunk.size() / 2);
        }
    } catch (...) {
        w->initFailed.store(true);
    }
}

} // namespace entity
