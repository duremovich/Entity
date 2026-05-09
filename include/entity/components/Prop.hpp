#pragma once

#include <string>
#include <array>
#include <entt/entt.hpp>

namespace entity {

/**
 * Prop component — non-screen mesh for stage pre-visualization.
 *
 * Props are 3D geometry (set pieces, venue architecture, audience risers,
 * podiums) that designers place in the stage to reason about spatial layout
 * and project alignment. Unlike Screens, props are NEVER part of the
 * projector output path — they exist purely in the editor's 3D stage view.
 *
 * Per ADR-0014 (editor/show thread split), props are excluded from
 * SceneSnapshot::screens. The show thread never sees them. If/when
 * projector masking lands (Phase 2), it will add a separate propCatalog
 * field to SceneSnapshot rather than folding props into screens.
 *
 * One Model can be referenced by many Props (e.g. one chair.obj placed
 * five times around the stage), exactly like Screen → Model.
 */
struct Prop {
    std::string name{"Prop"};

    // Reference to the model entity that provides geometry. Multiple
    // props can share a model.
    entt::entity modelEntity{entt::null};

    // Transform in world space.
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};  // Euler degrees
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};

    // Visibility in the 3D stage view.
    bool visible{true};

    // Matte tint used by Stage3DRenderer to fill triangles. Distinct from
    // Screen's video-textured fill so designers can tell set geometry apart
    // from projection surfaces at a glance. RGBA, 0..1.
    std::array<float, 4> displayColor{0.6f, 0.6f, 0.6f, 1.0f};
};

} // namespace entity
