#pragma once

#include "System.hpp"
#include "entity/bus/Message.hpp"
#include "entity/core/Types.hpp"
#include "entity/render/IRenderer.hpp"
#include <entt/entt.hpp>

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
    explicit CompositorSystem(IRenderer* renderer);

    void setDebugLogging(bool enabled) { m_debugLogging = enabled; }

    void initialize(entt::registry& registry) override;
    // Base-class override — satisfies System interface; Engine calls the
    // RenderFrame overload below directly.
    void update(entt::registry& registry, float deltaTime) override {}
    // Primary render path: walks rf.activeClips instead of the registry.
    // Registry stays as the parameter for Screen enumeration and
    // MappingSurface reads only.
    void update(const bus::RenderFrame& rf, entt::registry& registry, float deltaTime);
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "CompositorSystem"; }

    void renderMappingSurfaces(entt::registry& registry, TextureRef texture);

private:
    bool ensureScreenRenderTarget(entt::registry& registry, entt::entity screenEntity);

    IRenderer* m_renderer{nullptr};
    bool m_debugLogging{false};
};

} // namespace entity
