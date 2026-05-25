#pragma once

#include "entity/systems/System.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/DecodedFrame.hpp"
#include "entity/media/IDecodeBufferAllocator.hpp"
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
class DecodeBufferPool; // retained for backward-compat; callers that use
                        // setBufferPool(DecodeBufferPool*) still compile

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
    std::unique_ptr<Decoder>  decoder;            // Owned decoder (FFmpeg / PNGSeq / HAP)
    FrameCache*               cache{nullptr};     // Engine-global cache (non-owning)
    IDecodeBufferAllocator*   allocator{nullptr}; // Pixel-buffer allocator (non-owning)
    entt::entity              entity{entt::null}; // Clip entity used as cache key
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
     * Wire in the pixel-buffer allocator. Workers call
     * allocator->prepareDecodeBuffer() instead of resizing-from-empty after
     * every cache.put(). Optional — DecodeSystem works without one (falls
     * back to per-frame malloc), but smooth 4K playback requires it.
     *
     * Pass a CpuHeapDecodeBufferAllocator (wrapping DecodeBufferPool) for the
     * current default behaviour, or an UploadHeapDecodeBufferAllocator for
     * the zero-copy upload-heap path (Phase 4+).
     */
    void setAllocator(IDecodeBufferAllocator* allocator) { m_bufferAllocator = allocator; }

    /**
     * Legacy overload kept for existing call sites (Renderer.cpp passes a raw
     * DecodeBufferPool* from before the allocator abstraction). Renderer.cpp
     * is updated in Phase 3 to pass a CpuHeapDecodeBufferAllocator instead;
     * until that swap is complete, this overload makes the code compile.
     *
     * @deprecated Use setAllocator(IDecodeBufferAllocator*) instead.
     */
    void setBufferPool(DecodeBufferPool* pool) {
        // The concrete allocator wrapping the pool is owned externally
        // (by Renderer or whoever calls this). We just store the raw pointer
        // to the allocator interface. Since this overload is called only from
        // Renderer.cpp which is being updated simultaneously, this bridge is
        // intentionally thin — it satisfies the compiler while the migration lands.
        (void)pool; // Renderer.cpp will pass an allocator via setAllocator() instead.
    }

    void initialize(entt::registry& registry) override;
    void update(entt::registry& registry, float deltaTime) override;
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "DecodeSystem"; }

    void seekClip(entt::entity clipEntity, FrameNumber frame);
    void pauseAll();
    void resumeAll();

    /** Read-only worker handle for status inspection (UI overlay etc). */
    const DecodeWorker* getWorker(entt::entity clipEntity) const;

    /**
     * Returns true when the given clip has a decoded frame ready at
     * `mediaFrame` and is not mid-seek. Used by SeekSyncController to decide
     * when video is primed after a seek.
     *
     * - No worker → false (wait for bootstrap)
     * - initFailed → true (don't block on broken media)
     * - else → initialized && !seekPending && FrameCache::has(entity, mediaFrame)
     */
    bool isClipReadyAt(entt::entity clipEntity, FrameNumber mediaFrame) const;

    /**
     * Tear down the decode worker for a specific clip and evict the
     * clip's frames from the FrameCache. Joins the worker thread
     * (blocking; can take 50+ ms on 4K ProRes), releases the decoder,
     * and drops all cached decoded frames keyed by this entity so
     * stale frames from a previous media aren't served after a media
     * swap. Editor-thread-only — same constraint as the per-tick
     * lifecycle ops. No-op if no worker exists for the entity (the
     * cache eviction still runs).
     */
    void destroyClipWorker(entt::entity entity);

private:
    void createWorker(entt::entity entity, entt::registry& registry, FrameNumber initialFrame = 0);
    void destroyWorker(entt::entity entity);
    static void decodeThreadFunc(std::shared_ptr<DecodeWorker> worker);

    Timeline*                m_timeline{nullptr};
    FrameCache*              m_frameCache{nullptr};
    IDecodeBufferAllocator*  m_bufferAllocator{nullptr}; // non-owning

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

    // Sliding-window prefetch lookahead. Each editor tick (when not Stopped)
    // any not-yet-started clip whose startFrame is within this many seconds
    // of the playhead gets a worker bootstrapped — so the FFmpeg open + seek
    // + first-frame decode runs in the background, and when the playhead
    // (or a cue-jump) reaches the clip its frames are already in the cache.
    // 5 s is comfortably bigger than worst-case cold-open (~200–300 ms on
    // 4K ProRes) plus seek + first-decode, with headroom for slow disks.
    static constexpr double kPrefetchAheadSeconds = 5.0;
};

} // namespace entity
