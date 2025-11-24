#pragma once

#include "System.hpp"
#include <entt/entt.hpp>

namespace entity {

// Forward declaration
class D3D12Renderer;

/**
 * CompositorSystem - Renders visible layers to screen
 *
 * Queries entities with Transform and MediaLayer components,
 * sorts by z-order, and issues draw calls to the renderer.
 *
 * Phase 1: Renders colored quads
 * Phase 2: Will render textured quads with video frames
 */
class CompositorSystem : public System {
public:
    /**
     * Construct compositor system with renderer reference.
     * @param renderer Pointer to D3D12 renderer (must outlive this system)
     */
    explicit CompositorSystem(D3D12Renderer* renderer);

    void initialize(entt::registry& registry) override;
    void update(entt::registry& registry, float deltaTime) override;
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "CompositorSystem"; }

private:
    D3D12Renderer* m_renderer;
};

} // namespace entity
