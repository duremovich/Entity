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
    // MappingSurface calibration data — both read-only. Screen enumeration
    // uses rf.screens (the snapshot); ensureScreenRenderTarget writes are
    // replaced by ScreenRenderTargetAllocated R2D replies (Stage 4).
    void update(const bus::RenderFrame& rf, entt::registry& registry, float deltaTime);
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "CompositorSystem"; }

    void renderMappingSurfaces(entt::registry& registry, TextureRef texture);

    // Allocate or resize a compose target for a screen. When a new slot is
    // allocated, posts a ScreenRenderTargetAllocated R2D reply so the editor
    // thread can write the slot back into the Screen component for the next
    // SceneSnapshot. Returns the slot ID (UINT32_MAX on failure).
    std::uint32_t ensureScreenRenderTarget(const bus::ScreenSnapshot& screenSnap);

    // V1 Muncher render: dim playfield + yellow square at the baked
    // (muncher_x, muncher_y). Every "game entity" is a transformed
    // colored quad until sprite-atlas rendering lands.
    void drawMuncherPlayfield(const bus::GenerativeLayerSnapshot& gl,
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
    // runs exclusively on the show thread per ADR-0014).
    std::unordered_map<entt::entity, PendingAllocation> m_pendingAllocations;
};

} // namespace entity
