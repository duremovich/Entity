#include "entity/renderer/PlaybackPresenter.hpp"

#include "entity/components/Clip.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/media/DecodedFrame.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/FrameCache.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/systems/DecodeSystem.hpp"
#include "entity/timeline/Timeline.hpp"

#include <chrono>
#include <iostream>

namespace entity {

PlaybackPresenter::PlaybackPresenter(entt::registry& registry, IRenderer* renderer, FrameCache* cache)
    : m_registry(registry)
    , m_renderer(renderer)
    , m_frameCache(cache)
{}

PlaybackPresenter::~PlaybackPresenter() = default;

void PlaybackPresenter::present(const std::vector<ActiveClip>& active) {
    if (!m_renderer || !m_frameCache) return;

    auto funcStart = std::chrono::high_resolution_clock::now();
    int cacheHits = 0;
    int cacheMisses = 0;
    int nearestFallbacks = 0;

    // Snapshot playback state once per call. The nearest-fallback gate
    // wants a consistent value across the loop (HIGH-02 in
    // CODE_ISSUES.md flagged the prior "re-read mid-tick" pattern; the
    // active-set comes from Director which already saw the same value
    // during its tick, so this is the single source of truth for the
    // current Renderer pass).
    PlaybackState playState = m_timeline ? m_timeline->getPlaybackState() : PlaybackState::Stopped;

    for (const ActiveClip& ac : active) {
        auto* videoTex = m_registry.try_get<VideoTexture>(ac.entity);
        if (!videoTex) continue;

        // Skip if the frame number hasn't changed since last upload --
        // the GPU texture is rendered every tick but only needs re-upload
        // on change.
        auto* state = m_registry.try_get<ClipDecodeState>(ac.entity);
        FrameNumber lastFrame = state ? state->lastDecodedFrame : UINT32_MAX;
        if (ac.mediaFrame == lastFrame) continue;

        // Decoder is mid-seek -- its in-flight frames are stale. Hold
        // the current texture until the seek completes.
        const DecodeWorker* worker = m_decodeSystem ? m_decodeSystem->getWorker(ac.entity) : nullptr;
        if (worker && worker->seekPending.load()) continue;

        // Exact cache hit -- the common path on steady-state playback
        // and on click-to-recently-viewed-frame.
        if (auto lease = m_frameCache->get(ac.entity, ac.mediaFrame)) {
            const DecodedFrame& f = *lease;
            bool ok = m_renderer->uploadVideoFrameToSlot(
                videoTex->descriptorSlot, f.data.data(), f.width, f.height, f.format);
            if (ok) {
                videoTex->width  = f.width;
                videoTex->height = f.height;
                videoTex->colorSpace = f.colorSpace;
                // Per-clip MediaBin OCIO override wins over the decoder
                // tag when set (Phase C.12 #9). Director side already
                // resolved the override string in the active-set tuple.
                videoTex->ocioColorSpace = ac.ocioOverride.empty()
                    ? f.ocioColorSpace
                    : ac.ocioOverride;
                if (state) state->lastDecodedFrame = ac.mediaFrame;
            }
            cacheHits++;
            continue;
        }
        cacheMisses++;

        // Closest-available-frame fallback ONLY during Playing. The
        // fallback exists so playback keeps progressing when the
        // decoder is one frame behind moving in the right direction --
        // showing N-1 instead of N for one tick is fine and avoids
        // visible stutter.
        //
        // During Paused/Scrubbing it's actively wrong: the decoder may
        // be mid-seek with stale frames in flight, and showing the
        // "closest" would mean visibly bouncing through the decoder's
        // prior progress while the user waits for the actually-clicked
        // frame. Leave the texture as-is and let the next tick try
        // again.
        if (playState != PlaybackState::Playing) continue;

        if (auto nearestN = m_frameCache->nearestTo(ac.entity, ac.mediaFrame)) {
            if (auto nearestLease = m_frameCache->get(ac.entity, *nearestN)) {
                const DecodedFrame& f = *nearestLease;
                bool ok = m_renderer->uploadVideoFrameToSlot(
                    videoTex->descriptorSlot, f.data.data(), f.width, f.height, f.format);
                if (ok) {
                    videoTex->width  = f.width;
                    videoTex->height = f.height;
                    videoTex->colorSpace = f.colorSpace;
                    videoTex->ocioColorSpace = ac.ocioOverride.empty()
                        ? f.ocioColorSpace
                        : ac.ocioOverride;
                    // Don't bump lastDecodedFrame -- we want to re-try
                    // the exact frame next tick now that decoder has
                    // more time.
                }
                nearestFallbacks++;
            }
        }
    }

    auto funcMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - funcStart).count();
    if (funcMs > 100) {
        std::cout << "[CLIP UPDATE] Total=" << funcMs << "ms"
                  << " active=" << active.size()
                  << " hits=" << cacheHits
                  << " misses=" << cacheMisses
                  << " nearest=" << nearestFallbacks
                  << " state=" << static_cast<int>(playState) << std::endl;
    }
}

const DecodedFrame* PlaybackPresenter::getCurrentVideoFrame(const PlaybackTimeAuthority& auth) const {
    // Returns the freshest cached frame for the active clip -- used by
    // the single-clip preview path (StageWindow's 2D view, etc.). Looks
    // up the exact mapped frame; on a miss, returns null and lets the
    // caller fall back to whatever it last had. No nearest-frame
    // fallback here -- that belongs in the per-frame compositor path
    // (`present()` above), not the preview accessor.
    if (!m_frameCache) return nullptr;
    const Timeline* timeline = auth.timeline();
    if (!timeline) return nullptr;

    FrameNumber currentFrame = timeline->getCurrentFrame();
    auto view = m_registry.view<Clip, VideoTexture>();
    for (auto [entity, clip, videoTex] : view.each()) {
        if (!auth.isClipActiveAtFrame(clip, currentFrame)) continue;

        FrameNumber mediaFrame = auth.mapToMediaFrame(clip, currentFrame);
        // NB: leases hold a strong shared_ptr to the frame's bytes, but
        // we return a raw pointer here. Caller must use the frame
        // synchronously within this tick -- a caller stashing the
        // pointer for later is racy (cache eviction would dangle it).
        // All current callers do use it synchronously.
        if (auto lease = const_cast<FrameCache*>(m_frameCache)->get(entity, mediaFrame)) {
            return lease.get();
        }
    }
    return nullptr;
}

} // namespace entity
