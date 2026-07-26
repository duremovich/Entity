#pragma once

#include "entity/bus/Message.hpp"
#include "entity/core/Types.hpp"
#include "entity/media/FrameCache.hpp"   // FrameLease (returned by value)
#include <entt/entt.hpp>
#include <mutex>
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
// during playback, caches per-clip display state for the compositor.
//
// Knows nothing about Timeline / ProjectManager / clip math -- those live
// on the Director side. As of subtask 8 the per-tick `playState` arrives
// in the message body, so the prior `setTimeline` hook is gone -- the
// presenter never names Timeline.
//
// Per ADR-0014 this runs on the show thread and must not touch the
// registry — the editor thread is the sole writer, and `registry.destroy`
// during clip delete will race any show-thread read. Per-clip display
// state (last uploaded media frame, colour-space tags) lives in
// `m_clipDisplayState` keyed by entity, populated on upload, queried by
// CompositorSystem via `displayState()`. Stale entries for destroyed
// entities are leaked (entry count is bounded by total clips ever
// active in the session).
//
// Construction order: Renderer service owns this; needs IRenderer +
// FrameCache at construction (both are built first inside
// Renderer::initialize) and DecodeSystem via setDecodeSystem after the
// system is up.
class PlaybackPresenter {
public:
    struct ClipDisplayState {
        // The exact media frame this clip's texture is known-good for. Only
        // an exact-hit upload sets it; the nearest-frame fallback deliberately
        // leaves it alone so the next tick retries the frame we actually want.
        // It is therefore NOT "what's on the texture" — see lastUploadedFrame.
        FrameNumber       lastDecodedFrame{UINT32_MAX};
        // The media frame most recently uploaded to the texture by ANY path,
        // exact or fallback. This is what a viewer is actually looking at.
        // Diagnostics must read this one: during normal playback the fallback
        // does most of the uploading, so lastDecodedFrame can sit many frames
        // stale while the picture is moving perfectly well.
        FrameNumber       lastUploadedFrame{INVALID_FRAME};
        TextureColorSpace colorSpace{TextureColorSpace::Linear};
        std::string       ocioColorSpace;
    };

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

    // Display-state lookup for CompositorSystem. Returns the per-clip
    // colour-space + last-uploaded-frame stamped by the most recent
    // present(); falls back to a default ClipDisplayState for clips
    // that have never received an upload. Show-thread only — no
    // synchronisation needed because both producer (present) and
    // consumer (compositor) run on the show thread and present()
    // always runs first within a single tick.
    const ClipDisplayState& displayState(entt::entity entity) const {
        static const ClipDisplayState kDefault;
        auto it = m_clipDisplayState.find(entity);
        return it != m_clipDisplayState.end() ? it->second : kDefault;
    }

    // The media frame that last actually reached the clip's GPU texture.
    // Editor-thread-safe: `m_clipDisplayState` is show-thread-owned (an
    // off-thread read would race present()'s map inserts), so present()
    // republishes a guarded mirror each tick for the Clip Info readout.
    // Returns INVALID_FRAME if the clip has never been uploaded.
    //
    // This is deliberately the *presented* frame, not the *mapped* one: the
    // two diverge whenever an upload is skipped (exact cache miss while
    // paused), which is exactly the state that reads as a frozen picture.
    FrameNumber presentedFrame(entt::entity entity) const;

private:
    entt::registry& m_registry;
    IRenderer*      m_renderer{nullptr};
    FrameCache*     m_frameCache{nullptr};
    DecodeSystem*   m_decodeSystem{nullptr};

    // Guarded mirror of m_clipDisplayState[e].lastUploadedFrame, refreshed
    // once per present() so the editor thread can read it without touching
    // show-thread state. Updated in place; entries are never erased (see the
    // republish block in present()).
    mutable std::mutex                            m_presentedMutex;
    std::unordered_map<entt::entity, FrameNumber> m_presentedFrames;

    // Per-tick cache of fade multipliers from the bus payload; cleared
    // and repopulated at the top of each present() call.
    std::unordered_map<entt::entity, float> m_fadeMultipliers;

    // Per-clip display state stamped by present() and read by
    // CompositorSystem. Lives here (show-thread-local) instead of on
    // the VideoTexture component so deleting a clip on the editor
    // thread can't race a show-thread try_get against
    // registry.destroy() — see ADR-0014.
    std::unordered_map<entt::entity, ClipDisplayState> m_clipDisplayState;
};

} // namespace entity
