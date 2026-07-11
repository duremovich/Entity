#include "entity/systems/AudioSystem.hpp"
#include "entity/audio/AudioEngine.hpp"
#include "entity/audio/AudioMixer.hpp"
#include "entity/bus/Message.hpp"
#include "entity/director/CatalogClipMath.hpp"
#include "entity/components/AudioSource.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/timeline/PlaybackWrap.hpp"
#include "entity/timeline/SectionFade.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/systems/WarmSet.hpp"
#include "entity/core/Settings.hpp"
#include "entity/profile/Tracy.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace entity {

AudioSystem::AudioSystem() = default;

AudioSystem::~AudioSystem() {
    for (auto& [entity, worker] : m_workers) {
        if (worker) {
            worker->running.store(false);
            if (worker->thread.joinable())
                worker->thread.join();
        }
    }
    m_workers.clear();

    // Drain retired-but-unreaped workers — a joinable std::thread left in the
    // vector would call std::terminate on destruction. Stop already signaled
    // at retire time; join unconditionally.
    for (auto& [entity, worker] : m_retiredWorkers) {
        if (worker && worker->thread.joinable())
            worker->thread.join();
    }
    m_retiredWorkers.clear();
}

void AudioSystem::initialize(entt::registry& registry) {
    (void)registry;
    m_editorThreadId = std::this_thread::get_id();
}

void AudioSystem::createWorker(entt::entity e, entt::registry& registry) {
    if (!m_audioEngine) return;
    const Clip* clip = registry.try_get<Clip>(e);
    const AudioSource* as = registry.try_get<AudioSource>(e);
    if (!clip || !as) return;

    const int rate = m_audioEngine->sampleRate();
    auto worker = std::make_shared<AudioDecodeWorker>(rate);

    // Resolve the media path the same way DecodeSystem does (DecodeSystem.cpp).
    // clip->filepath is only a logical/relative reference (e.g.
    // "content/foo.mp4"); a project-loaded clip's decoder was opened by
    // ProjectManager's load callback through decoderPathFor() and carries the
    // resolved absolute path. Passing the raw relative path to AudioDecoder
    // fails — avformat_open_input resolves it against the process working
    // directory, not the project directory.
    worker->filepath = clip->filepath;
    if (const ClipDecodeState* st = registry.try_get<ClipDecodeState>(e)) {
        if (st->decoder && st->decoder->isOpen())
            worker->filepath = st->decoder->getFilePath();
    }
    worker->playbackMode.store(clip->playbackMode, std::memory_order_relaxed);

    // Convert Clip media frame in/out-points to output-rate samples.
    const double fps = clip->framerate > 0.0 ? clip->framerate : 30.0;
    worker->inPointSample  = static_cast<int64_t>(
        std::round(clip->mediaStartFrame / fps * rate));
    const int64_t outFrame = (clip->mediaOutFrame >= clip->mediaStartFrame)
        ? static_cast<int64_t>(clip->mediaOutFrame) + 1
        : static_cast<int64_t>(clip->totalMediaFrames);
    worker->outPointSample = static_cast<int64_t>(
        std::round(outFrame / fps * rate));
    if (worker->outPointSample <= worker->inPointSample)
        worker->outPointSample = worker->inPointSample + 1;

    worker->mixSource.gain.store(as->gain);
    worker->mixSource.mute.store(as->mute);
    worker->mixSource.solo.store(as->solo);
    worker->mixSource.ring = &worker->ring;

    if (m_audioEngine)
        m_audioEngine->mixer().registerSource(&worker->mixSource);

    worker->running.store(true);
    worker->thread = std::thread(&audioDecodeThreadFunc, worker);
    {
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        m_workers.emplace(e, std::move(worker));
    }
}

void AudioSystem::destroyWorker(entt::entity e) {
    // Extract under the lock; join + unregister outside it (leaf-lock rule —
    // the join can block on an in-flight decode and must not stall
    // show-thread lookups).
    std::shared_ptr<AudioDecodeWorker> worker;
    {
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        auto it = m_workers.find(e);
        if (it == m_workers.end()) return;
        worker = std::move(it->second);
        m_workers.erase(it);
    }
    if (worker) {
        worker->running.store(false);
        if (worker->thread.joinable())
            worker->thread.join();
        if (m_audioEngine)
            m_audioEngine->mixer().unregisterSource(&worker->mixSource);
    }
}

void AudioSystem::retireWorker(entt::entity e) {
    std::shared_ptr<AudioDecodeWorker> worker;
    {
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        auto it = m_workers.find(e);
        if (it == m_workers.end()) return;
        worker = std::move(it->second);
        m_workers.erase(it);
    }
    if (worker) {
        // Unregister the mix source NOW so the audio callback stops pulling
        // from this worker's ring immediately (the decode thread may still be
        // writing the ring until it observes running=false, but nothing reads
        // it once unregistered). Then signal stop and defer the join to
        // reapRetiredWorkers() — joining live here could block the editor tick
        // on an in-flight decode.
        if (m_audioEngine)
            m_audioEngine->mixer().unregisterSource(&worker->mixSource);
        worker->running.store(false);
    }
    m_retiredWorkers.emplace_back(e, std::move(worker));
}

void AudioSystem::reapRetiredWorkers() {
    for (auto it = m_retiredWorkers.begin(); it != m_retiredWorkers.end();) {
        auto& worker = it->second;
        if (!worker) {
            it = m_retiredWorkers.erase(it);
        } else if (worker->finished.load(std::memory_order_acquire)) {
            if (worker->thread.joinable())
                worker->thread.join();  // thread already exited; immediate
            it = m_retiredWorkers.erase(it);
        } else {
            ++it;
        }
    }
}

void AudioSystem::update(entt::registry& registry, float /*deltaTime*/) {
    ZoneScopedN("AudioSystem");
    if (!m_timeline || !m_audioEngine) return;

    // update() is editor-only since issue #74 — the show thread's stall
    // path is tickFromSnapshot (registry-free). Assert for dev builds;
    // early-return keeps the invariant enforced in Release (NDEBUG).
    assert(std::this_thread::get_id() == m_editorThreadId &&
           "AudioSystem::update is editor-only (#74); show thread uses tickFromSnapshot");
    if (std::this_thread::get_id() != m_editorThreadId) return;

    // Reap audio workers retired on a prior editor tick whose threads have
    // now exited (join returns immediately).
    reapRetiredWorkers();

    const FrameNumber currentTLFrame = m_timeline->getCurrentFrame();
    const double tlFPS = m_timeline->getFrameRate() > 0.0
        ? m_timeline->getFrameRate() : 30.0;
    const int rate = m_audioEngine->sampleRate();
    // Stamped into each steered worker's lastExpectedSampleNs so the
    // discontinuity threshold can scale with the real inter-tick gap.
    const int64_t steeringNowNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

    auto view = registry.view<Clip, AudioSource>();

    // Warm-set budget (spec: entity-decode-worker-budget). Same gate as
    // DecodeSystem but over audio-bearing clips only — audio workers get
    // their own decodeWorkerCap budget, separate from video.
    // NOTE: this block must stay in sync with DecodeSystem::update's copy —
    // the two systems' Params must agree or SeekSyncController can see a
    // video-warm/audio-cold clip and hold the gate.
    std::unordered_set<entt::entity> warm;
    {
        const Settings settings = activeSettings();
        warmset::Params wp;
        wp.playheadFrame    = currentTLFrame;
        wp.timelineFps      = tlFPS;
        wp.lookaheadSeconds = settings.decodeLookaheadSeconds;
        wp.cap              = settings.decodeWorkerCap;
        wp.nowNs            = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count();
        if (auto sel = m_timeline->getSelectedCueNumber()) {
            if (const CueTag* cue = m_timeline->findCueTag(*sel)) {
                wp.armedCueFrame = cue->frame;
            }
        }
        std::vector<warmset::ClipSpan> spans;
        spans.reserve(view.size_hint());
        for (auto [entity, clip, as] : view.each()) {
            (void)as;
            if (clip.loaded) spans.push_back({entity, clip.startFrame, clip.duration});
        }
        warm = warmset::compute(spans, wp, m_lastWarmNs);
        TracyPlot("Warm audio workers", static_cast<int64_t>(warm.size()));
    }

    // Clips whose workers are steered this tick (authored window, fade tail,
    // extension, or section-break continuation). The warm window only knows
    // authored spans; retiring a steered audio worker unregisters its mixer
    // source — an audible hard cut mid-fade. The cold-retire sweep skips them.
    std::unordered_set<entt::entity> steered;

    for (auto [entity, clip, as] : view.each()) {
        if (!clip.loaded) continue;

        std::shared_ptr<AudioDecodeWorker> worker = findWorker(entity);
        if (!worker) {
            if (!warm.contains(entity)) continue;   // budget: no worker for cold clips
            createWorker(entity, registry);
            worker = findWorker(entity);
            if (!worker) continue;
        }

        // Mirror gain/mute/solo each tick.
        worker->mixSource.gain.store(as.gain);
        worker->mixSource.mute.store(as.mute);
        worker->mixSource.solo.store(as.solo);

        // Reflect the worker's decoder result back onto the component: the
        // AudioDecoder is the source of truth for whether the media really
        // has an audio stream. Registry write — fine here since update() is
        // editor-only (#74). Keeps hasAudioStream correct regardless of how
        // the clip was created, so the PropertyWindow audio panel appears
        // and ProjectSerializer persists gain/mute/solo on the next save.
        if (!as.hasAudioStream
                && worker->initialized.load(std::memory_order_relaxed)) {
            as.hasAudioStream = true;
        }

        // Keep playback mode in sync (atomic — the decode thread reads it
        // for wrap decisions).
        worker->playbackMode.store(clip.playbackMode, std::memory_order_relaxed);

        // Determine whether this clip should be steering its worker.
        // A clip steers (seeks/prerolls) whenever the playhead is inside the
        // clip window — playing OR paused — OR while the clip is in
        // section-break continuation. Steering is decoupled from transport
        // state so the worker repositions and prerolls its ring at the parked
        // playhead while paused, exactly as the video DecodeSystem does. That
        // way a seek-then-Play does not stall on a cold audio seek + preroll
        // under the seek-sync gate (the user-visible "several-second pause").
        // Audible *output* stays gated on `playing` below (mixSource.active).
        // This is independent of the seek-sync gate so the worker seeks and
        // prerolls its ring even while the gate holds the timeline still.
        const bool playing =
            (m_timeline->getPlaybackState() == PlaybackState::Playing);
        const FrameNumber clipEnd = clip.startFrame + clip.duration;
        const bool inAuthored = (currentTLFrame >= clip.startFrame &&
                                 currentTLFrame < clipEnd);
        // Fix 5 (2026-05-23 follow-up) — extend steering to the section
        // fade tail so audio holds the same sample the video compositor
        // holds during the fade, instead of cutting hard at clipEnd
        // while video continues fading. tailFrames is 0 outside section
        // breaks, so this is a no-op for clips not aligned with one.
        const FrameNumber tailFrames =
            timeline::sectionFadeTailFrames(*m_timeline, clipEnd);
        // 2026-05-23 follow-up — Normal-mode break-aligned extension
        // (see Clip.hpp::computeExtendedDuration). The extension window
        // advances source frames naturally during the visible fade; audio
        // should advance in lockstep instead of held-clamping, so we
        // separate `inExtension` from `inTailHeld` exactly like
        // DecodeSystem does.
        const double endingBreakFadeSeconds = (tailFrames > 0)
            ? timeline::sectionFadeSecondsAtBreak(*m_timeline, clipEnd)
            : 0.0;
        const FrameNumber extendedDuration = entity::computeExtendedDuration(
            clip, tlFPS, /*endAlignsWithBreak*/ tailFrames > 0,
            endingBreakFadeSeconds);
        const FrameNumber extendedEnd = clip.startFrame + extendedDuration;
        const bool inExtension =
            (extendedDuration > clip.duration &&
             currentTLFrame >= clipEnd &&
             currentTLFrame < extendedEnd);
        const bool inTail = (tailFrames > 0 &&
                             currentTLFrame >= clipEnd &&
                             currentTLFrame < clipEnd + tailFrames);
        const bool inTailHeld = inTail && !inExtension;
        const bool inWindow = inAuthored || inExtension || inTail;
        bool inContinuation = false;
        if (clip.sectionBehavior == SectionBehavior::Normal) {
            const ClipPlaybackPhase* phase = registry.try_get<ClipPlaybackPhase>(entity);
            if (phase && phase->inContinuation)
                inContinuation = true;
        }
        // Steering: position/preroll the worker whenever the playhead is in
        // the clip window (playing OR paused) or in continuation.
        const bool shouldSteer = inWindow || inContinuation;
        if (shouldSteer) steered.insert(entity);

        // mixSource.active gates the mixer: the clip is *audible* only when it
        // would be playing audio (in-window AND transport Playing, or in
        // continuation) AND the seek-sync gate is not holding the timeline.
        // This stays gated on transport state exactly as before — steering
        // above is now decoupled, but audible output is not: no sound while
        // paused. When the gate is set, the worker still seeks/prerolls but
        // the mixer outputs silence until SeekSyncController releases.
        const bool shouldOutput = (inWindow && playing) || inContinuation;
        worker->mixSource.active.store(
            shouldOutput && !m_timeline->isSeekSyncGated());

        if (!shouldSteer) continue;

        // Compute expected clip-local output-rate sample, mirroring
        // DecodeSystem's timeline→media-frame mapping.
        // Fix 5 — in the section fade tail, clamp localTLFrame to the
        // last authored timeline frame so the worker steers toward the
        // held sample rather than advancing past clipEnd. Phase-steering
        // branches below override sourceLocalFrame for continuation and
        // post-break-anchor cases (they keep their existing semantics);
        // this clamp matters only when neither fires.
        //
        // 2026-05-23 follow-up — `inTailHeld` instead of `inTail` so the
        // Normal-mode extension window (inExtension covers it) advances
        // naturally and audio plays through the fade in lockstep with
        // video.
        const double frameRateRatio = clip.framerate / tlFPS;
        FrameNumber localTLFrame = inTailHeld
            ? (clip.duration - 1)
            : (currentTLFrame - clip.startFrame);
        FrameNumber sourceLocalFrame = static_cast<FrameNumber>(
            std::floor(localTLFrame * frameRateRatio));

        // Phase-steering (mirrors DecodeSystem phase path).
        const ClipPlaybackPhase* phase = registry.try_get<ClipPlaybackPhase>(entity);
        if (phase && phase->inContinuation
                && clip.sectionBehavior == SectionBehavior::Normal) {
            sourceLocalFrame = static_cast<FrameNumber>(
                std::floor(std::max(phase->sourcePhaseFrames, 0.0)));
        } else if (phase && phase->postBreakMediaAnchor >= 0
                && currentTLFrame >= phase->anchorTimelineFrame) {
            // Defensive guard (2026-05-23) — only apply the anchor
            // when the playhead is at or past anchorTimelineFrame. See
            // parallel guards in PlaybackTimeAuthority + DecodeSystem
            // for the full rationale.
            const double timelineDelta = static_cast<double>(
                currentTLFrame - phase->anchorTimelineFrame);
            const double localFloat =
                static_cast<double>(phase->postBreakMediaAnchor - clip.mediaStartFrame)
                + timelineDelta * frameRateRatio;
            sourceLocalFrame = static_cast<FrameNumber>(
                std::floor(std::max(localFloat, 0.0)));
        }

        const FrameNumber sourceLength = effectivePlaybackLength(clip);
        if (sourceLength <= 0) continue;

        // Wrap sourceLocalFrame per PlaybackMode. This site is already
        // source-local (the sample conversion below needs no
        // mediaStartFrame offset), so the shared primitive's result is
        // used directly; audio never reverse-decodes, so parity is unused.
        const FrameNumber mediaLocalFrame =
            wrapLocalFrame(clip.playbackMode, sourceLength, sourceLocalFrame).frame;

        // Convert media frame to clip-local output-rate sample.
        const double fps = clip.framerate > 0.0 ? clip.framerate : 30.0;
        const int64_t expectedSample = static_cast<int64_t>(
            std::round(mediaLocalFrame / fps * rate));

        steerAudioWorker(*worker, expectedSample, steeringNowNs, rate, tlFPS);
    }


    // Tear down workers for entities that are gone. retireWorker signals
    // stop + unregisters the mixer source but defers the thread join to
    // reapRetiredWorkers() (called at the top of update()), so a group
    // delete of N audio-bearing clips doesn't serialize N blocking joins on
    // one editor tick — the delete-freeze fix.
    {
        // One retire path covers both deleted entities and live-but-cold
        // clips (warm-set distance-retires) — unlike DecodeSystem there is
        // no cache-eviction split; audio rings die with the worker either
        // way. Steered clips (fade tail / extension / continuation) are
        // never cold-retired even when outside the warm window.
        std::vector<entt::entity> toRetire;
        {
            std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
            for (auto& [ent, w] : m_workers) {
                const bool deleted =
                    !registry.valid(ent) || !registry.all_of<Clip, AudioSource>(ent);
                if (deleted || (!warm.contains(ent) && !steered.contains(ent))) {
                    toRetire.push_back(ent);
                }
            }
        }
        for (entt::entity ent : toRetire) {
            retireWorker(ent);
        }
    }
}

void AudioSystem::steerAudioWorker(AudioDecodeWorker& w, int64_t expectedSample,
                                   int64_t nowNs, int rate, double tlFPS) {
    // Seek detection: discontinuity on the controller side. We compare
    // expectedSample to w.lastExpectedSample (the value the steering path
    // computed on its previous tick), not to the decode cursor — immune to
    // the decode/playback ring-fill offset. A genuine scrub/seek jumps by
    // far more than playback advances between two steering ticks.
    //
    // Gap-aware discontinuity test (issue #74): pre-#74 the threshold was a
    // fixed 3 editor ticks' worth of samples, which assumed steering ticks
    // are back-to-back. The show-thread stall fallback only engages after
    // the heartbeat is >50 ms stale — at a 60 fps timeline 3 ticks IS 50 ms,
    // so the fallback's first tick always mis-read normal playback advance
    // as a scrub and re-seeked (ring.clear() → audible dropout on every
    // stall entry).
    //
    // The gap between two steering ticks admits exactly two legitimate
    // histories: the playhead advanced through the clip the whole time
    // (stall handoff — expected ≈ last + gap×rate) or it didn't advance at
    // all (paused / just re-entered after idling — expected ≈ last). Accept
    // the new position if EITHER hypothesis explains it within the 3-tick
    // jitter slack; anything else is a genuine reposition and must seek.
    // A naive `slack + elapsedSamples` allowance would also absorb real
    // scrubs into a clip whose worker sat unsteered for the gap (warm but
    // out-of-window), leaving audio at the old offset permanently.
    if (!w.seekPending.load()) {
        const int64_t last = w.lastExpectedSample.load(std::memory_order_relaxed);
        const bool firstActiveTick = (last == AudioDecodeWorker::kNoExpectedSample);
        bool seekNeeded = firstActiveTick;
        if (!firstActiveTick) {
            const int64_t threeTicksSamples = static_cast<int64_t>(
                std::ceil(static_cast<double>(rate) / tlFPS * 3.0));
            const int64_t lastNs = w.lastExpectedSampleNs.load(std::memory_order_relaxed);
            const int64_t elapsedSamples = (lastNs > 0 && nowNs > lastNs)
                ? static_cast<int64_t>(std::ceil(
                      static_cast<double>(nowNs - lastNs) * 1e-9 * rate))
                : 0;
            const int64_t deltaStatic     = std::abs(expectedSample - last);
            const int64_t deltaContinuous =
                std::abs(expectedSample - (last + elapsedSamples));
            seekNeeded =
                (std::min(deltaStatic, deltaContinuous) > threeTicksSamples);
        }
        if (seekNeeded) {
            const int64_t clampedSample = std::clamp<int64_t>(
                expectedSample, 0,
                w.outPointSample - w.inPointSample - 1);
            w.seekTarget.store(clampedSample);
            w.seekPending.store(true);
            w.seekCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
    w.lastExpectedSample.store(expectedSample, std::memory_order_relaxed);
    w.lastExpectedSampleNs.store(nowNs, std::memory_order_relaxed);
}

void AudioSystem::tickFromSnapshot(const bus::SceneSnapshot& scene,
                                   std::int64_t rateNowNs) {
    ZoneScopedN("AudioSystem::tickFromSnapshot");
    if (!m_timeline || !m_audioEngine) return;

    const FrameNumber currentTLFrame = m_timeline->getCurrentFrame();
    const double tlFPS = m_timeline->getFrameRate() > 0.0
        ? m_timeline->getFrameRate() : 30.0;
    const int rate = m_audioEngine->sampleRate();
    const int64_t nowNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool playing =
        (m_timeline->getPlaybackState() == PlaybackState::Playing);
    const bool seekSyncGated = m_timeline->isSeekSyncGated();

    // Coverage note (issue #74): the catalog is baked from
    // view<Clip, VideoTexture> with an allocated slot, so an audio-bearing
    // clip whose slot isn't provisioned yet (~a frame after placement) is
    // invisible here. Accepted — slot provisioning completes within a frame
    // and the editor path covers it outside stalls. See ADR-0014.
    for (const bus::ClipCatalogEntry& ce : scene.clipCatalog) {
        const entt::entity entity = static_cast<entt::entity>(
            static_cast<std::uint32_t>(ce.entity));

        const std::shared_ptr<AudioDecodeWorker> worker = findWorker(entity);
        if (!worker) continue;
        if (!worker->running.load(std::memory_order_relaxed)) continue;

        const Clip clip = clipFromCatalog(ce);

        // Window/continuation gate — same shape as update()'s registry
        // path, but driven entirely by the BAKED catalog fields (no
        // Timeline section lock + vector copy per entry per tick, and
        // guaranteed to agree with mapToMediaFrameFromCatalogEx's realEnd
        // math, which uses the same baked inputs).
        const FrameNumber clipEnd = clip.startFrame + clip.duration;
        const FrameNumber tailFrames =
            catalogSectionTailFrames(ce, tlFPS);
        const FrameNumber extendedDuration = entity::computeExtendedDuration(
            clip, tlFPS, ce.endAlignsWithSectionBreak,
            ce.endingBreakFadeSeconds);
        const FrameNumber extendedEnd = clip.startFrame + extendedDuration;
        const bool inAuthored = (currentTLFrame >= clip.startFrame &&
                                 currentTLFrame < clipEnd);
        const bool inExtension =
            (extendedDuration > clip.duration &&
             currentTLFrame >= clipEnd &&
             currentTLFrame < extendedEnd);
        const bool inTail = (tailFrames > 0 &&
                             currentTLFrame >= clipEnd &&
                             currentTLFrame < clipEnd + tailFrames);
        const bool inWindow = inAuthored || inExtension || inTail;
        const bool inContinuation = ce.hasPhase && ce.phase_inContinuation
            && clip.sectionBehavior == SectionBehavior::Normal;

        // Drive audibility through the stall — same formula as update().
        // NOT optional: a Loop clip that ends mid-stall would otherwise keep
        // filling its ring and stay audible for the whole stall; a clip that
        // starts mid-stall would stay silent. Gain/mute/solo mirroring and
        // the hasAudioStream registry write stay editor-only (the mixSource
        // atomics hold their last-mirrored values through a stall).
        const bool shouldOutput = (inWindow && playing) || inContinuation;
        worker->mixSource.active.store(shouldOutput && !seekSyncGated);

        if (!inWindow && !inContinuation) continue;

        // Media-frame mapping via the shared catalog math (identical to what
        // buildRenderFrame displays), then to a clip-local output-rate
        // sample. Sample rate + in/out points live on the worker (set before
        // its thread started) — the snapshot carries no audio fields and
        // needs none.
        const CatalogMediaFrameResult m = mapToMediaFrameFromCatalogEx(
            ce, currentTLFrame, tlFPS, rateNowNs);
        const FrameNumber mediaLocalFrame = m.mediaFrame - clip.mediaStartFrame;
        const double fps = clip.framerate > 0.0 ? clip.framerate : 30.0;
        const int64_t expectedSample = static_cast<int64_t>(
            std::round(mediaLocalFrame / fps * rate));

        steerAudioWorker(*worker, expectedSample, nowNs, rate, tlFPS);
    }
}

std::shared_ptr<AudioDecodeWorker> AudioSystem::findWorker(entt::entity e) const {
    std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
    auto it = m_workers.find(e);
    return it != m_workers.end() ? it->second : nullptr;
}

int64_t AudioSystem::getWorkerSeekTargetFrame(entt::entity clipEntity,
                                               double clipFps) const {
    const std::shared_ptr<AudioDecodeWorker> worker = findWorker(clipEntity);
    if (!worker) return -1;
    const AudioDecodeWorker& w = *worker;
    if (w.initFailed.load(std::memory_order_relaxed)) return 0;
    if (!w.initialized.load(std::memory_order_acquire)) return -1;
    const double fps = (clipFps > 0.0) ? clipFps : 30.0;
    const int64_t sampleTarget = w.seekTarget.load(std::memory_order_relaxed);
    return static_cast<int64_t>(
        std::round(static_cast<double>(sampleTarget) / w.targetSampleRate * fps));
}

int64_t AudioSystem::getWorkerSeekCount(entt::entity clipEntity) const {
    const std::shared_ptr<AudioDecodeWorker> worker = findWorker(clipEntity);
    if (!worker) return -1;
    return static_cast<int64_t>(
        worker->seekCount.load(std::memory_order_relaxed));
}

bool AudioSystem::isWorkerSeekReady(entt::entity clipEntity) const {
    const std::shared_ptr<AudioDecodeWorker> worker = findWorker(clipEntity);
    if (!worker) return false;
    const AudioDecodeWorker& w = *worker;
    if (w.initFailed.load(std::memory_order_relaxed)) return true;
    return w.initialized.load(std::memory_order_acquire)
        && !w.seekPending.load(std::memory_order_acquire)
        && w.ring.availableFrames() >= kAudioPrerollFrames;
}

void AudioSystem::shutdown(entt::registry& registry) {
    (void)registry;
    // Swap the map into a local under the lock; signal/join/unregister
    // outside it (leaf-lock rule).
    std::unordered_map<entt::entity, std::shared_ptr<AudioDecodeWorker>> workers;
    {
        std::lock_guard<LockableBase(std::mutex)> lock(m_workersMutex);
        workers.swap(m_workers);
    }
    for (auto& [entity, worker] : workers) {
        if (worker) {
            worker->running.store(false);
        }
    }
    for (auto& [entity, worker] : workers) {
        if (worker && worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    if (m_audioEngine) {
        for (auto& [entity, worker] : workers) {
            if (worker)
                m_audioEngine->mixer().unregisterSource(&worker->mixSource);
        }
    }
    m_lastWarmNs.clear();

    // Drain retired-but-unreaped workers. Their mix source was already
    // unregistered + stop signaled at retire time; just join (unconditionally,
    // shutdown blocking is fine) and clear.
    for (auto& [entity, worker] : m_retiredWorkers) {
        if (worker && worker->thread.joinable())
            worker->thread.join();
    }
    m_retiredWorkers.clear();
}

} // namespace entity
