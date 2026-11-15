#include "entity/render/ShowMeshStore.hpp"

#include <iostream>
#include <utility>

namespace entity {

bool ShowMeshStore::provision(bus::ProvisionMesh&& msg) {
    const std::size_t vertexCount = msg.positions.size() / 3;
    bool ok = msg.revision != 0 &&
              vertexCount > 0 &&
              msg.positions.size() == vertexCount * 3 &&
              msg.texCoords.size() == vertexCount * 2 &&
              msg.indices.size() % 3 == 0;
    if (ok) {
        for (std::uint32_t idx : msg.indices) {
            if (idx >= vertexCount) { ok = false; break; }
        }
    }
    if (!ok) {
        std::cerr << "[ShowMeshStore] Rejected mesh " << msg.meshId
                  << " rev " << msg.revision << " (" << msg.positions.size()
                  << " position floats, " << msg.texCoords.size()
                  << " uv floats, " << msg.indices.size()
                  << " indices) - malformed payload" << std::endl;
        m_meshes.erase(msg.meshId);
        return false;
    }

    Mesh& mesh = m_meshes[msg.meshId];
    mesh.revision  = msg.revision;
    mesh.positions = std::move(msg.positions);
    mesh.texCoords = std::move(msg.texCoords);
    mesh.indices   = std::move(msg.indices);
    return true;
}

void ShowMeshStore::release(std::uint64_t meshId) {
    m_meshes.erase(meshId);
}

const ShowMeshStore::Mesh* ShowMeshStore::find(std::uint64_t meshId,
                                               std::uint32_t revision) const {
    // meshId 0 is a legal entity (EnTT index 0, version 0) - the bus also
    // uses 0 for "null", so "no mesh" is carried by revision 0 alone.
    if (revision == 0) return nullptr;
    auto it = m_meshes.find(meshId);
    if (it == m_meshes.end() || it->second.revision != revision) return nullptr;
    return &it->second;
}

} // namespace entity
