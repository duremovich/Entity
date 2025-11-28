#include "entity/media/ObjLoader.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <limits>

namespace entity {

void MeshData::calculateBounds() {
    if (vertices.empty()) return;

    minBounds = {std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
    maxBounds = {std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest(),
                 std::numeric_limits<float>::lowest()};

    for (const auto& v : vertices) {
        for (int i = 0; i < 3; ++i) {
            minBounds[i] = std::min(minBounds[i], v.position[i]);
            maxBounds[i] = std::max(maxBounds[i], v.position[i]);
        }
    }
}

std::vector<std::string> ObjLoader::split(const std::string& str, char delimiter) {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter)) {
        if (!token.empty()) {
            result.push_back(token);
        }
    }
    return result;
}

ObjLoader::FaceVertex ObjLoader::parseFaceVertex(const std::string& str) {
    FaceVertex fv;
    auto parts = split(str, '/');

    if (parts.size() >= 1 && !parts[0].empty()) {
        fv.posIndex = std::stoi(parts[0]) - 1; // OBJ indices are 1-based
    }
    if (parts.size() >= 2 && !parts[1].empty()) {
        fv.texIndex = std::stoi(parts[1]) - 1;
    }
    if (parts.size() >= 3 && !parts[2].empty()) {
        fv.normIndex = std::stoi(parts[2]) - 1;
    }

    return fv;
}

MeshData ObjLoader::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "[ObjLoader] Failed to open file: " << filepath << std::endl;
        return MeshData{};
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    // Extract filename as mesh name
    std::string name = filepath;
    auto lastSlash = name.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        name = name.substr(lastSlash + 1);
    }
    auto lastDot = name.find_last_of('.');
    if (lastDot != std::string::npos) {
        name = name.substr(0, lastDot);
    }

    return loadFromString(buffer.str(), name);
}

MeshData ObjLoader::loadFromString(const std::string& objData, const std::string& name) {
    MeshData mesh;
    mesh.name = name;

    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 2>> texCoords;
    std::vector<std::array<float, 3>> normals;

    std::istringstream stream(objData);
    std::string line;

    while (std::getline(stream, line)) {
        // Trim whitespace
        while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            line = line.substr(1);
        }
        if (line.empty() || line[0] == '#') continue;

        std::istringstream lineStream(line);
        std::string prefix;
        lineStream >> prefix;

        if (prefix == "v") {
            // Vertex position
            std::array<float, 3> pos{0.0f, 0.0f, 0.0f};
            lineStream >> pos[0] >> pos[1] >> pos[2];
            positions.push_back(pos);
        }
        else if (prefix == "vt") {
            // Texture coordinate
            std::array<float, 2> tex{0.0f, 0.0f};
            lineStream >> tex[0] >> tex[1];
            // Flip V coordinate (OBJ uses bottom-left origin, D3D uses top-left)
            tex[1] = 1.0f - tex[1];
            texCoords.push_back(tex);
        }
        else if (prefix == "vn") {
            // Normal
            std::array<float, 3> norm{0.0f, 0.0f, 1.0f};
            lineStream >> norm[0] >> norm[1] >> norm[2];
            normals.push_back(norm);
        }
        else if (prefix == "f") {
            // Face - can be triangle or quad
            std::vector<FaceVertex> faceVerts;
            std::string vertStr;
            while (lineStream >> vertStr) {
                faceVerts.push_back(parseFaceVertex(vertStr));
            }

            // Triangulate face (support quads and n-gons)
            for (size_t i = 2; i < faceVerts.size(); ++i) {
                // Triangle fan from first vertex
                std::array<FaceVertex, 3> tri = {faceVerts[0], faceVerts[i - 1], faceVerts[i]};

                for (const auto& fv : tri) {
                    MeshVertex vertex;

                    if (fv.posIndex >= 0 && fv.posIndex < static_cast<int>(positions.size())) {
                        vertex.position = positions[fv.posIndex];
                    }
                    if (fv.texIndex >= 0 && fv.texIndex < static_cast<int>(texCoords.size())) {
                        vertex.texCoord = texCoords[fv.texIndex];
                    }
                    if (fv.normIndex >= 0 && fv.normIndex < static_cast<int>(normals.size())) {
                        vertex.normal = normals[fv.normIndex];
                    }

                    mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
                    mesh.vertices.push_back(vertex);
                }
            }
        }
    }

    mesh.calculateBounds();

    std::cout << "[ObjLoader] Loaded " << name << ": "
              << mesh.vertices.size() << " vertices, "
              << mesh.indices.size() / 3 << " triangles" << std::endl;

    return mesh;
}

MeshData createDefaultScreenMesh() {
    MeshData mesh;
    mesh.name = "Default 16:9 Screen";

    // Create a 16:9 plane centered at origin
    float aspectRatio = 16.0f / 9.0f;
    float halfWidth = aspectRatio / 2.0f;
    float halfHeight = 0.5f;

    // Vertices (CCW winding, facing +Z)
    mesh.vertices = {
        // Position                          TexCoord      Normal
        {{-halfWidth, -halfHeight, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}}, // Bottom-left
        {{ halfWidth, -halfHeight, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}}, // Bottom-right
        {{ halfWidth,  halfHeight, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}, // Top-right
        {{-halfWidth,  halfHeight, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}, // Top-left
    };

    // Two triangles (CCW)
    mesh.indices = {0, 1, 2, 0, 2, 3};

    mesh.calculateBounds();
    return mesh;
}

} // namespace entity
