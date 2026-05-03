#pragma once

#include <array>
#include <string>
#include <entt/entt.hpp>

namespace entity {

/**
 * Projector component — a virtual camera that maps content onto ProjectionSurface screens.
 *
 * By default (targetSurfaceCount == 0) a projector illuminates all ProjectionSurface
 * screens in the scene. Populate targetSurfaces to restrict to an explicit set.
 *
 * Data flow (deferred rendering, not yet wired):
 *   Screen compose target  →  ProjectorRenderSystem (renders scene from projector viewpoint)
 *                          →  projector output texture  →  physical projector output
 */
struct Projector {
    std::string name{"Projector"};

    std::array<float, 3> position{0.f, 3.f, 5.f};
    std::array<float, 3> rotation{-20.f, 0.f, 0.f};  // Euler degrees: pitch (X), yaw (Y), roll (Z)

    float fovDegrees{50.f};
    float nearClip{0.1f};
    float farClip{50.f};

    bool enabled{true};

    // targetSurfaceCount == 0 → project onto all ProjectionSurface screens (default).
    // targetSurfaceCount > 0  → only the listed surfaces.
    static constexpr int MAX_TARGETS = 8;
    int targetSurfaceCount{0};
    std::array<entt::entity, MAX_TARGETS> targetSurfaces{};
};

} // namespace entity
