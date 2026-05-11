#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace entity {

/**
 * Camera component for 3D stage visualization.
 *
 * Pure data: position, orientation, projection params, and orbital
 * control state. Trivial matrix accessors (view / projection) are kept
 * inline — they touch only own fields and removing them would force ~20
 * call-site rewrites for no runtime benefit (same pragma as
 * Transform::getMatrix()).
 *
 * All interactive logic (orbit / pan / zoom / presets / orbit→Cartesian
 * conversion) lives in `entity::CameraControlSystem` — call those
 * directly when you need to mutate a Camera.
 */
struct Camera {
    // Camera position and orientation
    glm::vec3 position{0.0f, 2.0f, 5.0f};
    glm::vec3 target{0.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};

    // Projection parameters
    float fov{45.0f};                          // Field of view in degrees
    float aspectRatio{16.0f / 9.0f};
    float nearPlane{0.1f};
    float farPlane{100.0f};

    // Orbital camera state (consumed by CameraControlSystem)
    float orbitYaw{0.0f};
    float orbitPitch{20.0f};
    float orbitDistance{5.0f};
    glm::vec3 orbitTarget{0.0f, 0.5f, 0.0f};

    // Orbit limits — applied by CameraControlSystem::updateFromOrbit
    static constexpr float MIN_PITCH = -89.0f;
    static constexpr float MAX_PITCH = 89.0f;
    static constexpr float MIN_DISTANCE = 0.5f;
    static constexpr float MAX_DISTANCE = 50.0f;

    glm::mat4 getViewMatrix() const {
        return glm::lookAt(position, target, up);
    }

    glm::mat4 getProjectionMatrix() const {
        return glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
    }

    glm::mat4 getViewProjectionMatrix() const {
        return getProjectionMatrix() * getViewMatrix();
    }
};

} // namespace entity
