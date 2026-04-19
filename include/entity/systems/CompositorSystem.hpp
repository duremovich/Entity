#pragma once

#include "System.hpp"
#include "entity/core/Types.hpp"
#include "entity/render/IRenderer.hpp"
#include <entt/entt.hpp>

namespace entity {

// Forward declarations
class Timeline;

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
    /**
     * Construct compositor system with renderer reference.
     * @param renderer Pointer to rendering backend (must outlive this system)
     */
    explicit CompositorSystem(IRenderer* renderer);

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
     * @param texture Texture reference for the video texture to render
     */
    void renderMappingSurfaces(entt::registry& registry, TextureRef texture);

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

    IRenderer* m_renderer{nullptr};
    Timeline* m_timeline{nullptr};
    bool m_debugLogging{false};
};

} // namespace entity
