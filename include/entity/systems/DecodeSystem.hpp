#pragma once

#include "entity/systems/System.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/DecodedFrame.hpp"
#include "entity/media/IDecodeBufferAllocator.hpp"
#include "entity/core/Types.hpp"
#include "entity/components/Clip.hpp"
#include "entity/profile/Tracy.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <limits>
#include <vector>
#include <utility>

namespace entity {

namespace bus { struct SceneSnapshot; }

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
    // Set as the decode thread's very last act (after all cleanup, every
    // exit path). reapRetiredWorkers() waits on this before join() so a
    // retired worker is reaped without blocking the editor tick on an
    // in-flight decode. See retireWorker / reapRetiredWorkers.
    std::atomic<bool>        finished{false};
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

    // Ping-pong odd-cycle flag: update()/tickFromSnapshot write, decode
    // thread reads (reverse pacing). The old plain playbackMode /
    // totalMediaFrames mirror fields were write-only and were removed in
    // issue #74 — wrap behavior is driven entirely by the steered
    // targetFrame/seekTarget, not by the worker knowing the mode.
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
 *   - Editor thread: update() per frame; manages worker lifecycle + seek
 *     requests. Editor-only (asserted) since issue #74.
 *   - Show thread: tickFromSnapshot during editor stalls (registry-free
 *     steering of existing workers), plus getWorker via PlaybackPresenter.
 *   - Decode threads: one per clip; decode → FrameCache::put.
 *   - Communication via atomics + condition variables; the m_workers map
 *     itself is guarded by m_workersMutex (see leaf-lock rules below).
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

    /**
     * Show-thread editor-stall fallback (issue #74, ADR-0014 amendment).
     * Steers existing workers' targetFrame/seek atomics from the published
     * SceneSnapshot clip catalog — zero registry access, no worker
     * lifecycle. The frame math is the same catalog math buildRenderFrame
     * consumes (CatalogClipMath), so the decoder targets exactly what the
     * presenter will request during the stall. rateNowNs is the active rate
     * source's now in ns (PlaybackTimeAuthority::rateNow() * 1e9) — clock
     * domain for the NEW-08 continuation anchor.
     */
    void tickFromSnapshot(const bus::SceneSnapshot& scene,
                          std::int64_t rateNowNs);

    void seekClip(entt::entity clipEntity, FrameNumber frame);
    void pauseAll();
    void resumeAll();

    /**
     * Read-only worker handle for status inspection (UI overlay,
     * PlaybackPresenter's seek-freshness check, SeekSyncController gate).
     * Returns a shared_ptr copied under m_workersMutex so the handle stays
     * valid even if the editor retires/reaps the worker while the caller
     * (possibly the show thread) still holds it. Never cache the result
     * across frames — re-fetch per use.
     */
    std::shared_ptr<const DecodeWorker> getWorker(entt::entity clipEntity) const;

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

    // Retire-and-reap teardown (delete-freeze fix). retireWorker signals the
    // worker to stop and moves it to m_retiredWorkers WITHOUT joining, so the
    // editor tick never blocks on an in-flight decode (joining live cost 50+ ms
    // per 4K ProRes clip — N serial joins on a group delete froze the UI).
    // reapRetiredWorkers joins+erases each retired worker only once its thread
    // has set `finished`, so each join returns immediately. Editor-thread only;
    // no lock needed (m_workers / m_retiredWorkers are editor-thread-owned).
    //
    // evictCacheOnReap distinguishes delete-retires (true: the entity is gone,
    // drop its frames) from warm-set distance-retires (false: the clip is
    // alive and its cached frames must survive for the re-entry fast-path —
    // and a re-warmed clip may already have a NEW worker filling the cache
    // that a blanket reap-evict would wrongly clobber).
    void retireWorker(entt::entity entity, bool evictCacheOnReap);
    void reapRetiredWorkers();

    static void decodeThreadFunc(std::shared_ptr<DecodeWorker> worker);

    // Atomic-store + cv-notify half of seekClip, operating on a worker the
    // caller already holds. Callable from any thread; never touches
    // m_workers.
    static void requestSeek(DecodeWorker& worker, FrameNumber frame);

    // shared_ptr copy of the worker for `entity` under m_workersMutex
    // (nullptr if absent). The mutable overload backs update()/seekClip;
    // getWorker() wraps it in a const pointee for external readers.
    std::shared_ptr<DecodeWorker> findWorker(entt::entity entity) const;

    // Shared per-worker steering core: pingPongReverse/targetFrame stores +
    // seek-on-discontinuity + cache-miss recovery. Called by the editor
    // update() (registry-derived inputs) and tickFromSnapshot (catalog-
    // derived inputs). Touches only worker atomics + FrameCache — safe from
    // either thread, including wake overlap. Returns true when cache-miss
    // recovery fired (Tracy plot bookkeeping).
    bool steerWorker(entt::entity entity, DecodeWorker& worker,
                     FrameNumber mediaFrame, bool pingPongReverse,
                     bool inTailHold, FrameNumber sourceLength,
                     FrameNumber mediaStartFrame, PlaybackMode mode,
                     bool forceSeek);

    Timeline*                m_timeline{nullptr};
    FrameCache*              m_frameCache{nullptr};
    IDecodeBufferAllocator*  m_bufferAllocator{nullptr}; // non-owning

    // Guards the m_workers map object (find / emplace / erase / iterate).
    // Shared between the editor thread (lifecycle) and the show thread
    // (getWorker via PlaybackPresenter, tickFromSnapshot lookups). Issue #74.
    //
    // LEAF-LOCK RULES:
    //  1. Never hold m_workersMutex across thread::join, worker->mutex,
    //     FrameCache, Timeline, or decoder calls.
    //  2. Readers copy the shared_ptr out under a brief lock; all atomic
    //     stores / cv notifies / requestSeek happen after unlock. Never
    //     hand out a raw DecodeWorker* that outlives the lock scope.
    //  3. Map mutation (create/retire/destroy/reap/shutdown) stays
    //     editor-thread-only — the mutex protects readers, it does not
    //     license show-thread lifecycle ops.
    mutable TracyLockable(std::mutex, m_workersMutex);
    std::unordered_map<entt::entity, std::shared_ptr<DecodeWorker>> m_workers;

    // Workers signaled to stop but not yet joined (retire-and-reap teardown).
    // Drained by reapRetiredWorkers() once each thread has set `finished`, and
    // unconditionally in shutdown(). Editor-thread only — NOT covered by
    // m_workersMutex (a retired worker can still be referenced by a show-side
    // shared_ptr copy; the shared_ptr keeps it alive across the reap).
    struct RetiredWorker {
        entt::entity                  entity{entt::null};
        std::shared_ptr<DecodeWorker> worker;
        bool                          evictOnReap{true};
    };
    std::vector<RetiredWorker> m_retiredWorkers;

    // Captured in initialize() (called from editor thread on engine startup).
    // Backs the update() editor-only assert (#74): only the editor thread
    // mutates m_workers / runs lifecycle; the show thread's stall path is
    // tickFromSnapshot.
    std::thread::id   m_editorThreadId;

    std::atomic<bool> m_globalPaused{false};
    bool              m_wasScrubbing{false};

    // warmset::compute grace bookkeeping (editor-thread only, like m_workers).
    std::unordered_map<entt::entity, int64_t> m_lastWarmNs;

    // Decode-ahead configuration
    static constexpr uint32_t DECODE_AHEAD_FRAMES = 8; // ~270ms @30fps headroom
};

} // namespace entity
