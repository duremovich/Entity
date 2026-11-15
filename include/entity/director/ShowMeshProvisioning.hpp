#pragma once

// Editor-side half of the show-thread mesh store (ADR-0014; see
// entity/render/ShowMeshStore.hpp for the show-side half).
//
// The projector output path needs Model geometry on the show thread, which
// must never read the registry. The editor therefore hands the show thread
// its own copy via bus::ProvisionMesh, once per geometry change, and the
// SceneSnapshot carries (modelEntity, meshRevision) to look it up.
// Editor thread only.

#include "entity/bus/Message.hpp"
#include "entity/media/ObjLoader.hpp"

#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

namespace entity {

// Copy the parts of `mesh` the projector path reads (position, texCoord,
// indices) into a ProvisionMesh. Vertex normals and bounds stay behind.
bus::ProvisionMesh makeProvisionMesh(std::uint64_t meshId,
                                     std::uint32_t revision,
                                     const MeshData& mesh);

// For every Model referenced by a Screen whose geometry has not been
// provisioned since it last changed (Model::showMeshRevision == 0) and is
// valid, assign the next revision from `nextRevision` (monotonic, skips 0)
// and append a ProvisionMesh to `out`. The caller sends `out` on D2R BEFORE
// publishing the SceneSnapshot baked afterwards, so the store already holds
// every revision that snapshot names. Unreferenced models are not copied.
void collectShowMeshProvisions(entt::registry& registry,
                               std::uint32_t& nextRevision,
                               std::vector<bus::ProvisionMesh>& out);

} // namespace entity
