#include "entity/systems/AudioSystem.hpp"
#include "entity/audio/AudioEngine.hpp"
#include "entity/audio/AudioMixer.hpp"
#include "entity/components/AudioSource.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/ClipPlaybackPhase.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/timeline/SectionFade.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/profile/Tracy.hpp"

#include <algorithm>
#include <cmath>

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
    worker->playbackMode = clip->playbackMode;

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
    m_workers.emplace(e, std::move(worker));
}

void AudioSystem::destroyWorker(entt::entity e) {
    auto it = m_workers.find(e);
    if (it == m_workers.end()) return;
    auto& worker = it->second;
    if (worker) {
        worker->running.store(false);
        if (worker->thread.joinable())
            worker->thread.join();
        if (m_audioEngine)
            m_audioEngine->mixer().unregisterSource(&worker->mixSource);
    }
    m_workers.erase(it);
    m_lastExpectedSample.erase(e);
}

void AudioSystem::update(entt::registry& registry, float /*deltaTime*/) {
    ZoneScopedN("AudioSystem");
    if (!m_timeline || !m_audioEngine) return;

    const bool isEditorTick = (std::this_thread::get_id() == m_editorThreadId);
    const FrameNumber currentTLFrame = m_timeline->getCurrentFrame();
    const double tlFPS = m_timeline->getFrameRate() > 0.0
        ? m_timeline->getFrameRate() : 30.0;
    const int rate = m_audioEngine->sampleRate();

    auto view = registry.view<Clip, AudioSource>();
    for (auto [entity, clip, as] : view.each()) {
        if (!clip.loaded) continue;

        auto workerIt = m_workers.find(entity);
        if (workerIt == m_workers.end()) {
            if (!isEditorTick) continue;
            createWorker(entity, registry);
            workerIt = m_workers.find(entity);
            if (workerIt == m_workers.end()) continue;
        }

        auto& worker = workerIt->second;
        if (!worker) continue;

        // Mirror gain/mute/solo each tick.
        worker->mixSource.gain.store(as.gain);
        worker->mixSource.mute.store(as.mute);
        worker->mixSource.solo.store(as.solo);

        // Reflect the worker's decoder result back onto the component: the
        // AudioDecoder is the source of truth for whether the media really
        // has an audio stream. Editor-thread only (this is a registry
        // write — must not run on the show-thread fallback, ADR-0014).
        // Keeps hasAudioStream correct regardless of how the clip was
        // created, so the PropertyWindow audio panel appears and
        // ProjectSerializer persists gain/mute/solo on the next save.
        if (isEditorTick && !as.hasAudioStream
                && worker->initialized.load(std::memory_order_relaxed)) {
            as.hasAudioStream = true;
        }

        // Keep playback mode in sync.
        worker->playbackMode = clip.playbackMode;

        // Determine whether this clip should be steering its worker.
        // A clip steers (seeks/prerolls) whenever the transport is Playing and
        // the playhead is inside the clip window, OR while the clip is in
        // section-break continuation. This is independent of the seek-sync gate
        // so the worker seeks and prerolls its ring even while the gate holds
        // the timeline still.
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
        const bool shouldSteer = (inWindow && playing) || inContinuation;

        // mixSource.active gates the mixer: the clip is audible only when it
        // should be steering AND the seek-sync gate is not holding the timeline.
        // When the gate is set (Phase 4 path), the worker still seeks/prerolls
        // but the mixer outputs silence until SeekSyncController releases.
        // Gate is always false in this phase, so behaviour is unchanged.
        worker->mixSource.active.store(
            shouldSteer && !m_timeline->isSeekSyncGated());

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
        } else if (phase && phase->postBreakMediaAnchor >= 0) {
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

        // Wrap sourceLocalFrame per PlaybackMode to get media frame.
        FrameNumber mediaLocalFrame = sourceLocalFrame;
        if (sourceLocalFrame >= sourceLength) {
            switch (clip.playbackMode) {
                case PlaybackMode::Freeze:
                    mediaLocalFrame = sourceLength - 1;
                    break;
                case PlaybackMode::Loop:
                    mediaLocalFrame = sourceLocalFrame % sourceLength;
                    break;
                case PlaybackMode::PingPong: {
                    const FrameNumber cycle = sourceLocalFrame / sourceLength;
                    const FrameNumber pos   = sourceLocalFrame % sourceLength;
                    mediaLocalFrame = (cycle % 2 == 0) ? pos : (sourceLength - 1 - pos);
                    break;
                }
            }
        }

        // Convert media frame to clip-local output-rate sample.
        const double fps = clip.framerate > 0.0 ? clip.framerate : 30.0;
        const int64_t expectedSample = static_cast<int64_t>(
            std::round(mediaLocalFrame / fps * rate));

        // Seek detection: frame-over-frame discontinuity on the controller side.
        // We compare expectedSample to m_lastExpectedSample[entity] (the value
        // we computed last tick), not to the worker's decode cursor. During
        // normal playback expectedSample advances by ~(rate/tlFPS) samples per
        // tick — well under a 0.5s threshold. A genuine scrub/seek causes a
        // jump >> one tick's worth → we fire seekPending. This approach is
        // immune to the decode/playback ring-fill offset (the worker decodes
        // ahead by up to ~1s, but that offset is stable across ticks and never
        // shows up as a discontinuity on the controller side).
        if (!worker->seekPending.load()) {
            auto lastIt = m_lastExpectedSample.find(entity);
            const bool firstActiveTick = (lastIt == m_lastExpectedSample.end());
            bool seekNeeded = firstActiveTick;
            if (!firstActiveTick) {
                // One-tick advance in sample space: rate / tlFPS, with 3× slack
                // for jitter/rounding (still far below any real scrub delta).
                const int64_t oneTickSamples = static_cast<int64_t>(
                    std::ceil(static_cast<double>(rate) / tlFPS * 3.0));
                const int64_t delta = std::abs(expectedSample - lastIt->second);
                seekNeeded = (delta > oneTickSamples);
            }
            if (seekNeeded) {
                const int64_t clampedSample = std::clamp<int64_t>(
                    expectedSample, 0,
                    worker->outPointSample - worker->inPointSample - 1);
                worker->seekTarget.store(clampedSample);
                worker->seekPending.store(true);
            }
        }
        m_lastExpectedSample[entity] = expectedSample;
    }

    // Sliding-window prefetch — mirrors DecodeSystem::prefetchUpcoming so
    // audio workers warm in step with video. Without this, an MP4 clip
    // queued at a section break holds the SeekSyncController gate for the
    // full preroll-timeout (3 s) because its audio worker doesn't exist
    // until the playhead enters the clip window. Editor-thread only
    // (writes m_workers); skipped when Stopped.
    if (isEditorTick &&
        m_timeline->getPlaybackState() != PlaybackState::Stopped) {
        ZoneScopedN("AudioSystem::prefetchUpcoming");
        const FrameNumber prefetchAhead = static_cast<FrameNumber>(
            std::ceil(kPrefetchAheadSeconds * tlFPS));
        const FrameNumber windowEnd = currentTLFrame + prefetchAhead;

        for (auto [entity, clip, as] : view.each()) {
            if (!clip.loaded) continue;
            if (clip.startFrame <= currentTLFrame) continue;
            if (clip.startFrame >  windowEnd)      continue;
            if (m_workers.contains(entity))        continue;
            createWorker(entity, registry);
        }
    }

    // Destroy workers for entities that are gone (editor-thread only).
    if (isEditorTick) {
        std::vector<entt::entity> toRemove;
        for (auto& [ent, w] : m_workers) {
            if (!registry.valid(ent) || !registry.all_of<Clip, AudioSource>(ent)) {
                toRemove.push_back(ent);
            }
        }
        for (entt::entity ent : toRemove) {
            destroyWorker(ent);
        }
    }
}

int64_t AudioSystem::getWorkerSeekTargetFrame(entt::entity clipEntity,
                                               double clipFps) const {
    auto it = m_workers.find(clipEntity);
    if (it == m_workers.end() || !it->second) return -1;
    const AudioDecodeWorker& w = *it->second;
    if (w.initFailed.load(std::memory_order_relaxed)) return 0;
    if (!w.initialized.load(std::memory_order_acquire)) return -1;
    const double fps = (clipFps > 0.0) ? clipFps : 30.0;
    const int64_t sampleTarget = w.seekTarget.load(std::memory_order_relaxed);
    return static_cast<int64_t>(
        std::round(static_cast<double>(sampleTarget) / w.targetSampleRate * fps));
}

bool AudioSystem::isWorkerSeekReady(entt::entity clipEntity) const {
    auto it = m_workers.find(clipEntity);
    if (it == m_workers.end() || !it->second) return false;
    const AudioDecodeWorker& w = *it->second;
    if (w.initFailed.load(std::memory_order_relaxed)) return true;
    return w.initialized.load(std::memory_order_acquire)
        && !w.seekPending.load(std::memory_order_acquire)
        && w.ring.availableFrames() >= kAudioPrerollFrames;
}

void AudioSystem::shutdown(entt::registry& registry) {
    (void)registry;
    for (auto& [entity, worker] : m_workers) {
        if (worker) {
            worker->running.store(false);
        }
    }
    for (auto& [entity, worker] : m_workers) {
        if (worker && worker->thread.joinable()) {
            worker->thread.join();
        }
    }
    if (m_audioEngine) {
        for (auto& [entity, worker] : m_workers) {
            if (worker)
                m_audioEngine->mixer().unregisterSource(&worker->mixSource);
        }
    }
    m_workers.clear();
    m_lastExpectedSample.clear();
}

} // namespace entity
