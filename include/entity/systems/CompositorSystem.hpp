#pragma once

#include "System.hpp"
#include "entity/core/Types.hpp"
#include <entt/entt.hpp>
#include <d3d12.h>

namespace entity {

// Forward declarations
class D3D12Renderer;
class Timeline;

/**
 * CompositorSystem - Renders visible layers to screen
 *
 * Queries entities with Transform and MediaLayer components,
 * sorts by z-order, and issues draw calls to the renderer.
 *
 * Timeline-aware: Only renders clips that are active at the current frame.
 * Supports multi-layer compositing with alpha blending.
 */
class CompositorSystem : public System {
public:
    /**
     * Construct compositor system with renderer reference.
     * @param renderer Pointer to D3D12 renderer (must outlive this system)
     */
    explicit CompositorSystem(D3D12Renderer* renderer);

    /**
     * Set the timeline for frame-accurate rendering.
     */
    void setTimeline(Timeline* timeline) { m_timeline = timeline; }

    void initialize(entt::registry& registry) override;
    void update(entt::registry& registry, float deltaTime) override;
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "CompositorSystem"; }

    /**
     * Enable/disable verbose debug logging.
     */
    void setDebugLogging(bool enabled) { m_debugLogging = enabled; }

    /**
     * Render a video texture through all visible mapping surfaces.
     * Used for projection mapping output.
     *
     * @param registry The ECS registry containing mapping surfaces
     * @param textureSrv GPU descriptor handle for the video texture to render
     */
    void renderMappingSurfaces(entt::registry& registry, D3D12_GPU_DESCRIPTOR_HANDLE textureSrv);

private:
    /**
     * Check if a clip is active at the given frame.
     */
    bool isClipActiveAtFrame(const struct Clip& clip, FrameNumber frame) const;

    /**
     * Ensure a screen has a valid render target, creating one if needed.
     * Handles lazy allocation and dimension change detection.
     * @param registry The ECS registry
     * @param screenEntity The screen entity to check/initialize
     * @return true if screen has valid render target after call
     */
    bool ensureScreenRenderTarget(entt::registry& registry, entt::entity screenEntity);

    D3D12Renderer* m_renderer{nullptr};
    Timeline* m_timeline{nullptr};
    bool m_debugLogging{false};
};

} // namespace entity
