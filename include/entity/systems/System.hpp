#pragma once

#include <entt/entt.hpp>

namespace entity {

/**
 * Base System interface for ECS architecture.
 *
 * Systems contain logic that operates on components.
 * All systems inherit from this interface and implement update().
 *
 * Design Principles:
 * - Systems are stateless logic processors
 * - All data lives in components (data-oriented design)
 * - Use registry.view<Component>() for cache-friendly iteration
 * - Keep update() performance-critical code allocation-free
 *
 * Lifecycle:
 * 1. initialize() - Called once during engine startup
 * 2. update() - Called every frame (main loop)
 * 3. shutdown() - Called once during engine shutdown
 */
class System {
public:
    virtual ~System() = default;

    /**
     * Called once during engine initialization.
     * Use this for one-time setup, resource loading, or system state initialization.
     *
     * @param registry The ECS registry containing all entities and components
     */
    virtual void initialize(entt::registry& registry) {}

    /**
     * Called every frame - pure virtual, must be implemented by derived systems.
     * This is where the core system logic executes.
     *
     * PERFORMANCE CRITICAL: Minimize allocations, use component views for iteration.
     *
     * @param registry The ECS registry containing all entities and components
     * @param deltaTime Time elapsed since last frame in seconds
     */
    virtual void update(entt::registry& registry, float deltaTime) = 0;

    /**
     * Called once during engine shutdown.
     * Use this for cleanup, resource release, or final state saving.
     *
     * @param registry The ECS registry containing all entities and components
     */
    virtual void shutdown(entt::registry& registry) {}

    /**
     * Get system name for debugging and logging.
     * Should return a static string literal (no allocation).
     *
     * @return System name as null-terminated string
     */
    virtual const char* getName() const = 0;
};

} // namespace entity
