#pragma once

#include "entity/bus/Message.hpp"
#include "entity/core/Types.hpp"
#include <entt/entt.hpp>

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

    // Single-clip preview accessor (StageWindow's 2D view, etc.). Walks
    // `view<Clip, VideoTexture>`, asks the time authority which clip is
    // active at the current timeline frame and what mediaFrame to look
    // up, returns a raw DecodedFrame* into the cache. Caller must use
    // the pointer synchronously within this tick -- the FrameLease that
    // backs it is released on return, so a stashed pointer is racy
    // against eviction.
    const DecodedFrame* getCurrentVideoFrame(const PlaybackTimeAuthority& auth) const;

private:
    entt::registry& m_registry;
    IRenderer*      m_renderer{nullptr};
    FrameCache*     m_frameCache{nullptr};
    DecodeSystem*   m_decodeSystem{nullptr};
};

} // namespace entity
