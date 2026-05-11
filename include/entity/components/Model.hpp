#pragma once

#include "entity/media/ObjLoader.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <memory>

namespace entity {

/**
 * Model component - holds 3D mesh data loaded from OBJ files
 *
 * Models are used as geometry for screens (projection surfaces)
 * and can be reused across multiple screens.
 */
struct Model {
    std::string name;           // Display name
    std::string filepath;       // Original file path (empty if built-in)
    MeshData mesh;              // Loaded mesh data

    // GPU resource handles (set by renderer)
    uint32_t vertexBufferSlot{UINT32_MAX};  // Slot in vertex buffer pool
    uint32_t indexBufferSlot{UINT32_MAX};   // Slot in index buffer pool
    bool gpuResourcesValid{false};
    bool gpuUploadFailed{false};  // Set on first slot-exhaustion; suppresses retry log spam

    bool isValid() const { return mesh.isValid(); }
};

// Native bounds of a model (mesh extents along each axis). Falls back to the
// default 16:9 screen-plane bounds (1.778m × 1m × 0 deep) when no valid mesh
// is supplied — that's the geometry Screens render when modelEntity is null.
inline std::array<float, 3> meshNativeBounds(const MeshData* mesh) {
    if (mesh && mesh->isValid()) {
        return {
            mesh->maxBounds[0] - mesh->minBounds[0],
            mesh->maxBounds[1] - mesh->minBounds[1],
            mesh->maxBounds[2] - mesh->minBounds[2],
        };
    }
    return {16.0f / 9.0f, 1.0f, 0.0f};
}

// Convert a user-facing size (meters, Blender 1u = 1m convention) into the
// vertex-multiplier "effective scale" that the render path applies via
// glm::scale. Divides per-axis size by the mesh's native bounds; clamps
// degenerate axes (bounds < 1e-6) to avoid blowing up. For a Blender-authored
// mesh whose native bounds match the user's size, this returns 1.0 per axis.
inline std::array<float, 3> computeEffectiveScale(
    const std::array<float, 3>& sizeMeters,
    const MeshData* mesh)
{
    const auto bounds = meshNativeBounds(mesh);
    constexpr float kEps = 1e-6f;
    return {
        sizeMeters[0] / std::max(bounds[0], kEps),
        sizeMeters[1] / std::max(bounds[1], kEps),
        sizeMeters[2] / std::max(bounds[2], kEps),
    };
}

} // namespace entity
