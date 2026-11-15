#pragma once

/**
 * ShowMeshStore - show-thread-owned copies of Model geometry (ADR-0014).
 *
 * The projector output path (OutputManager::renderToOutput) projects,
 * lens-distorts and warps every mesh vertex on the CPU each frame, so it
 * needs vertex data, not a GPU buffer slot. It used to read Model::mesh out
 * of the live registry, which the editor thread frees on model delete, mesh
 * hot reload and project close/load. This store holds the show thread's own
 * copy instead, filled by bus::ProvisionMesh and emptied by bus::ReleaseMesh.
 * Nothing the editor does can free memory the show thread is reading.
 *
 * Lookup is keyed by (meshId, revision) from the ScreenSnapshot: a snapshot
 * that predates a hot reload (old revision) or outlives a delete (id gone)
 * finds nothing, and the caller skips that target for the frame instead of
 * drawing geometry the snapshot was not baked against. Revisions are
 * engine-wide monotonic and never reused, so the generation check is exact.
 *
 * Single-threaded: every method runs on the show thread.
 */

#include "entity/bus/Message.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace entity {

class ShowMeshStore {
public:
    struct Mesh {
        std::uint32_t              revision{0};
        std::vector<float>         positions;  // xyz per vertex
        std::vector<float>         texCoords;  // uv per vertex
        std::vector<std::uint32_t> indices;    // whole triangles, all < vertexCount()

        std::size_t vertexCount() const { return positions.size() / 3; }
    };

    /**
     * Store (or replace) the geometry for msg.meshId. Validates the payload
     * once here so the per-frame draw loop needs no bounds checks: revision
     * non-zero, positions a multiple of 3, two texCoords per vertex, a whole
     * number of triangles, every index in range. A rejected payload also
     * drops any older revision for the id (the snapshot that follows names
     * the rejected revision, which must not fall back to stale geometry).
     * Returns false on rejection.
     */
    bool provision(bus::ProvisionMesh&& msg);

    void release(std::uint64_t meshId);
    void clear() { m_meshes.clear(); }

    /** nullptr unless meshId is present at exactly `revision`. */
    const Mesh* find(std::uint64_t meshId, std::uint32_t revision) const;

    std::size_t size() const { return m_meshes.size(); }

private:
    std::unordered_map<std::uint64_t, Mesh> m_meshes;
};

} // namespace entity
