#pragma once

#include "System.hpp"
#include "entity/bus/IMessageTransport.hpp"
#include "entity/bus/Message.hpp"
#include "entity/core/Types.hpp"
#include "entity/render/IRenderer.hpp"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace entity {

// Forward declarations
class Timeline;
class PlaybackPresenter;

/**
 * CompositorSystem - Renders visible layers to screen
 *
 * Queries entities with Transform and MediaLayer components,
 * sorts by z-order, and issues draw calls through the IRenderer interface.
 *
 * Timeline-aware: Only renders clips that are active at the current frame.
 * Supports multi-layer compositing with alpha blending.
 */
class CompositorSystem : public System {
public:
    explicit CompositorSystem(IRenderer* renderer, bus::IMessageTransport* transport = nullptr);

    void setTransport(bus::IMessageTransport* transport) { m_transport = transport; }
    void setDebugLogging(bool enabled) { m_debugLogging = enabled; }
    void setPlaybackPresenter(PlaybackPresenter* p) { m_playbackPresenter = p; }

    void initialize(entt::registry& registry) override;
    // Base-class override — satisfies System interface; Engine calls the
    // RenderFrame overload below directly.
    void update(entt::registry& registry, float deltaTime) override {}
    // Primary render path: walks rf.activeClips instead of the registry.
    // Registry is touched only for VideoTexture colorSpace reads and
    // OutputSurface calibration data — both read-only. Screen enumeration
    // uses rf.screens (the snapshot); ensureScreenRenderTarget writes are
    // replaced by ScreenRenderTargetAllocated R2D replies (Stage 4).
    //
    // rf is non-const because PASS 1.5 patches `postEffectsSlot` on each
    // ContentLayerSnapshot with the chain's final ping-pong slot before
    // PASS 2 reads it (issue #54). The mutation is scoped to this call —
    // nothing else reads rf during update().
    void update(bus::RenderFrame& rf, entt::registry& registry, float deltaTime);
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "CompositorSystem"; }

    // Allocate or resize a compose target for a screen. When a new slot is
    // allocated, posts a ScreenRenderTargetAllocated R2D reply so the editor
    // thread can write the slot back into the Screen component for the next
    // SceneSnapshot. Returns the slot ID (UINT32_MAX on failure).
    std::uint32_t ensureScreenRenderTarget(const bus::ScreenSnapshot& screenSnap);

    // Allocate or resize a compose target for a generative layer. Same R2D
    // ack pattern as ensureScreenRenderTarget — posts
    // GenerativeLayerRenderTargetAllocated when allocating. Returns the slot
    // ID (UINT32_MAX on failure). Called from PASS 1 of update().
    std::uint32_t ensureGenerativeRenderTarget(const bus::GenerativeLayerSnapshot& gl);

    // PASS 1.5 scratch pool (DAG executor). Effect intermediates are
    // show-thread-local compose targets acquired per node and recycled by
    // liveness — the editor never needs these slots, so the old per-layer
    // ping-pong + EffectChainRenderTargetAllocated ack round-trip is gone
    // (ADR-0019 amendment). Slots are retained at high-water and shared
    // across layers; reset at the top of update() releases the previous
    // frame's held outputs.
    std::uint32_t acquireEffectScratch(std::uint32_t width, std::uint32_t height);
    void          releaseEffectScratch(std::uint32_t slot);
    void          resetEffectScratch();

    // V1 Muncher render: dim playfield + walls + pellets + ghosts + Muncher.
    // Draws in LAYER-LOCAL NDC into the active compose target — caller is
    // expected to have called beginComposeTarget on the layer's own RT
    // already. PASS 2 applies the UV-space transform via drawTexturedQuad.
    void drawMuncherPlayfield(const bus::GenerativeLayerSnapshot& gl,
                              float drawOpacity);

    // Text layer render: blits the pre-rasterized video-pool texture
    // (uploaded by TextSystem on the editor thread) into the active
    // compose target. No-op if textTextureSlot < 0.
    void drawTextLayer(const bus::GenerativeLayerSnapshot& gl,
                       float drawOpacity);

private:
    struct PendingAllocation {
        std::uint32_t slot{UINT32_MAX};
        std::uint32_t width{0};
        std::uint32_t height{0};
    };

    bus::IMessageTransport* m_transport{nullptr};
    IRenderer* m_renderer{nullptr};
    PlaybackPresenter* m_playbackPresenter{nullptr};
    bool m_debugLogging{false};
    // Show-thread-only; no synchronization needed (CompositorSystem::update
    // runs exclusively on the show thread per ADR-0014). One map per RT-owning
    // archetype — same shape, separate keys so cache evictions don't cross.
    std::unordered_map<entt::entity, PendingAllocation> m_pendingAllocations;
    std::unordered_map<entt::entity, PendingAllocation> m_pendingGenerativeAllocations;

    // PASS 1.5 scratch pool — show-thread-only. Exact-size reuse first,
    // then resize of any free slot, then a fresh compose target. Retained
    // at high-water (no releaseComposeTarget exists — same bounded-growth
    // posture as the rest of the compose pool).
    struct EffectScratchSlot {
        std::uint32_t slot{UINT32_MAX};
        std::uint32_t width{0};
        std::uint32_t height{0};
        bool          inUse{false};
    };
    std::vector<EffectScratchSlot> m_effectScratchPool;
};

} // namespace entity
