#pragma once

#include "../core/Types.hpp"
#include <glm/glm.hpp>
#include <string>
#include <array>

namespace entity {

/**
 * Corner indices for mapping surfaces.
 */
enum class SurfaceCorner {
    TopLeft = 0,
    TopRight = 1,
    BottomRight = 2,
    BottomLeft = 3
};

/**
 * MappingSurface component for projection mapping.
 *
 * Defines a quadrilateral surface that maps video content to arbitrary
 * 4-corner positions. Used for corner-pin warping in projection mapping.
 *
 * Coordinates are in normalized device coordinates (-1 to 1) or
 * pixel coordinates depending on the coordinate mode.
 */
struct MappingSurface {
    // Surface identification
    std::string name{"Surface"};
    uint32_t surfaceIndex{0};

    // Corner positions (in order: TL, TR, BR, BL)
    // Default: unit quad centered at origin
    std::array<glm::vec2, 4> corners{{
        {-0.5f,  0.5f},   // Top-left
        { 0.5f,  0.5f},   // Top-right
        { 0.5f, -0.5f},   // Bottom-right
        {-0.5f, -0.5f}    // Bottom-left
    }};

    // Source UV coordinates (which part of the input to map)
    // Default: full texture (0,0) to (1,1)
    std::array<glm::vec2, 4> sourceUVs{{
        {0.0f, 0.0f},     // Top-left
        {1.0f, 0.0f},     // Top-right
        {1.0f, 1.0f},     // Bottom-right
        {0.0f, 1.0f}      // Bottom-left
    }};

    // Soft edge (feather) amounts for edge blending (0-1)
    struct SoftEdge {
        float left{0.0f};
        float right{0.0f};
        float top{0.0f};
        float bottom{0.0f};
    } softEdge;

    // Visibility and rendering
    bool visible{true};
    bool selected{false};
    float brightness{1.0f};       // Overall brightness multiplier
    float gamma{1.0f};            // Gamma correction (1.0 = linear)

    // Grid subdivision for smoother warping (1 = no subdivision)
    uint32_t gridSubdivisions{1};

    // Output target (which display/output this surface renders to)
    uint32_t outputIndex{0};

    // Source region within the output's input (defaults to full input)
    // These are normalized coordinates (0-1) within the output's input region.
    // For example, if the output's input region is the left half of the raster,
    // and sourceRegion is (0.5, 0, 0.5, 1), this surface will show
    // the right half of that left-half region.
    struct SourceRegion {
        float x{0.0f};
        float y{0.0f};
        float width{1.0f};
        float height{1.0f};

        bool isFullRegion() const {
            return x == 0.0f && y == 0.0f && width == 1.0f && height == 1.0f;
        }

        void reset() {
            x = 0.0f;
            y = 0.0f;
            width = 1.0f;
            height = 1.0f;
        }
    } sourceRegion;

    // Helper methods

    /**
     * Get corner position by index.
     */
    glm::vec2& getCorner(SurfaceCorner corner) {
        return corners[static_cast<size_t>(corner)];
    }

    const glm::vec2& getCorner(SurfaceCorner corner) const {
        return corners[static_cast<size_t>(corner)];
    }

    /**
     * Set corner position.
     */
    void setCorner(SurfaceCorner corner, const glm::vec2& position) {
        corners[static_cast<size_t>(corner)] = position;
    }

    /**
     * Reset to default unit quad.
     */
    void resetCorners() {
        corners = {{
            {-0.5f,  0.5f},
            { 0.5f,  0.5f},
            { 0.5f, -0.5f},
            {-0.5f, -0.5f}
        }};
    }

    /**
     * Get center point of the surface.
     */
    glm::vec2 getCenter() const {
        return (corners[0] + corners[1] + corners[2] + corners[3]) * 0.25f;
    }

    /**
     * Move entire surface by offset.
     */
    void translate(const glm::vec2& offset) {
        for (auto& corner : corners) {
            corner += offset;
        }
    }

    /**
     * Scale surface around center.
     */
    void scale(float factor) {
        glm::vec2 center = getCenter();
        for (auto& corner : corners) {
            corner = center + (corner - center) * factor;
        }
    }

    /**
     * Check if a point is inside the surface (approximate).
     * Uses simple bounding box check - not precise for warped quads.
     */
    bool containsPoint(const glm::vec2& point) const {
        float minX = corners[0].x, maxX = corners[0].x;
        float minY = corners[0].y, maxY = corners[0].y;

        for (const auto& c : corners) {
            minX = std::min(minX, c.x);
            maxX = std::max(maxX, c.x);
            minY = std::min(minY, c.y);
            maxY = std::max(maxY, c.y);
        }

        return point.x >= minX && point.x <= maxX &&
               point.y >= minY && point.y <= maxY;
    }

    /**
     * Find nearest corner to a point.
     * Returns corner index (0-3) and distance squared.
     */
    std::pair<int, float> findNearestCorner(const glm::vec2& point) const {
        int nearest = 0;
        float nearestDistSq = glm::dot(corners[0] - point, corners[0] - point);

        for (int i = 1; i < 4; ++i) {
            float distSq = glm::dot(corners[i] - point, corners[i] - point);
            if (distSq < nearestDistSq) {
                nearest = i;
                nearestDistSq = distSq;
            }
        }

        return {nearest, nearestDistSq};
    }

    /**
     * Update sourceUVs based on sourceRegion.
     * Call this after modifying sourceRegion to recalculate texture coords.
     */
    void updateSourceUVsFromRegion() {
        float left = sourceRegion.x;
        float right = sourceRegion.x + sourceRegion.width;
        float top = sourceRegion.y;
        float bottom = sourceRegion.y + sourceRegion.height;

        sourceUVs[0] = {left, top};      // Top-left
        sourceUVs[1] = {right, top};     // Top-right
        sourceUVs[2] = {right, bottom};  // Bottom-right
        sourceUVs[3] = {left, bottom};   // Bottom-left
    }

    /**
     * Reset sourceUVs and sourceRegion to full texture.
     */
    void resetSourceRegion() {
        sourceRegion.reset();
        sourceUVs = {{
            {0.0f, 0.0f},
            {1.0f, 0.0f},
            {1.0f, 1.0f},
            {0.0f, 1.0f}
        }};
    }
};

} // namespace entity
