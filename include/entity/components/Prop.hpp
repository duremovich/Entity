#pragma once

#include "entity/components/Model.hpp"
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

    // Transform in world space. `size` is the real-world bounding-box size
    // in meters (Blender 1u = 1m); the render path divides by the mesh's
    // native bounds (see Model.hpp::computeEffectiveScale) to get the vertex
    // multiplier. Default 1m³ is replaced on model assign with the mesh's
    // authored dimensions.
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};  // Euler degrees
    std::array<float, 3> size{1.0f, 1.0f, 1.0f};

    // Visibility in the 3D stage view.
    bool visible{true};

    // Stage-view opacity. Fades the prop in the editor's 3D stage view only;
    // does not affect any output path because props are excluded from
    // SceneSnapshot. Multiplied into displayColor.a at the StageMeshDraw site.
    float opacity{1.0f};

    // Matte tint used by Stage3DRenderer to fill triangles. Distinct from
    // Screen's video-textured fill so designers can tell set geometry apart
    // from projection surfaces at a glance. RGBA, 0..1.
    std::array<float, 4> displayColor{0.6f, 0.6f, 0.6f, 1.0f};
};

// Assign a Model entity to a Prop and sync its size to the mesh's authored
// bounds. Pass entt::null (or a null Model*) to reset to the 1m^3 default.
// Use this everywhere instead of writing `prop.modelEntity = e;` directly —
// keeps Size and the mesh in lockstep so computeEffectiveScale doesn't
// distort a non-cubic mesh against the unit-cube default. Promised by the
// Prop.hpp comment "Default 1m^3 is replaced on model assign with the
// mesh's authored dimensions" — this is where that lives.
inline void applyModelToProp(Prop& prop, entt::entity modelEntity,
                              const Model* model) {
    prop.modelEntity = modelEntity;
    if (model && model->mesh.isValid()) {
        prop.size = meshNativeBounds(&model->mesh);
    } else {
        prop.size = {1.0f, 1.0f, 1.0f};
    }
}

} // namespace entity
