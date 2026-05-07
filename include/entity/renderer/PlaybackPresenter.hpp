#pragma once

#include "entity/bus/Message.hpp"
#include "entity/core/Types.hpp"
#include <entt/entt.hpp>
#include <unordered_map>

namespace entity {

class IRenderer;
class DecodeSystem;
class FrameCache;
class PlaybackTimeAuthority;
struct DecodedFrame;

// PlaybackPresenter -- Renderer-side half of the old PlaybackController.
// Consumes a `bus::RenderFrame` per tick, leases frames from FrameCache,
// uploads to the GPU via IRenderer, applies the nearest-frame fallback
// during playback, stamps videoTex fields after upload.
//
// Knows nothing about Timeline / ProjectManager / clip math -- those live
// on the Director side. As of subtask 8 the per-tick `playState` arrives
// in the message body, so the prior `setTimeline` hook is gone -- the
// presenter never names Timeline.
//
// Construction order: Renderer service owns this; needs IRenderer +
// FrameCache at construction (both are built first inside
// Renderer::initialize) and DecodeSystem via setDecodeSystem after the
// system is up.
class PlaybackPresenter {
public:
    PlaybackPresenter(entt::registry& registry, IRenderer* renderer, FrameCache* cache);
    ~PlaybackPresenter();

    PlaybackPresenter(const PlaybackPresenter&) = delete;
    PlaybackPresenter& operator=(const PlaybackPresenter&) = delete;

    void setDecodeSystem(DecodeSystem* s) { m_decodeSystem = s; }

    // Per-tick GPU upload pass. Called from Renderer-side bus drain
    // between beginFrame() and the compositor pass so uploads land on the
    // open command list before any shader reads them.
    void present(const bus::RenderFrame& rf);

    // Phase D — refresh the per-clip section-fade-multiplier cache from
    // the bus payload. Called from present() but exposed separately so
    // unit tests can populate the cache without a live renderer backend.
    void refreshFadeMultiplierCache(const bus::RenderFrame& rf);

    // Single-clip preview accessor (StageWindow's 2D view, etc.). Walks
    // `view<Clip, VideoTexture>`, asks the time authority which clip is
    // active at the current timeline frame and what mediaFrame to look
    // up, returns a raw DecodedFrame* into the cache. Caller must use
    // the pointer synchronously within this tick -- the FrameLease that
    // backs it is released on return, so a stashed pointer is racy
    // against eviction.
    const DecodedFrame* getCurrentVideoFrame(const PlaybackTimeAuthority& auth) const;

    // Phase D — section fade envelope lookup. The bus payload carries
    // sectionFadeMultiplier per active clip; PlaybackPresenter caches
    // it Renderer-locally each tick so CompositorSystem can multiply
    // it into the per-clip draw opacity at composite time. Caching it
    // here (vs. mutating MediaLayer.opacity in the registry) keeps the
    // boundary contract clean and avoids the exponential-decay bug
    // that would happen for clips without an enabled Opacity keyframe
    // track to rewrite the multiplied value each tick.
    float fadeMultiplier(entt::entity entity) const {
        auto it = m_fadeMultipliers.find(entity);
        return it != m_fadeMultipliers.end() ? it->second : 1.0f;
    }

private:
    entt::registry& m_registry;
    IRenderer*      m_renderer{nullptr};
    FrameCache*     m_frameCache{nullptr};
    DecodeSystem*   m_decodeSystem{nullptr};

    // Per-tick cache of fade multipliers from the bus payload; cleared
    // and repopulated at the top of each present() call.
    std::unordered_map<entt::entity, float> m_fadeMultipliers;
};

} // namespace entity
