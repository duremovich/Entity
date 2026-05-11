#pragma once

#include "entity/systems/System.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/DecodedFrame.hpp"
#include "entity/core/Types.hpp"
#include "entity/components/Clip.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <limits>

namespace entity {

// Forward declarations
class Timeline;
class FrameCache;
class DecodeBufferPool;

/**
 * DecodeWorker - per-clip decode thread state.
 *
 * Owns the Decoder for one clip and pumps decoded frames into the engine-
 * global FrameCache. Pre-Phase C.10 this owned a FrameRingBuffer; the cache
 * subsumes both that role and the per-clip "ping-pong needs all frames"
 * special-casing — as long as a clip's working set fits under the cache
 * budget, ping-pong's bidirectional access never re-decodes.
 */
struct DecodeWorker {
    std::unique_ptr<Decoder> decoder;            // Owned decoder (FFmpeg / PNGSeq / HAP)
    FrameCache*              cache{nullptr};     // Engine-global cache (non-owning)
    DecodeBufferPool*        pool{nullptr};      // Engine-global decode-buffer pool (non-owning)
    entt::entity             entity{entt::null}; // Clip entity used as cache key
    std::thread              thread;

    // Thread control
    std::atomic<bool>        running{false};
    std::atomic<bool>        paused{false};
    std::atomic<bool>        seekPending{false};
    std::atomic<FrameNumber> seekTarget{0};
    std::atomic<FrameNumber> currentFrame{0};    // Last decoded frame
    std::atomic<FrameNumber> targetFrame{0};     // Decode-ahead target

    // Sentinel for "no frame requested yet" (used for discontinuity detection)
    static constexpr FrameNumber INVALID_FRAME = std::numeric_limits<FrameNumber>::min();
    std::atomic<FrameNumber> lastRequestedFrame{INVALID_FRAME};

    // Deferred initialization (decoder opened in worker thread, not main thread)
    std::atomic<bool>        initialized{false};
    std::atomic<bool>        initFailed{false};
    std::string              filepath;
    MediaType                mediaType{MediaType::Unknown};

    // Playback mode for loop/ping-pong support
    PlaybackMode             playbackMode{PlaybackMode::Freeze};
    FrameNumber              totalMediaFrames{0};
    std::atomic<bool>        pingPongReverse{false};

    // Synchronization for pause/resume
    std::mutex               mutex;
    std::condition_variable  cv;
};

/**
 * DecodeSystem - ECS system for managing background decode threads.
 *
 * ALWAYS-WARM BUFFERING FOR ZERO-LATENCY PLAYBACK:
 * Decode workers run continuously, targeting the current timeline position
 * regardless of playback state. Buffers stay warm at the playhead so Play
 * starts instantly.
 *
 * Decoded frames land in the engine-global FrameCache (set via
 * setFrameCache); all readers (PlaybackPresenter, etc.) pull from there.
 *
 * Threading:
 *   - Main thread: update() per frame; manages worker lifecycle + seek requests.
 *   - Decode threads: one per clip; decode → FrameCache::put.
 *   - Communication via atomics + condition variables.
 */
class DecodeSystem : public System {
public:
    DecodeSystem();
    ~DecodeSystem();

    void setTimeline(Timeline* timeline) { m_timeline = timeline; }
    void setFrameCache(FrameCache* cache) { m_frameCache = cache; }

    /**
     * Wire in the engine-global decode-buffer pool. Workers acquire pixel
     * buffers from it instead of resizing-from-empty after every
     * cache.put(std::move(...)). Optional -- DecodeSystem works without it
     * (workers fall back to per-frame malloc), but smooth high-bitrate
     * playback (especially 4K ProRes 4444) requires it.
     */
    void setBufferPool(DecodeBufferPool* pool) { m_bufferPool = pool; }

    void initialize(entt::registry& registry) override;
    void update(entt::registry& registry, float deltaTime) override;
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "DecodeSystem"; }

    void seekClip(entt::entity clipEntity, FrameNumber frame);
    void pauseAll();
    void resumeAll();

    /** Read-only worker handle for status inspection (UI overlay etc). */
    const DecodeWorker* getWorker(entt::entity clipEntity) const;

private:
    void createWorker(entt::entity entity, entt::registry& registry, FrameNumber initialFrame = 0);
    void destroyWorker(entt::entity entity);
    static void decodeThreadFunc(std::shared_ptr<DecodeWorker> worker);

    Timeline*         m_timeline{nullptr};
    FrameCache*       m_frameCache{nullptr};
    DecodeBufferPool* m_bufferPool{nullptr};

    std::unordered_map<entt::entity, std::shared_ptr<DecodeWorker>> m_workers;

    // Captured in initialize() (called from editor thread on engine startup).
    // Used in update() to gate worker create/destroy: only the editor thread
    // mutates m_workers. The show-thread fallback (Engine.cpp:982) still
    // ticks targetFrame on existing workers, but skips lifecycle ops to
    // avoid two threads concurrently joining the same decode std::thread.
    std::thread::id   m_editorThreadId;

    std::atomic<bool> m_globalPaused{false};
    bool              m_wasScrubbing{false};

    // Decode-ahead configuration
    static constexpr uint32_t DECODE_AHEAD_FRAMES = 8; // ~270ms @30fps headroom
};

} // namespace entity
