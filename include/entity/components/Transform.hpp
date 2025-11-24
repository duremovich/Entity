#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace entity {

/**
 * Transform component for spatial positioning, rotation, and scaling.
 *
 * This component stores both the individual transform parameters
 * and a cached transformation matrix for efficient rendering.
 */
struct Transform {
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 rotation{0.0f, 0.0f, 0.0f};  // Euler angles in degrees
    glm::vec3 scale{1.0f, 1.0f, 1.0f};

    // Cached transformation matrix (mutable for lazy evaluation in const methods)
    mutable glm::mat4 matrix{1.0f};
    mutable bool dirty{true};  // True if matrix needs to be recalculated

    /**
     * Update the transformation matrix from position, rotation, and scale.
     * Only recalculates if dirty flag is set.
     */
    void updateMatrix() const {
        if (!dirty) return;

        // Build transformation matrix: Translation * Rotation * Scale
        matrix = glm::mat4(1.0f);
        matrix = glm::translate(matrix, position);

        // Apply rotation (Z * Y * X order)
        matrix = glm::rotate(matrix, glm::radians(rotation.z), glm::vec3(0, 0, 1));
        matrix = glm::rotate(matrix, glm::radians(rotation.y), glm::vec3(0, 1, 0));
        matrix = glm::rotate(matrix, glm::radians(rotation.x), glm::vec3(1, 0, 0));

        matrix = glm::scale(matrix, scale);

        dirty = false;
    }

    /**
     * Get the transformation matrix, updating it if necessary.
     */
    const glm::mat4& getMatrix() const {
        updateMatrix();
        return matrix;
    }

    /**
     * Set position and mark matrix as dirty.
     */
    void setPosition(const glm::vec3& pos) {
        position = pos;
        dirty = true;
    }

    /**
     * Set rotation (in degrees) and mark matrix as dirty.
     */
    void setRotation(const glm::vec3& rot) {
        rotation = rot;
        dirty = true;
    }

    /**
     * Set scale and mark matrix as dirty.
     */
    void setScale(const glm::vec3& scl) {
        scale = scl;
        dirty = true;
    }
};

} // namespace entity
