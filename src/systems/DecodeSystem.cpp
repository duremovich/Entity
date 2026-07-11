/**
 * DecodeSystem Implementation
 *
 * Manages background decode threads. Each clip with a FrameBuffer marker
 * gets a worker that pumps decoded frames into the engine-global FrameCache.
 */

#include "entity/systems/DecodeSystem.hpp"
#include "entity/bus/Message.hpp"
#include "entity/director/CatalogClipMath.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/timeline/PlaybackWrap.hpp"
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
#include "entity/systems/WarmSet.hpp"
#include "entity/core/Settings.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace entity {

namespace {
// Verbose per-tick / per-decode diagnostic logging.
// OFF by default — these lines (Seek: jump, [DECODE PACE]) fire dozens
// of times per second under cache pressure, both spamming the terminal
// and adding measurable cost (stdout serialization is ~0.5-2ms per line
// on Windows when sustained). Re-enable for diagnostic sessions with
// the env var `ENTITY_DECODE_VERBOSE=1`. Tracy zones / plots remain the
// canonical observation surface (FrameCache hit rate %, Decode queue
// depth, per-system zones).
bool decodeLogsEnabled() {
    static const bool enabled = []() {
        const char* v = std::getenv("ENTITY_DECODE_VERBOSE");
        return v && *v && v[0] != '0';
    }();
    return enabled;
}
} // namespace

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

    // Drain retired-but-unreaped workers — their stop signal was already sent
    // at retire time. A joinable std::thread left in the vector would call
    // std::terminate on destruction, so join each unconditionally here.
    for (auto& r : m_retiredWorkers) {
        if (r.worker && r.worker->thread.joinable()) {
            r.worker->thread.join();
        }
    }
    m_retiredWorkers.clear();
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
    // Per-tick counter of how often Fix 4 cache-miss recovery force-seeks
    // fire across all clips. Plotted at the end of this tick so the .tracy
    // file shows the thrash rate. Steady >0 == LRU eviction is undoing
    // useful decode work; targeted fix is a per-clip cache share or HW
    // decode. Pre-amendment baseline expected 0/tick on a healthy single
    // clip; ≥1/tick under multi-clip cache pressure (crossfade with 4K H.264).
    [[maybe_unused]] int64_t cacheMissRecoveryCount = 0;

    // update() is editor-only since issue #74 — the show thread's stall
    // path is tickFromSnapshot (registry-free). The old design re-entered
    // this function from the show thread, which raced project load's
    // registry mutation (EnTT view iteration during clear/destroy = UB).
    // The assert catches misuse in dev builds; the early-return keeps the
    // invariant enforced in Release (NDEBUG) too, where a rogue off-thread
    // caller would otherwise run the full lifecycle path and reintroduce
    // the #74 crash class.
    assert(std::this_thread::get_id() == m_editorThreadId &&
           "DecodeSystem::update is editor-only (#74); show thread uses tickFromSnapshot");
    if (std::this_thread::get_id() != m_editorThreadId) return;

    // Reap workers retired on a prior editor tick whose threads have now
    // exited (join returns immediately).
    reapRetiredWorkers();

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

    // Warm-set budget (spec: entity-decode-worker-budget).
    // NOTE: this block must stay in sync with AudioSystem::update's copy —
    // the two systems' Params must agree or SeekSyncController can see a
    // video-warm/audio-cold clip and hold the gate.
    std::unordered_set<entt::entity> warm;
    FrameNumber armedCueFrame = -1;
    {
        const Settings settings = activeSettings();
        warmset::Params wp;
        wp.playheadFrame    = currentTimelineFrame;
        wp.timelineFps      = m_timeline->getFrameRate();
        wp.lookaheadSeconds = settings.decodeLookaheadSeconds;
        wp.cap              = settings.decodeWorkerCap;
        wp.nowNs            = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count();
        if (auto sel = m_timeline->getSelectedCueNumber()) {
            if (const CueTag* cue = m_timeline->findCueTag(*sel)) {
                wp.armedCueFrame = cue->frame;
                armedCueFrame    = cue->frame;
            }
        }
        std::vector<warmset::ClipSpan> spans;
        spans.reserve(view.size_hint());
        for (auto [entity, clip] : view.each()) {
            if (clip.loaded) spans.push_back({entity, clip.startFrame, clip.duration});
        }
        warm = warmset::compute(spans, wp, m_lastWarmNs);
        TracyPlot("Warm decode workers", static_cast<int64_t>(warm.size()));
    }

    // Clips whose workers are actively steered this tick (authored window,
    // section-fade tail, or Fix-8 extension). The warm window only knows
    // authored spans, so a clip in a long tail/extension can fall out of
    // warm+grace while still producing output — the cold-retire sweep must
    // never take a steered worker (audio would hard-cut and video freeze
    // mid-fade).
    std::unordered_set<entt::entity> steered;

    // FrameBuffer is an empty marker type; entt elides empty components from
    // view::each's tuple, so the binding here is (entity, clip) only.
    for (auto [entity, clip] : view.each()) {
        if (!clip.loaded) continue;

        std::shared_ptr<DecodeWorker> workerPtr = findWorker(entity);
        if (!workerPtr) {
            if (!warm.contains(entity)) continue;   // budget: no worker for cold clips

            // Bootstrap a worker at the current playhead-mapped media frame.
            // A clip warmed by the armed-cue window (playhead elsewhere)
            // bootstraps at the CUE's mapped frame instead — prewarming a
            // mid-clip cue at the clip's first frame would open the decoder
            // but cache the wrong frames, so the fire still paid the full
            // seek + first-decode under the gate.
            FrameNumber initialMediaFrame = clip.mediaStartFrame;
            FrameNumber anchorFrame = -1;
            if (currentTimelineFrame >= clip.startFrame &&
                currentTimelineFrame < clip.startFrame + clip.duration) {
                anchorFrame = currentTimelineFrame;
            } else if (armedCueFrame >= clip.startFrame &&
                       armedCueFrame < clip.startFrame + clip.duration) {
                anchorFrame = armedCueFrame;
            }
            if (anchorFrame >= 0) {
                double timelineFrameRate = m_timeline->getFrameRate();
                double frameRateRatio = clip.framerate / timelineFrameRate;
                FrameNumber localFrame = anchorFrame - clip.startFrame;
                FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));
                initialMediaFrame = clip.mediaStartFrame + sourceLocalFrame;
            }
            createWorker(entity, registry, initialMediaFrame);
            workerPtr = findWorker(entity);
            if (!workerPtr) continue;
        }

        DecodeWorker* worker = workerPtr.get();

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
        const double timelineFps = m_timeline->getFrameRate();
        // 2026-05-23 follow-up — match PTA's break-aligned extension
        // semantics so DecodeSystem advances source frames through the
        // fade-window (Fix 8 + fadeSeconds cap) instead of clamping to
        // the held-tail. Without this, slot 1 in a two-clip-meeting-at-
        // fade-break setup froze visually mid-fade (operator-reported).
        const double endingBreakFadeSeconds = (tailFrames > 0)
            ? timeline::sectionFadeSecondsAtBreak(*m_timeline, clipEnd)
            : 0.0;
        const FrameNumber extendedDuration = entity::computeExtendedDuration(
            clip, timelineFps, /*endAlignsWithBreak*/ tailFrames > 0,
            endingBreakFadeSeconds);
        const FrameNumber extendedEnd = clip.startFrame + extendedDuration;
        const bool inAuthored =
            (currentTimelineFrame >= clip.startFrame &&
             currentTimelineFrame < clipEnd);
        // inExtension is the post-clipEnd region that Fix 8's extension
        // covers (zero-width when no extension applies). Frames in here
        // advance naturally, like inAuthored — they're NOT held-tail.
        const bool inExtension =
            (extendedDuration > clip.duration &&
             currentTimelineFrame >= clipEnd &&
             currentTimelineFrame < extendedEnd);
        // inTail keeps its prior meaning (post-clipEnd held-tail window)
        // but the localFrame clamp + decode-ahead suppression below now
        // ignore inTail when inExtension covers the same frame. After
        // extendedEnd, the held-tail behavior is preserved for Locked
        // and non-extended clips.
        const bool inTail =
            (tailFrames > 0 &&
             currentTimelineFrame >= clipEnd &&
             currentTimelineFrame < clipEnd + tailFrames);
        const bool inTailHeld = inTail && !inExtension;
        if (inAuthored || inExtension || inTail) {
            steered.insert(entity);
            // 2026-05-23 diagnostic — rate-limited (1 line/sec/entity)
            // per-clip state dump when the clip is active. Pairs with
            // PlaybackPresenter's [PRESENT EMPTY] log: if the
            // presenter sees an empty cache for this entity but
            // [DECODE STATE] shows it's being steered, the worker is
            // producing slowly or not at all; if no [DECODE STATE]
            // line for the entity, DecodeSystem isn't seeing it.
            if (decodeLogsEnabled()) {
                static thread_local std::unordered_map<entt::entity, int64_t> lastDecodeStateLogNs;
                const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                auto& last = lastDecodeStateLogNs[entity];
                if (nowNs - last > 1'000'000'000LL) {
                    last = nowNs;
                    std::cout << "[DECODE STATE] entity="
                              << static_cast<uint32_t>(entity)
                              << " tl=" << currentTimelineFrame
                              << " inAuth=" << (inAuthored ? 1 : 0)
                              << " inExt=" << (inExtension ? 1 : 0)
                              << " inTail=" << (inTail ? 1 : 0)
                              << " inTailHeld=" << (inTailHeld ? 1 : 0)
                              << " workerCurrent=" << worker->currentFrame.load()
                              << " seekPending=" << (worker->seekPending.load() ? 1 : 0)
                              << std::endl;
                }
            }
            // Active clip: compute target media frame using playback mode.
            // In the held-tail (inTailHeld = past extension, before tail
            // ends — Locked clips or non-extended Normal clips), clamp
            // localFrame to the last authored timeline frame so the
            // worker holds the same frame the gate predicate and
            // compositor read out of mapToMediaFrame. In the extension
            // window (inExtension), localFrame advances naturally so
            // source content keeps playing through the fade. Phase-
            // steering branches below override sourceLocalFrame for
            // continuation and post-break-anchor cases — those keep
            // their existing semantics; this clamp only matters when
            // neither fires.
            FrameNumber localFrame = inTailHeld
                ? (clip.duration - 1)
                : (currentTimelineFrame - clip.startFrame);

            double frameRateRatio = clip.framerate / timelineFps;
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
            } else if (phase && phase->postBreakMediaAnchor >= 0
                    && currentTimelineFrame >= phase->anchorTimelineFrame) {
                // Round-2 fixup, Phase 4 — post-break anchor steers the
                // decoder so the worker target tracks the same media
                // frame the presenter is mapping to. Mirrors the
                // anchor branch in PlaybackTimeAuthority::mapToMediaFrame
                // — the wrap math below (Loop / PingPong / Freeze)
                // applies the same way to this sourceLocalFrame.
                //
                // Defensive guard (2026-05-23) — only apply the anchor
                // when the playhead is at or past anchorTimelineFrame.
                // resetAnchorsAcrossScrub should clear stale anchors
                // on scrub-back and on Stop but its delta-based
                // discontinuity detector can miss slow drags / clip-
                // move workflows. With a stale anchor the math's
                // max(...,0) clamp pins worker->targetFrame at the
                // held media frame for every tick where the gate
                // condition fails — visible as a video freeze with
                // audio still playing (audio worker decodes forward
                // independently of the steered target). See parallel
                // guards in PlaybackTimeAuthority + AudioSystem.
                const double timelineDelta = static_cast<double>(
                    currentTimelineFrame - phase->anchorTimelineFrame);
                const double localFloat =
                    static_cast<double>(phase->postBreakMediaAnchor - clip.mediaStartFrame)
                    + timelineDelta * frameRateRatio;
                sourceLocalFrame = static_cast<FrameNumber>(
                    std::floor(std::max(localFloat, 0.0)));
            }

            FrameNumber sourceLength = effectivePlaybackLength(clip);
            // Shared wrap primitive (ADR-0029 D3): one call yields the
            // wrapped frame and the PingPong cycle parity the steering
            // needs (parity was previously recomputed out-of-band; on the
            // in-window leg cycle was 0, so it was always false — same as
            // the primitive's passthrough).
            const WrapResult w =
                wrapLocalFrame(clip.playbackMode, sourceLength, sourceLocalFrame);
            const FrameNumber mediaFrame = clip.mediaStartFrame + w.frame;
            const bool isReverse = w.reverse;

            if (steerWorker(entity, *worker, mediaFrame, isReverse, inTailHeld,
                            sourceLength, clip.mediaStartFrame,
                            clip.playbackMode, scrubbingJustEnded)) {
                ++cacheMissRecoveryCount;
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

    // Emit decode backlog plot: total frames-behind across all active workers.
    // Measures how many frames each worker still needs to decode to reach
    // its target (targetFrame - currentFrame, clamped to 0). Rising steadily
    // means decoder is falling behind realtime; steady at 0 means caught up.
#ifdef TRACY_ENABLE
    // Guarded: in OFF builds TracyPlot expands to nothing, and the value
    // isn't worth a whole-map lock sweep (a contention window against the
    // show thread's per-clip getWorker) to compute and discard.
    {
        int64_t totalPending = 0;
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        for (auto& [ent, w] : m_workers) {
            if (!w || !w->initialized.load(std::memory_order_relaxed)) continue;
            const FrameNumber target  = w->targetFrame.load(std::memory_order_relaxed);
            const FrameNumber current = w->currentFrame.load(std::memory_order_relaxed);
            if (target > current) totalPending += static_cast<int64_t>(target - current);
        }
        TracyPlot("Decode queue depth", totalPending);
    }
#endif
    // Steady non-zero values indicate LRU eviction is force-re-decoding
    // frames the worker just produced — the canonical multi-clip 4K H.264
    // thrash pattern. Goal under healthy load: <1/tick. Spike at section
    // break crossings or during crossfade hints at the cache budget vs
    // active-clip working set imbalance.
    TracyPlot("Cache-miss recoveries / tick", cacheMissRecoveryCount);

    // Tear down workers for entities that were destroyed. Joining a worker
    // on the editor tick blocks for the duration of any in-flight decode
    // (4K ProRes can take 50+ ms; a group delete of N clips = N serial
    // joins → visible UI freeze). retireWorker signals stop and defers the
    // join to reapRetiredWorkers() (called at the top of update()) once the
    // thread has actually exited, so the editor tick never blocks.
    {
        std::vector<entt::entity> toRemove;      // deleted entities: retire + evict cache
        std::vector<entt::entity> toCool;        // live but cold clips: retire, KEEP cache
        {
            std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
            for (auto& [entity, worker] : m_workers) {
                if (!registry.valid(entity) || !registry.all_of<Clip, FrameBuffer>(entity)) {
                    toRemove.push_back(entity);
                } else if (!warm.contains(entity) && !steered.contains(entity)) {
                    toCool.push_back(entity);
                }
            }
        }
        for (entt::entity entity : toRemove) {
            // Drop this clip's frames from the cache so they don't squat budget.
            // reapRetiredWorkers does a second evict once the thread exits, to
            // catch frames an in-flight decode lands after this point.
            if (m_frameCache) m_frameCache->evictClip(entity);
            retireWorker(entity, /*evictCacheOnReap=*/true);
        }
        for (entt::entity entity : toCool) {
            // Distance-retire: close the decoder (frees FFmpeg buffers) but
            // leave decoded frames in the LRU so re-entry hits the seek
            // fast-path (cache->has) instead of a cold first-decode.
            retireWorker(entity, /*evictCacheOnReap=*/false);
        }
    }
}

bool DecodeSystem::steerWorker(entt::entity entity, DecodeWorker& worker,
                               FrameNumber mediaFrame, bool pingPongReverse,
                               bool inTailHold, FrameNumber sourceLength,
                               FrameNumber mediaStartFrame, PlaybackMode mode,
                               bool forceSeek) {
    // Shared steering core (issue #74): everything here touches only the
    // worker's atomics and the (internally locked) FrameCache — safe from
    // both the editor tick and the show-thread stall fallback, including
    // the wake-overlap window where both run concurrently. Both callers
    // compute mediaFrame from the same timeline-frame atomic and clip
    // values, so concurrent steers differ by at most one tick — last-writer-
    // wins on the target atomics, and a duplicate requestSeek is idempotent
    // (seekPending gates pile-up; the decode thread's cache fast-path makes
    // a redundant seek nearly free).
    worker.pingPongReverse.store(pingPongReverse);

    // Decode-ahead target: in ping-pong reverse, target the current
    // mediaFrame (we need frames at and below this one); otherwise
    // run ahead by DECODE_AHEAD_FRAMES.
    //
    // Fix 6 (2026-05-23 follow-up to Fix 5) — held tail clips target
    // exactly mediaFrame, no decode-ahead. Past clipEnd there is no
    // "ahead" to decode; eight extra frames per seek would just
    // pressure the global FrameCache LRU and evict other active
    // clips' working sets — which is what caused the visible chug
    // through the fade after Fix 5 closed the freeze (~50 force-
    // seeks per 5s tail, decoder thrashing).
    //
    // 2026-05-23 follow-up — `inTailHold` instead of `inTail` so the
    // Normal-mode extension window keeps decode-ahead engaged for natural
    // realtime playback through the fade. Only the pure-held-tail case
    // (Locked clips, or Normal clips with no extension) gets the
    // no-decode-ahead clamp.
    if ((mode == PlaybackMode::PingPong && pingPongReverse) || inTailHold) {
        worker.targetFrame.store(mediaFrame);
    } else {
        worker.targetFrame.store(mediaFrame + DECODE_AHEAD_FRAMES);
    }

    // Seek-on-discontinuity. Same thresholds during scrubbing as
    // outside it — seekPending already prevents piling up seeks, so
    // running this path during a drag doesn't thrash. The previous
    // "skip seeks while scrubbing" rule produced multi-second display
    // freezes when scrubbing across big gaps because the worker had
    // to packet-skip every frame in between (ProRes ~10ms each →
    // scrub from 0 to 1000 = ~10 s of frozen display). Small forward
    // jumps still hit the packet-skip path below the threshold.
    const FrameNumber lastRequested = worker.lastRequestedFrame.load();
    bool needsSeek = false;
    bool cacheMissRecovery = false;

    // Cache-miss recovery. The worker can be ahead of mediaFrame
    // (worker.currentFrame >= mediaFrame) but the global FrameCache
    // no longer has it — LRU evicted the worker's previously-decoded
    // frames while another clip was playing. The worker won't re-
    // decode on its own because nextFrame > targetFrame from its
    // perspective; it just idles. Force a seek so it re-decodes.
    // The decode-thread fast path skips the actual seek when
    // cache.has(seekTarget) is true, so this is cheap when the
    // cache happens to be hot.
    //
    // Guard on worker.currentFrame >= mediaFrame to avoid spurious
    // seeks during normal play, when the worker is decoding TOWARD
    // mediaFrame and the cache transiently misses because the decode
    // hasn't landed yet. In that case currentFrame < mediaFrame and
    // we let the worker work.
    //
    // Fix 6 (2026-05-23) — skip cache-miss recovery for held tail
    // clips (inTailHold): the held frame's GPU texture stays resident
    // across cache misses, so re-decoding buys nothing visually and
    // creates a thrash loop under cache pressure. The extension window
    // (not held) keeps normal recovery.
    if (!inTailHold && !worker.seekPending.load() && m_frameCache &&
            !m_frameCache->has(entity, mediaFrame) &&
            worker.currentFrame.load() >= mediaFrame) {
        needsSeek = true;
        cacheMissRecovery = true;
    }

    if (!worker.seekPending.load() && lastRequested != DecodeWorker::INVALID_FRAME) {
        int64_t frameDelta = static_cast<int64_t>(mediaFrame) - static_cast<int64_t>(lastRequested);

        if (mode == PlaybackMode::PingPong) {
            // Ping-pong: only seek on huge jumps (the cache holds the
            // working set; small back-and-forth motion is hits, not
            // seeks). The "jump > full source length" heuristic
            // catches user-initiated mid-cycle scrubs.
            if (std::abs(frameDelta) > static_cast<int64_t>(sourceLength)) {
                needsSeek = true;
            }
        } else if (mode == PlaybackMode::Loop &&
                   sourceLength > 0 &&
                   frameDelta == -static_cast<int64_t>(sourceLength - 1)) {
            // Phase 1 fix — Loop continuation wrap. The phase-driven
            // mediaFrame just stepped from (end - 1) back to
            // mediaStartFrame; that's not a scrub, it's a planned
            // wrap. Issue a single one-shot seek to the start so the
            // decoder is positioned for the next cycle's keyframe-1
            // (~10 ms for ProRes), instead of the backward-scrub
            // path that would clear/refill the buffer.
            requestSeek(worker, mediaStartFrame);
            worker.lastRequestedFrame.store(mediaFrame);
            return cacheMissRecovery;
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

    worker.lastRequestedFrame.store(mediaFrame);

    if (forceSeek) {
        needsSeek = true;
    }

    if (needsSeek) {
        if (decodeLogsEnabled()) {
            std::cout << "Seek: jump from " << lastRequested
                      << " to " << mediaFrame << std::endl;
        }
        requestSeek(worker, mediaFrame);
    }
    return cacheMissRecovery;
}

void DecodeSystem::tickFromSnapshot(const bus::SceneSnapshot& scene,
                                    std::int64_t rateNowNs) {
    ZoneScopedN("DecodeSystem::tickFromSnapshot");
    if (!m_timeline) return;

    const FrameNumber currentTimelineFrame = m_timeline->getCurrentFrame();
    const double timelineFps = m_timeline->getFrameRate() > 0.0
        ? m_timeline->getFrameRate() : 30.0;

    for (const bus::ClipCatalogEntry& ce : scene.clipCatalog) {
        const entt::entity entity = static_cast<entt::entity>(
            static_cast<std::uint32_t>(ce.entity));

        // Steer only pre-existing workers; lifecycle stays editor-only.
        // A worker absent from the map (not yet bootstrapped, or already
        // retired) is simply skipped — the editor catches up on its next
        // tick. `running == false` means retire was signaled between the
        // map lookup and here; skipping avoids pointless steers on a
        // winding-down decoder (a stale steer would be harmless anyway —
        // the decode loop exits on !running regardless of seekPending).
        const std::shared_ptr<DecodeWorker> worker = findWorker(entity);
        if (!worker) continue;
        if (!worker->running.load(std::memory_order_relaxed)) continue;

        const Clip clip = clipFromCatalog(ce);

        // Active-window gate — same predicates as update()'s registry path,
        // but driven entirely by the BAKED catalog fields (no Timeline
        // section lock + vector copy per entry per tick, and guaranteed to
        // agree with mapToMediaFrameFromCatalogEx's realEnd math, which
        // uses the same baked inputs).
        const FrameNumber clipEnd = clip.startFrame + clip.duration;
        const FrameNumber tailFrames =
            catalogSectionTailFrames(ce, timelineFps);
        const FrameNumber extendedDuration = entity::computeExtendedDuration(
            clip, timelineFps, ce.endAlignsWithSectionBreak,
            ce.endingBreakFadeSeconds);
        const FrameNumber extendedEnd = clip.startFrame + extendedDuration;
        const bool inAuthored =
            (currentTimelineFrame >= clip.startFrame &&
             currentTimelineFrame < clipEnd);
        const bool inExtension =
            (extendedDuration > clip.duration &&
             currentTimelineFrame >= clipEnd &&
             currentTimelineFrame < extendedEnd);
        const bool inTail =
            (tailFrames > 0 &&
             currentTimelineFrame >= clipEnd &&
             currentTimelineFrame < clipEnd + tailFrames);
        // Continuation steering is included explicitly (a parked-at-break
        // Loop/PingPong clip must keep cycling through the stall, NEW-08);
        // mapToMediaFrameFromCatalogEx re-derives the live phase from the
        // baked wall-clock anchor + rateNowNs.
        const bool inContinuation = ce.hasPhase && ce.phase_inContinuation
            && clip.sectionBehavior == SectionBehavior::Normal;

        if (inAuthored || inExtension || inTail || inContinuation) {
            const CatalogMediaFrameResult m = mapToMediaFrameFromCatalogEx(
                ce, currentTimelineFrame, timelineFps, rateNowNs);
            const FrameNumber sourceLength = effectivePlaybackLength(clip);
            steerWorker(entity, *worker, m.mediaFrame, m.pingPongReverse,
                        m.inTailHold, sourceLength, clip.mediaStartFrame,
                        clip.playbackMode, /*forceSeek*/ false);
        } else if (currentTimelineFrame < clip.startFrame) {
            // Timeline before clip start — prep for re-entry to avoid a
            // stale-frame flash (mirror of update()'s pre-start re-arm;
            // atomics only).
            const FrameNumber lastRequested = worker->lastRequestedFrame.load();
            if (lastRequested != DecodeWorker::INVALID_FRAME &&
                lastRequested != clip.mediaStartFrame &&
                !worker->seekPending.load()) {
                requestSeek(*worker, clip.mediaStartFrame);
                worker->lastRequestedFrame.store(DecodeWorker::INVALID_FRAME);
            }
        }
    }
}

void DecodeSystem::shutdown(entt::registry& registry) {
    (void)registry;
    std::cout << "DecodeSystem shutting down..." << std::endl;

    // Swap the map into a local under the lock, then signal + join outside
    // it (leaf-lock rule: never hold m_workersMutex across join).
    std::unordered_map<entt::entity, std::shared_ptr<DecodeWorker>> workers;
    {
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        workers.swap(m_workers);
    }
    for (auto& [entity, worker] : workers) {
        if (worker && worker->running.load()) {
            worker->running.store(false);
            worker->cv.notify_all();
        }
    }
    for (auto& [entity, worker] : workers) {
        if (worker && worker->thread.joinable()) {
            worker->thread.join();
        }
    }

    // Drain any retired-but-unreaped workers unconditionally. Their stop
    // signal was already sent at retire time; join regardless of `finished`
    // (shutdown blocking is fine — the UI is going away) and evict.
    for (auto& r : m_retiredWorkers) {
        if (r.worker && r.worker->thread.joinable()) {
            r.worker->thread.join();
        }
        if (m_frameCache && r.evictOnReap) m_frameCache->evictClip(r.entity);
    }
    m_retiredWorkers.clear();
    m_lastWarmNs.clear();

    std::cout << "DecodeSystem shutdown complete" << std::endl;
}

void DecodeSystem::requestSeek(DecodeWorker& worker, FrameNumber frame) {
    if (worker.initFailed.load()) return;

    // Update target BEFORE signaling seek so the decode thread sees a valid
    // target the moment it processes the seek (no stall on stale target).
    worker.targetFrame.store(frame + DECODE_AHEAD_FRAMES);

    worker.seekTarget.store(frame);
    worker.seekPending.store(true);
    worker.cv.notify_all();
}

void DecodeSystem::seekClip(entt::entity clipEntity, FrameNumber frame) {
    std::shared_ptr<DecodeWorker> worker = findWorker(clipEntity);
    if (!worker) return;
    requestSeek(*worker, frame);
}

void DecodeSystem::pauseAll() {
    m_globalPaused.store(true);
    std::vector<std::shared_ptr<DecodeWorker>> workers;
    {
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        workers.reserve(m_workers.size());
        for (auto& [entity, worker] : m_workers) {
            if (worker) workers.push_back(worker);
        }
    }
    for (auto& worker : workers) worker->paused.store(true);
}

void DecodeSystem::resumeAll() {
    m_globalPaused.store(false);
    std::vector<std::shared_ptr<DecodeWorker>> workers;
    {
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        workers.reserve(m_workers.size());
        for (auto& [entity, worker] : m_workers) {
            if (worker) workers.push_back(worker);
        }
    }
    for (auto& worker : workers) {
        worker->paused.store(false);
        worker->cv.notify_all();
    }
}

std::shared_ptr<DecodeWorker> DecodeSystem::findWorker(entt::entity entity) const {
    std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
    auto it = m_workers.find(entity);
    return it != m_workers.end() ? it->second : nullptr;
}

std::shared_ptr<const DecodeWorker> DecodeSystem::getWorker(entt::entity clipEntity) const {
    return findWorker(clipEntity);
}

bool DecodeSystem::isClipReadyAt(entt::entity clipEntity, FrameNumber mediaFrame) const {
    const std::shared_ptr<const DecodeWorker> w = getWorker(clipEntity);
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
    worker->cache     = m_frameCache;
    worker->allocator = m_bufferAllocator; // may be nullptr; worker falls back to per-frame malloc
    worker->entity   = entity;
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

    worker->seekTarget.store(initialFrame);
    worker->seekPending.store(true);
    auto workerCreateElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - workerCreateStart).count();

    auto threadStart = std::chrono::high_resolution_clock::now();
    worker->thread = std::thread(&DecodeSystem::decodeThreadFunc, worker);
    auto threadElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - threadStart).count();

    {
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        m_workers[entity] = worker;
    }

    auto totalElapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - totalStart).count();

    std::cout << "[TIMING] createWorker total=" << totalElapsed << "us"
              << " (workerCreate=" << workerCreateElapsed << "us"
              << ", thread=" << threadElapsed << "us)"
              << " for " << clip->filepath << std::endl;
}

void DecodeSystem::destroyWorker(entt::entity entity) {
    // Extract under the lock; signal + join outside it (leaf-lock rule —
    // the join can block 50+ ms on 4K ProRes and must not stall show-thread
    // getWorker lookups).
    std::shared_ptr<DecodeWorker> worker;
    {
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        auto it = m_workers.find(entity);
        if (it == m_workers.end()) return;
        worker = std::move(it->second);
        m_workers.erase(it);
    }
    if (worker) {
        worker->running.store(false);
        worker->cv.notify_all();
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    std::cout << "Destroyed decode worker for entity" << std::endl;
}

void DecodeSystem::retireWorker(entt::entity entity, bool evictCacheOnReap) {
    std::shared_ptr<DecodeWorker> worker;
    {
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        auto it = m_workers.find(entity);
        if (it == m_workers.end()) return;
        worker = std::move(it->second);
        m_workers.erase(it);
    }
    if (worker) {
        // Signal stop but DO NOT join here — the thread may be mid-decode
        // (50+ ms on 4K ProRes). reapRetiredWorkers() joins on a later tick
        // once `finished` is set, so the join returns immediately.
        worker->running.store(false);
        worker->cv.notify_all();
    }
    m_retiredWorkers.push_back({entity, std::move(worker), evictCacheOnReap});
}

void DecodeSystem::reapRetiredWorkers() {
    for (auto it = m_retiredWorkers.begin(); it != m_retiredWorkers.end();) {
        auto& worker = it->worker;
        if (worker && worker->finished.load(std::memory_order_acquire)) {
            if (worker->thread.joinable()) {
                worker->thread.join();  // thread already exited; immediate
            }
            // Second evict (delete-retires only): an in-flight decode may have
            // landed frames in the cache AFTER the evict that ran at retire
            // time. Safe to fire after the entity is destroyed — FrameCache
            // keys by the full entt::entity handle (index + version), so a
            // recycled clip can't be evicted by a stale handle (see
            // FrameCache::Key). Distance-retires (evictOnReap=false) keep
            // their frames: the clip is alive, re-entry rides the seek
            // fast-path, and a re-warmed clip's NEW worker may already be
            // refilling the cache this reap must not clobber.
            if (m_frameCache && it->evictOnReap) m_frameCache->evictClip(it->entity);
            it = m_retiredWorkers.erase(it);
        } else if (!worker) {
            // Defensive: a null slot (shouldn't happen) — drop it.
            it = m_retiredWorkers.erase(it);
        } else {
            ++it;
        }
    }
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

    // Mark `finished` as the thread's very last act on EVERY exit path
    // (early-return decoder-open failures, normal loop exit, and the
    // try/catch below). reapRetiredWorkers() gates join() on this flag so
    // the editor tick never blocks on an in-flight decode. The guard sits
    // after the worker null-check above so it can safely touch worker->.
    struct FinishedGuard {
        DecodeWorker* w;
        ~FinishedGuard() { w->finished.store(true, std::memory_order_release); }
    } finishedGuard{worker.get()};

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

    // Wire the allocator before open() so the decoder uses the correct backing
    // store (CPU heap or D3D12 UPLOAD heap) from the very first decodeFrame call.
    decoder->setAllocator(worker->allocator);

    Result openResult = decoder->open(worker->filepath);
    if (openResult != Result::Success) {
        std::cerr << "Decode thread: Failed to open media file: " << worker->filepath << std::endl;
        worker->initFailed.store(true);
        return;
    }

    worker->decoder = std::move(decoder);
    worker->initialized.store(true);
    std::cout << "Decode thread: Decoder initialized for " << worker->filepath << std::endl;

    // Pre-allocate the scratch frame via the allocator (same backing store as
    // the decoder will use). Falls back to CpuHeap when no allocator is wired.
    DecodedFrame frame;
    if (worker->allocator) {
        worker->allocator->prepareDecodeBuffer(frame,
                                               worker->decoder->getWidth(),
                                               worker->decoder->getHeight(),
                                               TextureFormat::RGBA8_UNORM);
    } else {
        frame.allocate(worker->decoder->getWidth(), worker->decoder->getHeight());
    }

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
        // Ensure the scratch frame has a backing buffer of the right dimensions.
        // The allocator handles both CpuHeap (tight-packed) and UploadHeap
        // (256-byte-aligned RowPitch) paths, including reuse when dimensions
        // haven't changed (the common case). Fallback path when no allocator
        // is wired (e.g. unit tests) retains the old size-check behaviour.
        const uint32_t decW = worker->decoder->getWidth();
        const uint32_t decH = worker->decoder->getHeight();
        if (worker->allocator) {
            worker->allocator->prepareDecodeBuffer(frame, decW, decH,
                                                   TextureFormat::RGBA8_UNORM);
        } else if (frame.size() < static_cast<size_t>(decW) * decH * 4) {
            // Fallback path when no allocator wired (e.g. unit tests).
            frame.storage = DecodedFrame::Storage::CpuHeap;
            frame.cpuData.resize(static_cast<size_t>(decW) * decH * 4);
        }

        // [DECODE PACE] only fires when adaptive pacing kicked in (nextFrame
        // jumped forward to match the playhead, dropping `skipped` source
        // frames). Gated behind ENTITY_DECODE_VERBOSE — Tracy zones are
        // the canonical observation surface for this; the stdout line was
        // diagnostic-only.
        auto decodeStart = std::chrono::high_resolution_clock::now();
        ZoneScopedN("Decode");
        ZoneValue(static_cast<uint64_t>(nextFrame));
        Result result = worker->decoder->decodeFrame(nextFrame, frame);
        if (skipped > 0 && decodeLogsEnabled()) {
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
