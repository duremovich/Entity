/**
 * DecodeSystem Implementation
 *
 * Manages background decode threads. Each clip with a FrameBuffer marker
 * gets a worker that pumps decoded frames into the engine-global FrameCache.
 */

#include "entity/systems/DecodeSystem.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/timeline/SectionFade.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/DecodeBufferPool.hpp"
#include "entity/media/FrameCache.hpp"
#include <cmath>
#include <iostream>
#include <chrono>

namespace entity {

DecodeSystem::DecodeSystem() = default;

DecodeSystem::~DecodeSystem() {
    // Ensure all workers are stopped
    for (auto& [entity, worker] : m_workers) {
        if (worker && worker->running.load()) {
            worker->running.store(false);
            worker->cv.notify_all();
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
    }
    m_workers.clear();
}

void DecodeSystem::initialize(entt::registry& registry) {
    (void)registry;
    m_editorThreadId = std::this_thread::get_id();
    std::cout << "DecodeSystem initialized" << std::endl;
}

void DecodeSystem::update(entt::registry& registry, float deltaTime) {
    ZoneScopedN("DecodeSystem::update");
    (void)deltaTime;
    if (!m_timeline) return;

    // Gate worker lifecycle (create/destroy) to the editor thread. The
    // show-thread fallback (Engine.cpp:982) re-enters this function during
    // editor stalls so existing workers keep targeting fresh frames, but
    // it must NOT mutate m_workers — concurrent join() on the same decode
    // std::thread from both threads throws std::system_error(no_such_process)
    // when the second join finds the OS handle already released.
    const bool isEditorTick =
        (std::this_thread::get_id() == m_editorThreadId);

    // Detect scrubbing end - when scrubbing stops, force seeks for all active clips
    bool currentlyScrubbing = m_timeline->isScrubbing();
    bool scrubbingJustEnded = m_wasScrubbing && !currentlyScrubbing;
    m_wasScrubbing = currentlyScrubbing;

    // ALWAYS-WARM BUFFERING — workers don't pause on timeline state, only on
    // explicit globalPaused (off by default). Frames sit hot at the playhead
    // so Play starts with zero buffering latency.
    if (m_globalPaused.load()) {
        resumeAll();
    }

    FrameNumber currentTimelineFrame = m_timeline->getCurrentFrame();

    auto view = registry.view<Clip, FrameBuffer>();
    // FrameBuffer is an empty marker type; entt elides empty components from
    // view::each's tuple, so the binding here is (entity, clip) only.
    for (auto [entity, clip] : view.each()) {
        if (!clip.loaded) continue;

        auto workerIt = m_workers.find(entity);
        if (workerIt == m_workers.end()) {
            // Bootstrap is editor-only (writes m_workers). New clips can't
            // appear during an editor stall (registry is frozen), so the
            // show-thread fallback never has a legitimate clip without a
            // pre-existing worker — skip and let the next editor tick
            // bootstrap if needed.
            if (!isEditorTick) continue;

            // Bootstrap a worker at the current playhead-mapped media frame.
            FrameNumber initialMediaFrame = clip.mediaStartFrame;
            if (currentTimelineFrame >= clip.startFrame &&
                currentTimelineFrame < clip.startFrame + clip.duration) {
                double timelineFrameRate = m_timeline->getFrameRate();
                double frameRateRatio = clip.framerate / timelineFrameRate;
                FrameNumber localFrame = currentTimelineFrame - clip.startFrame;
                FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));
                initialMediaFrame = clip.mediaStartFrame + sourceLocalFrame;
            }
            createWorker(entity, registry, initialMediaFrame);
            workerIt = m_workers.find(entity);
            if (workerIt == m_workers.end()) continue;
        }

        DecodeWorker* worker = workerIt->second.get();
        if (!worker) continue;

        // Keep worker's playback-mode + duration views in sync with the clip
        worker->playbackMode = clip.playbackMode;
        if (clip.totalMediaFrames > 0) {
            worker->totalMediaFrames = clip.totalMediaFrames;
        } else {
            double frameRateRatio = clip.framerate / m_timeline->getFrameRate();
            worker->totalMediaFrames = static_cast<FrameNumber>(clip.duration * frameRateRatio);
        }

        const FrameNumber clipEnd = clip.startFrame + clip.duration;
        // Fix 5 (2026-05-23 follow-up) — extend the "active for steering"
        // window to include the section fade tail. During the tail the
        // worker is no longer being advanced by playback (timeline has
        // passed clipEnd), but the gate predicate and the compositor
        // both ask for the held frame mapToMediaFrame produces at
        // clipEnd-1. Keep steering the worker so the held frame stays
        // in cache against LRU eviction during long tails or multi-clip
        // pressure. tailFrames is 0 when no section break aligns with
        // clipEnd, so this is a no-op for clips not at a break.
        const FrameNumber tailFrames =
            timeline::sectionFadeTailFrames(*m_timeline, clipEnd);
        const bool inAuthored =
            (currentTimelineFrame >= clip.startFrame &&
             currentTimelineFrame < clipEnd);
        const bool inTail =
            (tailFrames > 0 &&
             currentTimelineFrame >= clipEnd &&
             currentTimelineFrame < clipEnd + tailFrames);
        if (inAuthored || inTail) {
            // Active clip: compute target media frame using playback mode.
            // In the tail, clamp localFrame to the last authored timeline
            // frame so the worker holds the same frame the gate predicate
            // and compositor read out of mapToMediaFrame. Phase-steering
            // branches below override sourceLocalFrame for continuation
            // and post-break-anchor cases — those keep their existing
            // semantics; this clamp only matters when neither fires.
            FrameNumber localFrame = inTail
                ? (clip.duration - 1)
                : (currentTimelineFrame - clip.startFrame);

            double timelineFrameRate = m_timeline->getFrameRate();
            double frameRateRatio = clip.framerate / timelineFrameRate;
            FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));

            // Phase 1 fix — when the SectionScheduler has put this clip in
            // Normal continuation, the timeline frame is frozen at the
            // break but `phase.sourcePhaseFrames` keeps walking at
            // `clip.framerate`. Steer the worker's target by the same
            // phase so the decoder doesn't park at break + DECODE_AHEAD
            // and silently miss every subsequent wrapped frame.
            const ClipPlaybackPhase* phase = registry.try_get<ClipPlaybackPhase>(entity);
            if (phase && phase->inContinuation
                    && clip.sectionBehavior == SectionBehavior::Normal) {
                const double phaseClamped = std::max(phase->sourcePhaseFrames, 0.0);
                sourceLocalFrame = static_cast<FrameNumber>(std::floor(phaseClamped));
            } else if (phase && phase->postBreakMediaAnchor >= 0) {
                // Round-2 fixup, Phase 4 — post-break anchor steers the
                // decoder so the worker target tracks the same media
                // frame the presenter is mapping to. Mirrors the
                // anchor branch in PlaybackTimeAuthority::mapToMediaFrame
                // — the wrap math below (Loop / PingPong / Freeze)
                // applies the same way to this sourceLocalFrame.
                const double timelineDelta = static_cast<double>(
                    currentTimelineFrame - phase->anchorTimelineFrame);
                const double localFloat =
                    static_cast<double>(phase->postBreakMediaAnchor - clip.mediaStartFrame)
                    + timelineDelta * frameRateRatio;
                sourceLocalFrame = static_cast<FrameNumber>(
                    std::floor(std::max(localFloat, 0.0)));
            }

            FrameNumber sourceLength = effectivePlaybackLength(clip);
            FrameNumber mediaFrame = clip.mediaStartFrame;

            if (sourceLocalFrame < sourceLength) {
                mediaFrame = clip.mediaStartFrame + sourceLocalFrame;
            } else {
                switch (clip.playbackMode) {
                    case PlaybackMode::Freeze:
                        mediaFrame = clip.mediaStartFrame + sourceLength - 1;
                        break;
                    case PlaybackMode::Loop:
                        mediaFrame = clip.mediaStartFrame + (sourceLocalFrame % sourceLength);
                        break;
                    case PlaybackMode::PingPong: {
                        FrameNumber cycle = sourceLocalFrame / sourceLength;
                        FrameNumber pos = sourceLocalFrame % sourceLength;
                        if (cycle % 2 == 0) {
                            mediaFrame = clip.mediaStartFrame + pos;
                        } else {
                            mediaFrame = clip.mediaStartFrame + (sourceLength - 1 - pos);
                        }
                        break;
                    }
                }
            }

            bool isReverse = false;
            if (clip.playbackMode == PlaybackMode::PingPong && sourceLength > 0) {
                FrameNumber cycle = sourceLocalFrame / sourceLength;
                isReverse = (cycle % 2 == 1);
            }
            worker->pingPongReverse.store(isReverse);

            // Decode-ahead target: in ping-pong reverse, target the current
            // mediaFrame (we need frames at and below this one); otherwise
            // run ahead by DECODE_AHEAD_FRAMES.
            if (clip.playbackMode == PlaybackMode::PingPong && isReverse) {
                worker->targetFrame.store(mediaFrame);
            } else {
                worker->targetFrame.store(mediaFrame + DECODE_AHEAD_FRAMES);
            }

            // Seek-on-discontinuity. Same thresholds during scrubbing as
            // outside it — seekPending already prevents piling up seeks, so
            // running this path during a drag doesn't thrash. The previous
            // "skip seeks while scrubbing" rule produced multi-second display
            // freezes when scrubbing across big gaps because the worker had
            // to packet-skip every frame in between (ProRes ~10ms each →
            // scrub from 0 to 1000 = ~10 s of frozen display). Small forward
            // jumps still hit the packet-skip path below the threshold.
            FrameNumber lastRequested = worker->lastRequestedFrame.load();
            bool needsSeek = false;

            // Cache-miss recovery. The worker can be ahead of mediaFrame
            // (worker->currentFrame >= mediaFrame) but the global FrameCache
            // no longer has it — LRU evicted the worker's previously-decoded
            // frames while another clip was playing. The worker won't re-
            // decode on its own because nextFrame > targetFrame from its
            // perspective; it just idles. Force a seek so it re-decodes.
            // The decode-thread fast path (line ~555) skips the actual seek
            // when cache.has(seekTarget) is true, so this is cheap when the
            // cache happens to be hot.
            //
            // Guard on worker->currentFrame >= mediaFrame to avoid spurious
            // seeks during normal play, when the worker is decoding TOWARD
            // mediaFrame and the cache transiently misses because the decode
            // hasn't landed yet. In that case currentFrame < mediaFrame and
            // we let the worker work.
            if (!worker->seekPending.load() && m_frameCache &&
                    !m_frameCache->has(entity, mediaFrame) &&
                    worker->currentFrame.load() >= mediaFrame) {
                needsSeek = true;
            }

            if (!worker->seekPending.load() && lastRequested != DecodeWorker::INVALID_FRAME) {
                int64_t frameDelta = static_cast<int64_t>(mediaFrame) - static_cast<int64_t>(lastRequested);

                if (clip.playbackMode == PlaybackMode::PingPong) {
                    // Ping-pong: only seek on huge jumps (the cache holds the
                    // working set; small back-and-forth motion is hits, not
                    // seeks). The "jump > full source length" heuristic
                    // catches user-initiated mid-cycle scrubs.
                    if (std::abs(frameDelta) > static_cast<int64_t>(sourceLength)) {
                        needsSeek = true;
                    }
                } else if (clip.playbackMode == PlaybackMode::Loop &&
                           sourceLength > 0 &&
                           frameDelta == -static_cast<int64_t>(sourceLength - 1)) {
                    // Phase 1 fix — Loop continuation wrap. The phase-driven
                    // mediaFrame just stepped from (end - 1) back to
                    // mediaStartFrame; that's not a scrub, it's a planned
                    // wrap. Issue a single one-shot seek to the start so the
                    // decoder is positioned for the next cycle's keyframe-1
                    // (~10 ms for ProRes), instead of the backward-scrub
                    // path that would clear/refill the buffer.
                    seekClip(entity, clip.mediaStartFrame);
                    worker->lastRequestedFrame.store(mediaFrame);
                    continue;
                } else {
                    constexpr int SEEK_HYSTERESIS = 8;
                    if (frameDelta < -SEEK_HYSTERESIS) {
                        needsSeek = true;  // backward scrub
                    } else if (frameDelta > DECODE_AHEAD_FRAMES + SEEK_HYSTERESIS) {
                        needsSeek = true;  // user-driven big forward jump
                    }
                    // CPU-bound lag: NO force-seek. The worker self-paces by
                    // jumping `nextFrame` to the current playhead each
                    // iteration (see decodeThreadFunc); ProResDecoder's
                    // decodeFrame uses cheap packet-skipping for those
                    // small forward jumps. User preference: smooth realtime
                    // > getting all source frames. Force-seek with
                    // avcodec_flush_buffers produced 76 ms freezes every
                    // ~300 ms; adaptive pacing produces a steady frame-drop
                    // pattern instead.
                }
            }

            worker->lastRequestedFrame.store(mediaFrame);

            if (scrubbingJustEnded) {
                needsSeek = true;
            }

            if (needsSeek) {
                std::cout << "Seek: jump from " << lastRequested << " to " << mediaFrame << std::endl;
                seekClip(entity, mediaFrame);
            }
        } else if (currentTimelineFrame < clip.startFrame) {
            // Timeline before clip start - prep for re-entry to avoid stale-frame flash
            FrameNumber lastRequested = worker->lastRequestedFrame.load();
            if (lastRequested != DecodeWorker::INVALID_FRAME &&
                lastRequested != clip.mediaStartFrame &&
                !worker->seekPending.load()) {
                seekClip(entity, clip.mediaStartFrame);
                worker->lastRequestedFrame.store(DecodeWorker::INVALID_FRAME);
            }
        }
    }

    // Sliding-window prefetch — warm decoders for clips that will start
    // within kPrefetchAheadSeconds of the current playhead so a GO,
    // continuous play-through, or cue-jump lands on a hot worker instead
    // of paying the cold FFmpeg-open + seek + first-frame cost inside the
    // seek-sync gate (ADR-0026). Editor-thread only because it writes
    // m_workers (same constraint as the bootstrap path above). Skipped
    // when Stopped (project teardown / pre-load); runs in Paused so an
    // operator who pauses, scrubs, then plays still benefits.
    if (isEditorTick &&
        m_timeline->getPlaybackState() != PlaybackState::Stopped) {
        ZoneScopedN("DecodeSystem::prefetchUpcoming");
        const double fps = std::max(1.0, m_timeline->getFrameRate());
        const FrameNumber prefetchAhead = static_cast<FrameNumber>(
            std::ceil(kPrefetchAheadSeconds * fps));
        const FrameNumber windowEnd = currentTimelineFrame + prefetchAhead;

        for (auto [entity, clip] : view.each()) {
            if (!clip.loaded) continue;
            // Strict > so already-started or active clips fall through to
            // the bootstrap / steer path above; only warm not-yet-started
            // clips inside the lookahead window. In practice the bootstrap
            // path (line ~82) already creates workers for every loaded
            // Clip+FrameBuffer entity regardless of activity, so this loop
            // is usually a no-op — kept as a safety net for late-loaded
            // clips (async MediaProbe completion mid-play, etc.).
            if (clip.startFrame <= currentTimelineFrame) continue;
            if (clip.startFrame >  windowEnd)             continue;
            if (m_workers.contains(entity))               continue;
            createWorker(entity, registry, clip.mediaStartFrame);
        }
    }

    // Emit decode backlog plot: total frames-behind across all active workers.
    // Measures how many frames each worker still needs to decode to reach
    // its target (targetFrame - currentFrame, clamped to 0). Rising steadily
    // means decoder is falling behind realtime; steady at 0 means caught up.
    {
        int64_t totalPending = 0;
        for (auto& [ent, w] : m_workers) {
            if (!w || !w->initialized.load(std::memory_order_relaxed)) continue;
            const FrameNumber target  = w->targetFrame.load(std::memory_order_relaxed);
            const FrameNumber current = w->currentFrame.load(std::memory_order_relaxed);
            if (target > current) totalPending += static_cast<int64_t>(target - current);
        }
        TracyPlot("Decode queue depth", totalPending);
    }

    // Tear down workers for entities that were destroyed. Editor-only:
    // destroyWorker calls thread.join() which blocks for the duration of
    // any in-flight decode (4K ProRes can take 50+ ms — long enough for
    // the show-thread fallback to fire and re-enter). Two threads joining
    // the same std::thread races on the OS handle and throws no_such_process.
    if (isEditorTick) {
        std::vector<entt::entity> toRemove;
        for (auto& [entity, worker] : m_workers) {
            if (!registry.valid(entity) || !registry.all_of<Clip, FrameBuffer>(entity)) {
                toRemove.push_back(entity);
            }
        }
        for (entt::entity entity : toRemove) {
            // Drop this clip's frames from the cache so they don't squat budget
            if (m_frameCache) m_frameCache->evictClip(entity);
            destroyWorker(entity);
        }
    }
}

void DecodeSystem::shutdown(entt::registry& registry) {
    (void)registry;
    std::cout << "DecodeSystem shutting down..." << std::endl;

    for (auto& [entity, worker] : m_workers) {
        if (worker && worker->running.load()) {
            worker->running.store(false);
            worker->cv.notify_all();
        }
    }
    for (auto& [entity, worker] : m_workers) {
        if (worker && worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    m_workers.clear();
    std::cout << "DecodeSystem shutdown complete" << std::endl;
}

void DecodeSystem::seekClip(entt::entity clipEntity, FrameNumber frame) {
    auto workerIt = m_workers.find(clipEntity);
    if (workerIt == m_workers.end()) return;

    DecodeWorker* worker = workerIt->second.get();
    if (!worker) return;

    if (worker->initFailed.load()) return;

    // Update target BEFORE signaling seek so the decode thread sees a valid
    // target the moment it processes the seek (no stall on stale target).
    worker->targetFrame.store(frame + DECODE_AHEAD_FRAMES);

    worker->seekTarget.store(frame);
    worker->seekPending.store(true);
    worker->cv.notify_all();
}

void DecodeSystem::pauseAll() {
    m_globalPaused.store(true);
    for (auto& [entity, worker] : m_workers) {
        if (worker) worker->paused.store(true);
    }
}

void DecodeSystem::resumeAll() {
    m_globalPaused.store(false);
    for (auto& [entity, worker] : m_workers) {
        if (worker) {
            worker->paused.store(false);
            worker->cv.notify_all();
        }
    }
}

const DecodeWorker* DecodeSystem::getWorker(entt::entity clipEntity) const {
    auto it = m_workers.find(clipEntity);
    return it != m_workers.end() ? it->second.get() : nullptr;
}

bool DecodeSystem::isClipReadyAt(entt::entity clipEntity, FrameNumber mediaFrame) const {
    const DecodeWorker* w = getWorker(clipEntity);
    if (!w) return false;
    if (w->initFailed.load(std::memory_order_relaxed)) return true;
    return w->initialized.load(std::memory_order_acquire)
        && !w->seekPending.load(std::memory_order_acquire)
        && m_frameCache && m_frameCache->has(clipEntity, mediaFrame);
}

void DecodeSystem::createWorker(entt::entity entity, entt::registry& registry, FrameNumber initialFrame) {
    auto totalStart = std::chrono::high_resolution_clock::now();

    auto* clip = registry.try_get<Clip>(entity);
    if (!clip) {
        std::cerr << "Cannot create decode worker: missing Clip component" << std::endl;
        return;
    }
    if (!m_frameCache) {
        std::cerr << "Cannot create decode worker: FrameCache not injected (Engine::initialize wiring bug)" << std::endl;
        return;
    }

    auto workerCreateStart = std::chrono::high_resolution_clock::now();
    auto worker = std::make_shared<DecodeWorker>();
    worker->cache  = m_frameCache;
    worker->pool   = m_bufferPool;  // may be nullptr; worker falls back to per-frame malloc
    worker->entity = entity;
    worker->running.store(true);
    worker->currentFrame.store(initialFrame);
    worker->targetFrame.store(initialFrame + DECODE_AHEAD_FRAMES);

    // If the main thread already opened a decoder on this entity (via
    // Engine::onMediaDroppedOnTimeline or ProjectManager::load's callback),
    // prefer the path + media type from THAT decoder — those values went
    // through ProjectManager::decoderPathFor and reflect whatever transcoded
    // HAP file the library entry points at. Falling back to clip->filepath/
    // mediaType would open the ORIGINAL (e.g. ProRes) path with a HAPDecoder,
    // which fails at the codec_id check.
    const ClipDecodeState* state = registry.try_get<ClipDecodeState>(entity);
    if (state && state->decoder && state->decoder->isOpen()) {
        worker->filepath  = state->decoder->getFilePath();
        worker->mediaType = state->decoder->getMediaType();
    } else {
        worker->filepath  = clip->filepath;
        worker->mediaType = clip->mediaType;
    }
    worker->initialized.store(false);
    worker->initFailed.store(false);

    worker->playbackMode = clip->playbackMode;
    worker->totalMediaFrames = clip->totalMediaFrames > 0 ? clip->totalMediaFrames : clip->duration;

    worker->seekTarget.store(initialFrame);
    worker->seekPending.store(true);
    auto workerCreateElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - workerCreateStart).count();

    auto threadStart = std::chrono::high_resolution_clock::now();
    worker->thread = std::thread(&DecodeSystem::decodeThreadFunc, worker);
    auto threadElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - threadStart).count();

    m_workers[entity] = worker;

    auto totalElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - totalStart).count();

    std::cout << "[TIMING] createWorker total=" << totalElapsed << "us"
              << " (workerCreate=" << workerCreateElapsed << "us"
              << ", thread=" << threadElapsed << "us)"
              << " for " << clip->filepath << std::endl;
}

void DecodeSystem::destroyWorker(entt::entity entity) {
    auto it = m_workers.find(entity);
    if (it == m_workers.end()) return;

    DecodeWorker* worker = it->second.get();
    if (worker) {
        worker->running.store(false);
        worker->cv.notify_all();
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    m_workers.erase(it);
    std::cout << "Destroyed decode worker for entity" << std::endl;
}

void DecodeSystem::destroyClipWorker(entt::entity entity) {
    // Evict cached frames keyed by this entity FIRST — otherwise a media
    // swap (SetClipMediaCommand) would serve stale frames from the old
    // source after the worker is torn down. Mirrors the per-tick
    // destroy path's eviction order in update().
    if (m_frameCache) {
        m_frameCache->evictClip(entity);
    }
    destroyWorker(entity);
}

void DecodeSystem::decodeThreadFunc(std::shared_ptr<DecodeWorker> worker) {
    if (!worker || !worker->cache) {
        std::cerr << "Decode thread started with invalid worker state" << std::endl;
        return;
    }

    tracy::SetThreadName(("Decode #" + std::to_string(
        static_cast<uint32_t>(worker->entity))).c_str());

    // Wrap the body in try/catch so a decoder bug (bad file, corrupted stream,
    // bad_alloc, unexpected FFmpeg return) doesn't tear down the whole process.
    // The clip is marked failed and playback continues without it.
    try {

    std::cout << "Decode thread started for entity (initializing decoder...)" << std::endl;

    // Deferred decoder open in the worker thread (not main) so the main loop
    // never blocks on FFmpeg's open path when a clip becomes active.
    auto decoder = createDecoder(worker->mediaType);
    if (!decoder) {
        std::cerr << "Decode thread: Failed to create decoder for media type: "
                  << static_cast<int>(worker->mediaType) << std::endl;
        worker->initFailed.store(true);
        return;
    }

    Result openResult = decoder->open(worker->filepath);
    if (openResult != Result::Success) {
        std::cerr << "Decode thread: Failed to open media file: " << worker->filepath << std::endl;
        worker->initFailed.store(true);
        return;
    }

    worker->decoder = std::move(decoder);
    worker->initialized.store(true);
    std::cout << "Decode thread: Decoder initialized for " << worker->filepath << std::endl;

    DecodedFrame frame;
    frame.allocate(worker->decoder->getWidth(), worker->decoder->getHeight());

    FrameNumber nextFrame = 0;

    while (worker->running.load()) {
        if (worker->paused.load()) {
            std::unique_lock<std::mutex> lock(worker->mutex);
            worker->cv.wait(lock, [worker]() {
                return !worker->paused.load() || !worker->running.load();
            });
            if (!worker->running.load()) break;
        }

        if (worker->seekPending.load()) {
            FrameNumber seekFrame = worker->seekTarget.load();

            // If the cache already has the seek target, no decoder seek is
            // needed — we just resume decode-ahead from after that frame.
            // This is the click-to-recently-viewed-frame fast path: zero
            // decode work, zero re-seek cost.
            if (worker->cache->has(worker->entity, seekFrame)) {
                nextFrame = seekFrame + 1;
                worker->currentFrame.store(seekFrame);
            } else {
                Result result = worker->decoder->seek(seekFrame);
                if (result == Result::Success) {
                    nextFrame = seekFrame;
                    worker->currentFrame.store(seekFrame);
                } else {
                    std::cerr << "Decode seek failed for frame " << seekFrame << std::endl;
                }
            }

            // Belt-and-braces: if the main thread hasn't bumped target past
            // nextFrame yet, do it ourselves so the decode loop has work.
            FrameNumber currentTarget = worker->targetFrame.load();
            if (nextFrame > currentTarget) {
                worker->targetFrame.store(nextFrame + DECODE_AHEAD_FRAMES);
            }
            worker->seekPending.store(false);
        }

        FrameNumber target = worker->targetFrame.load();

        if (nextFrame > target) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Adaptive realtime pacing. `target` = playhead + DECODE_AHEAD_FRAMES;
        // recover the playhead. If we've fallen behind it, jump nextFrame
        // forward so the next decoded frame matches "now," not a stale
        // sequential position. ProResDecoder::decodeFrame uses cheap
        // packet-skipping for these small forward jumps, so this stride
        // cost is bounded.
        //
        // User preference: smooth realtime > getting all source frames. At
        // 4K ProRes 4444 60fps, decoder caps at ~30fps; this pattern
        // produces evenly-spaced 30fps output instead of bursty 76 ms
        // freeze + 5-frame jump every ~300 ms.
        //
        // When decoder keeps up (HAP, smaller content), nextFrame is
        // already >= playhead and this is a no-op.
        const FrameNumber playhead =
            (target > DECODE_AHEAD_FRAMES) ? (target - DECODE_AHEAD_FRAMES) : 0;
        FrameNumber skipped = 0;
        if (nextFrame < playhead) {
            skipped = playhead - nextFrame;
            nextFrame = playhead;
        }

        // Don't decode past media duration. Loop / ping-pong wrap-around is
        // driven by DecodeSystem::update issuing seeks; the worker just
        // parks until that arrives.
        if (nextFrame >= worker->decoder->getDuration()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Skip if cache already has the frame — saves one decode + memcpy.
        // Common during ping-pong cycle when the working set is fully hot.
        if (worker->cache->has(worker->entity, nextFrame)) {
            worker->currentFrame.store(nextFrame);
            nextFrame++;
            continue;
        }

        // Acquire a pixel buffer for this iteration. The std::move into
        // the cache at the bottom of this loop empties frame.data; rather
        // than reallocating ~37.7 MB per 4K-ProRes frame from the OS heap
        // (the path that produced visible playback stutter), we pull from
        // a buffer pool that recycles buffers freed by cache eviction.
        // Cold start: pool is empty, acquire mallocs fresh; once the
        // cache fills and starts evicting, the deleters in FrameCache
        // route those buffers back to the pool, and steady-state acquire
        // is a free-list pop.
        //
        // For RGBA8 this is the obvious w*h*4; HAP's BC* paths set the
        // right size internally during decode. Conservative size here is
        // fine -- BC payloads are smaller than RGBA8 of the same w*h.
        const size_t expectedBytes = static_cast<size_t>(
            worker->decoder->getWidth()) * worker->decoder->getHeight() * 4;
        if (frame.data.size() != expectedBytes) {
            if (worker->pool) {
                frame.data = worker->pool->acquire(expectedBytes);
            } else {
                // Fallback path when no pool wired (e.g. unit tests).
                frame.data.resize(expectedBytes);
            }
        }

        // [DECODE PACE] only fires when adaptive pacing kicked in (nextFrame
        // jumped forward to match the playhead, dropping `skipped` source
        // frames). Confirms the new realtime path is exercised under load
        // and shows the steady-state stride. Quiet for HAP / fast content.
        auto decodeStart = std::chrono::high_resolution_clock::now();
        ZoneScopedN("Decode");
        ZoneValue(static_cast<uint64_t>(nextFrame));
        Result result = worker->decoder->decodeFrame(nextFrame, frame);
        if (skipped > 0) {
            auto decodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - decodeStart).count();
            std::cout << "[DECODE PACE] entity=" << static_cast<uint32_t>(worker->entity)
                      << " frame=" << nextFrame
                      << " skipped=" << skipped
                      << " ms=" << decodeMs << std::endl;
        }

        if (result == Result::Success && frame.valid.load(std::memory_order_acquire)) {
            frame.frameNumber = nextFrame;
            worker->cache->put(worker->entity, nextFrame, std::move(frame));
            // After move, frame.data is empty — top of loop re-allocates.
            worker->currentFrame.store(nextFrame);
            nextFrame++;
        } else if (result == Result::EndOfStream) {
            // FFmpeg-encoded HAP fixtures over-report nb_frames by 1 (encoder
            // buffering quirk). Park nextFrame at the duration so the guard
            // above sleeps until a seek arrives. Not an error.
            nextFrame = worker->decoder->getDuration();
        } else {
            std::cerr << "Failed to decode frame " << nextFrame << std::endl;
            nextFrame++;
        }
    }

    std::cout << "Decode thread exiting" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "[DecodeThread] Unhandled exception for " << worker->filepath
                  << ": " << e.what() << " — marking clip failed, playback continues" << std::endl;
        worker->initFailed.store(true);
        worker->running.store(false);
    } catch (...) {
        std::cerr << "[DecodeThread] Unknown exception for " << worker->filepath
                  << " — marking clip failed, playback continues" << std::endl;
        worker->initFailed.store(true);
        worker->running.store(false);
    }
}

} // namespace entity
