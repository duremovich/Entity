#include "entity/director/ShowMeshProvisioning.hpp"

#include "entity/components/Model.hpp"
#include "entity/components/Screen.hpp"

namespace entity {

bus::ProvisionMesh makeProvisionMesh(std::uint64_t meshId,
                                     std::uint32_t revision,
                                     const MeshData& mesh) {
    bus::ProvisionMesh pm;
    pm.meshId   = meshId;
    pm.revision = revision;
    pm.positions.reserve(mesh.vertices.size() * 3);
    pm.texCoords.reserve(mesh.vertices.size() * 2);
    for (const MeshVertex& v : mesh.vertices) {
        pm.positions.insert(pm.positions.end(), v.position.begin(), v.position.end());
        pm.texCoords.insert(pm.texCoords.end(), v.texCoord.begin(), v.texCoord.end());
    }
    pm.indices = mesh.indices;
    return pm;
}

void collectShowMeshProvisions(entt::registry& registry,
                               std::uint32_t& nextRevision,
                               std::vector<bus::ProvisionMesh>& out) {
    for (auto [screenEntity, screen] : registry.view<Screen>().each()) {
        (void)screenEntity;
        const entt::entity modelEntity = screen.modelEntity;
        if (modelEntity == entt::null || !registry.valid(modelEntity)) continue;
        Model* model = registry.try_get<Model>(modelEntity);
        if (!model || model->showMeshRevision != 0 || !model->mesh.isValid()) continue;

        if (nextRevision == 0) nextRevision = 1;  // 0 means "no mesh" on the wire
        model->showMeshRevision = nextRevision++;
        out.push_back(makeProvisionMesh(static_cast<std::uint64_t>(modelEntity),
                                        model->showMeshRevision, model->mesh));
    }
}

} // namespace entity
