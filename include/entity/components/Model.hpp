#pragma once

#include "entity/media/ObjLoader.hpp"
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

    bool isValid() const { return mesh.isValid(); }
};

} // namespace entity
