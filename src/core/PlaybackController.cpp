#include "entity/core/PlaybackController.hpp"

#include "entity/render/IRenderer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/systems/DecodeSystem.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/FrameCache.hpp"
#include "entity/media/DecodedFrame.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/project/ProjectManager.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace entity {

PlaybackController::PlaybackController(entt::registry& registry, Timeline* timeline, IRenderer* renderer)
    : m_registry(registry)
    , m_timeline(timeline)
    , m_renderer(renderer)
{
}

PlaybackController::~PlaybackController() = default;

void PlaybackController::applyInputColorSpaceOverride(const Clip& clip, VideoTexture& videoTex) const {
    if (!m_projectManager) return;
    if (clip.filepath.empty()) return;
    const auto* entry = m_projectManager->findEntry(clip.filepath);
    if (!entry) return;
    if (entry->inputColorSpaceOverride.empty()) return;
    videoTex.ocioColorSpace = entry->inputColorSpaceOverride;
}

void PlaybackController::startTiming() {
    m_startTime = Clock::now();
    m_lastFrameTime = m_startTime;
    m_deltaTime = 0.0;
    m_elapsedTime = 0.0;
    m_frameCount = 0;
}

void PlaybackController::updateTiming() {
    TimePoint currentTime = Clock::now();
    std::chrono::duration<double> delta = currentTime - m_lastFrameTime;
    m_deltaTime = delta.count();
    std::chrono::duration<double> elapsed = currentTime - m_startTime;
    m_elapsedTime = elapsed.count();
    m_lastFrameTime = currentTime;
}

bool PlaybackController::isClipActiveAtFrame(const Clip& clip, FrameNumber frame) const {
    return frame >= clip.startFrame && frame < (clip.startFrame + clip.duration);
}

FrameNumber PlaybackController::mapToMediaFrame(const Clip& clip, FrameNumber timelineFrame) const {
    FrameNumber localFrame = timelineFrame - clip.startFrame;

    double timelineFrameRate = m_timeline ? m_timeline->getFrameRate() : 30.0;
    double frameRateRatio = clip.framerate / timelineFrameRate;
    FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));

    FrameNumber sourceLength = clip.totalMediaFrames > 0 ? clip.totalMediaFrames :
        static_cast<FrameNumber>(clip.duration * frameRateRatio);

    if (sourceLocalFrame < sourceLength) {
        return clip.mediaStartFrame + sourceLocalFrame;
    }

    switch (clip.playbackMode) {
        case PlaybackMode::Freeze:
            return clip.mediaStartFrame + sourceLength - 1;
        case PlaybackMode::Loop:
            return clip.mediaStartFrame + (sourceLocalFrame % sourceLength);
        case PlaybackMode::PingPong: {
            FrameNumber cycle = sourceLocalFrame / sourceLength;
            FrameNumber pos = sourceLocalFrame % sourceLength;
            if (cycle % 2 == 0) {
                return clip.mediaStartFrame + pos;
            } else {
                return clip.mediaStartFrame + (sourceLength - 1 - pos);
            }
        }
    }
    return clip.mediaStartFrame + sourceLength - 1;
}

const DecodedFrame* PlaybackController::getCurrentVideoFrame() const {
    // Returns the freshest cached frame for the active clip — used by the
    // single-clip preview path (StageWindow's 2D view, etc). Looks up the
    // exact mapped frame; on a miss, returns null and lets the caller fall
    // back to whatever it last had. No nearest-frame fallback here — that
    // belongs in the per-frame compositor path, not the preview accessor.
    if (!m_frameCache || !m_timeline) return nullptr;

    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    auto view = m_registry.view<Clip, VideoTexture>();
    for (auto [entity, clip, videoTex] : view.each()) {
        if (!isClipActiveAtFrame(clip, currentFrame)) continue;

        FrameNumber mediaFrame = mapToMediaFrame(clip, currentFrame);
        // NB: leases hold a strong shared_ptr to the frame's bytes, but we
        // return a raw pointer here. Caller is expected to use the frame
        // synchronously within this tick — a caller stashing the pointer for
        // later is racy (cache eviction would dangle it). All current callers
        // do use it synchronously.
        if (auto lease = const_cast<FrameCache*>(m_frameCache)->get(entity, mediaFrame)) {
            return lease.get();
        }
    }
    return nullptr;
}

void PlaybackController::updateClipVideos() {
    if (!m_renderer || !m_timeline || !m_frameCache) return;

    auto funcStart = std::chrono::high_resolution_clock::now();
    int clipCount = 0;
    int activeCount = 0;
    int cacheHits = 0;
    int cacheMisses = 0;
    int nearestFallbacks = 0;

    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    PlaybackState playState = m_timeline->getPlaybackState();

    auto view = m_registry.view<Clip, VideoTexture>();
    for (auto [entity, clip, videoTex] : view.each()) {
        clipCount++;
        if (!videoTex.isAllocated()) continue;
        if (!isClipActiveAtFrame(clip, currentFrame)) continue;
        activeCount++;

        FrameNumber mediaFrame = mapToMediaFrame(clip, currentFrame);

        // Skip if frame number hasn't changed since last upload (rendered every
        // tick, but the GPU texture only needs re-upload on change).
        auto* state = m_registry.try_get<ClipDecodeState>(entity);
        FrameNumber lastFrame = state ? state->lastDecodedFrame : UINT32_MAX;
        if (mediaFrame == lastFrame) continue;

        // Decoder is mid-seek — its in-flight frames are stale. Hold the
        // current texture until the seek completes.
        const DecodeWorker* worker = m_decodeSystem ? m_decodeSystem->getWorker(entity) : nullptr;
        if (worker && worker->seekPending.load()) continue;

        // Exact cache hit — the common path on steady-state playback and on
        // click-to-recently-viewed-frame.
        if (auto lease = m_frameCache->get(entity, mediaFrame)) {
            const DecodedFrame& f = *lease;
            bool ok = m_renderer->uploadVideoFrameToSlot(
                videoTex.descriptorSlot, f.data.data(), f.width, f.height, f.format);
            if (ok) {
                videoTex.width = f.width;
                videoTex.height = f.height;
                videoTex.colorSpace = f.colorSpace;
                videoTex.ocioColorSpace = f.ocioColorSpace;
                applyInputColorSpaceOverride(clip, videoTex);
                if (state) state->lastDecodedFrame = mediaFrame;
            }
            cacheHits++;
            continue;
        }
        cacheMisses++;

        // Closest-available-frame fallback ONLY during Playing. The fallback
        // exists so playback keeps progressing when the decoder is one frame
        // behind moving in the right direction — there, showing N-1 instead
        // of N for one tick is fine and avoids visible stutter.
        //
        // During Paused/Scrubbing it's actively wrong: the decoder may be
        // mid-seek with stale frames in flight, and showing the "closest"
        // would mean visibly bouncing through the decoder's prior progress
        // while the user waits for the actually-clicked frame. Leave the
        // texture as-is and let the next tick try again.
        if (playState != PlaybackState::Playing) continue;

        if (auto nearestN = m_frameCache->nearestTo(entity, mediaFrame)) {
            if (auto nearestLease = m_frameCache->get(entity, *nearestN)) {
                const DecodedFrame& f = *nearestLease;
                bool ok = m_renderer->uploadVideoFrameToSlot(
                    videoTex.descriptorSlot, f.data.data(), f.width, f.height, f.format);
                if (ok) {
                    videoTex.width = f.width;
                    videoTex.height = f.height;
                    videoTex.colorSpace = f.colorSpace;
                    videoTex.ocioColorSpace = f.ocioColorSpace;
                    applyInputColorSpaceOverride(clip, videoTex);
                    // Don't bump lastDecodedFrame — we want to re-try the
                    // exact frame next tick now that decoder has more time.
                }
                nearestFallbacks++;
            }
        }
    }

    auto funcMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - funcStart).count();
    if (funcMs > 100) {
        std::cout << "[CLIP UPDATE] Total=" << funcMs << "ms"
                  << " clips=" << clipCount
                  << " active=" << activeCount
                  << " hits=" << cacheHits
                  << " misses=" << cacheMisses
                  << " nearest=" << nearestFallbacks
                  << " frame=" << currentFrame
                  << " state=" << static_cast<int>(playState) << std::endl;
    }
}

} // namespace entity
