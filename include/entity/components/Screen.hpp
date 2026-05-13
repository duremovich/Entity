#pragma once

#include "entity/components/Model.hpp"
#include "entity/core/Types.hpp"
#include <string>
#include <array>
#include <entt/entt.hpp>

namespace entity {

enum class ScreenType : uint8_t {
    LEDWall = 0,
    ProjectionSurface = 1,
};

/**
 * Screen component - represents a projection surface in the 3D scene
 *
 * A screen is a surface where video content is displayed. It consists of:
 * - 3D geometry (from a Model component)
 * - Transform (position, rotation, scale in world space)
 * - Resolution (internal render resolution for the screen's content)
 *
 * Clips can be mapped to screens via their targetScreen property.
 */
struct Screen {
    std::string name{"Screen"};

    // Reference to the model entity that provides geometry
    entt::entity modelEntity{entt::null};

    // Screen content resolution (independent of model geometry)
    uint32_t width{1920};
    uint32_t height{1080};

    // Transform in world space. `size` is the real-world bounding-box size
    // in meters (matching Blender's default 1 unit = 1 meter convention).
    // The render path divides by the mesh's native bounds (see
    // Model.hpp::computeEffectiveScale) to get the vertex multiplier. New
    // screens with no model assigned default to a 4m × 2.25m flat panel.
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};  // Euler angles (degrees)
    std::array<float, 3> size{4.0f, 2.25f, 0.0f};

    // Visibility
    bool visible{true};
    float opacity{1.0f};

    // Display order (for overlapping screens)
    int32_t zOrder{0};

    // GPU render target for this screen's content
    uint32_t renderTargetSlot{UINT32_MAX};
    bool renderTargetValid{false};

    // Screen type — determines rendering/output path
    ScreenType type{ScreenType::LEDWall};
};

// Assign a Model entity to a Screen and sync its size to the mesh's authored
// bounds. Pass entt::null (or a null Model*) to reset to the flat 16:9 panel
// default. Use this everywhere instead of writing `screen.modelEntity = e;`
// directly — keeps Size and the mesh in lockstep so computeEffectiveScale
// doesn't squash a 3D mesh's Z to 0 against the flat-panel default.
inline void applyModelToScreen(Screen& screen, entt::entity modelEntity,
                                const Model* model) {
    screen.modelEntity = modelEntity;
    if (model && model->mesh.isValid()) {
        screen.size = meshNativeBounds(&model->mesh);
    } else {
        screen.size = {4.0f, 2.25f, 0.0f};
    }
}

/**
 * Feed mapping types (inspired by disguise)
 */
enum class FeedMappingType {
    Direct,         // Direct UV mapping from model
    Perspective,    // Perspective projection onto surface
    Cylindrical,    // Cylindrical unwrap
    Spherical,      // Spherical unwrap
    Custom          // Custom UV coordinates
};

/**
 * Feed mapping for advanced clip-to-screen routing
 *
 * Allows clips to be mapped to screens with custom transformations,
 * similar to disguise's feed mapping system.
 */
struct FeedMapping {
    std::string name{"Feed 1"};

    // Source clip entity (or null for any clip targeting this feed)
    entt::entity sourceClip{entt::null};

    // Target screen entity
    entt::entity targetScreen{entt::null};

    // Mapping type
    FeedMappingType type{FeedMappingType::Direct};

    // UV transform (offset and scale within the screen)
    std::array<float, 2> uvOffset{0.0f, 0.0f};
    std::array<float, 2> uvScale{1.0f, 1.0f};
    float uvRotation{0.0f};  // Degrees

    // Soft edge blending
    bool softEdgeEnabled{false};
    std::array<float, 4> softEdge{0.0f, 0.0f, 0.0f, 0.0f};  // Left, Right, Top, Bottom (0-1)

    // Color correction per-feed
    float brightness{1.0f};
    float contrast{1.0f};
    float gamma{1.0f};
};

} // namespace entity
