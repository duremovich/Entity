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

    void renderOutputSurfaces(entt::registry& registry, TextureRef texture);

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

    // Allocate or resize the two ping-pong compose targets used by PASS 1.5
    // for a layer's effect chain (issue #54). Posts one
    // EffectChainRenderTargetAllocated R2D ack per side on first allocation.
    // Returns true when both slots are valid and ready to draw with.
    bool ensureEffectPingPongTargets(std::uint64_t layerEntity,
                                      std::int32_t snapshotSlotA,
                                      std::int32_t snapshotSlotB,
                                      std::uint32_t width,
                                      std::uint32_t height,
                                      std::uint32_t& outSlotA,
                                      std::uint32_t& outSlotB);

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

    // Two slots per layer with effects (sideA + sideB) ping-pong through the
    // chain. Filled in on first PASS 1.5 visit, mirrored back through the
    // EffectChainRenderTargetAllocated R2D ack so the next snapshot carries
    // them on the corresponding LayerEffectsSnapshot.
    struct PendingEffectAllocation {
        std::uint32_t slotA{UINT32_MAX};
        std::uint32_t slotB{UINT32_MAX};
        std::uint32_t width{0};
        std::uint32_t height{0};
    };
    std::unordered_map<entt::entity, PendingEffectAllocation> m_pendingEffectAllocations;
};

} // namespace entity
