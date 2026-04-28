#pragma once

#include "entity/core/Types.hpp"
#include <entt/entt.hpp>

#include <vector>

namespace entity {

class IRenderer;
class Timeline;
class DecodeSystem;
class FrameCache;
class PlaybackTimeAuthority;
struct ActiveClip;
struct DecodedFrame;

// PlaybackPresenter -- Renderer-side half of the old PlaybackController.
// Receives a per-tick active set from PlaybackTimeAuthority, leases
// frames from FrameCache, uploads to the GPU via IRenderer, applies the
// nearest-frame fallback during playback, stamps videoTex fields after
// upload.
//
// Knows nothing about Timeline transitions / ProjectManager / clip math
// -- those live on the Director side. The one concession: a read-only
// Timeline pointer is held so the nearest-frame fallback can gate on
// PlaybackState (the fallback is wrong while paused / scrubbing). Subtask
// 8 swaps that for a `playState` field on the RenderFrame bus message.
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

    // Read-only Timeline -- only consulted for getPlaybackState in the
    // nearest-fallback gate. Wired by Engine after Director exists.
    void setTimeline(const Timeline* t) { m_timeline = t; }

    // Per-tick GPU upload pass. Called from Engine::render() between
    // beginFrame() and the compositor pass so uploads land on the open
    // command list before any shader reads them.
    void present(const std::vector<ActiveClip>& active);

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
    const Timeline* m_timeline{nullptr};
};

} // namespace entity
