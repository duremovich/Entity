#pragma once

#include <string>
#include <vector>
#include <array>

namespace entity {

/**
 * Vertex data for 3D mesh rendering
 */
struct MeshVertex {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 2> texCoord{0.0f, 0.0f};
    std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
};

/**
 * Loaded mesh data from OBJ file
 */
struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    std::string name;

    // Bounding box
    std::array<float, 3> minBounds{0.0f, 0.0f, 0.0f};
    std::array<float, 3> maxBounds{0.0f, 0.0f, 0.0f};

    bool isValid() const { return !vertices.empty(); }

    void calculateBounds();
};

/**
 * OBJ file loader for 3D models
 *
 * Supports:
 * - Vertex positions (v)
 * - Texture coordinates (vt)
 * - Normals (vn)
 * - Triangular and quad faces (f)
 *
 * Note: Only loads geometry, not materials
 */
class ObjLoader {
public:
    /**
     * Load an OBJ file from disk
     * @param filepath Path to the .obj file
     * @return MeshData containing the loaded geometry
     */
    static MeshData load(const std::string& filepath);

    /**
     * Load OBJ data from a string (for embedded models)
     * @param objData OBJ format string data
     * @param name Name to assign to the mesh
     * @return MeshData containing the loaded geometry
     */
    static MeshData loadFromString(const std::string& objData, const std::string& name);

private:
    struct FaceVertex {
        int posIndex{-1};
        int texIndex{-1};
        int normIndex{-1};
    };

    static FaceVertex parseFaceVertex(const std::string& str);
    static std::vector<std::string> split(const std::string& str, char delimiter);
};

/**
 * Create a default 16:9 plane mesh (for screens without custom geometry)
 */
MeshData createDefaultScreenMesh();

} // namespace entity
