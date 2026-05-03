#pragma once

#include <array>
#include <string>
#include <vector>
#include <entt/entt.hpp>

namespace entity {

/**
 * Single (3D world point ↔ projector UV) correspondence used by the
 * calibration system. Persisted on the Projector entity so points survive
 * close/reopen of the calibration window.
 */
struct CalibrationPoint {
    std::array<float, 3> worldPos{0.f, 0.f, 0.f};
    std::array<float, 2> projectorUV{0.5f, 0.5f};
    bool isAligned{false}; // user has positioned the crosshair for this point
};

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

    // Calibration
    entt::entity linkedOutput{entt::null};  // OutputDisplay entity driving this projector
    bool isCalibrated{false};              // true after a successful solve + apply

    // Persisted correspondence points for incremental calibration. The
    // calibration window loads these on open and saves on every modification
    // so the user doesn't lose work across close/reopen cycles.
    std::vector<CalibrationPoint> calibrationPoints;
};

} // namespace entity
