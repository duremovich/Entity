#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace entity {

/**
 * Camera component for 3D stage visualization.
 *
 * Provides view and projection matrices for rendering
 * the 3D stage environment with floor and screens.
 */
struct Camera {
    // Camera position and orientation
    glm::vec3 position{0.0f, 2.0f, 5.0f};    // Camera position in world space
    glm::vec3 target{0.0f, 0.0f, 0.0f};       // Point camera looks at
    glm::vec3 up{0.0f, 1.0f, 0.0f};           // Up direction (usually Y-up)

    // Projection parameters
    float fov{45.0f};                          // Field of view in degrees
    float aspectRatio{16.0f / 9.0f};          // Width / Height
    float nearPlane{0.1f};                     // Near clipping plane
    float farPlane{100.0f};                    // Far clipping plane

    // Orbital camera state (for mouse control)
    float orbitYaw{0.0f};                      // Horizontal rotation (degrees)
    float orbitPitch{20.0f};                   // Vertical rotation (degrees)
    float orbitDistance{5.0f};                 // Distance from target
    glm::vec3 orbitTarget{0.0f, 0.5f, 0.0f};  // Center point of orbit

    // Orbit limits
    static constexpr float MIN_PITCH = -89.0f;
    static constexpr float MAX_PITCH = 89.0f;
    static constexpr float MIN_DISTANCE = 0.5f;
    static constexpr float MAX_DISTANCE = 50.0f;

    /**
     * Get the view matrix (world to camera transform)
     */
    glm::mat4 getViewMatrix() const {
        return glm::lookAt(position, target, up);
    }

    /**
     * Get the projection matrix (camera to clip space)
     */
    glm::mat4 getProjectionMatrix() const {
        return glm::perspective(glm::radians(fov), aspectRatio, nearPlane, farPlane);
    }

    /**
     * Get combined view-projection matrix
     */
    glm::mat4 getViewProjectionMatrix() const {
        return getProjectionMatrix() * getViewMatrix();
    }

    /**
     * Update camera position from orbital parameters.
     * Call this after modifying orbitYaw, orbitPitch, or orbitDistance.
     */
    void updateFromOrbit() {
        // Clamp pitch to avoid gimbal lock
        orbitPitch = glm::clamp(orbitPitch, MIN_PITCH, MAX_PITCH);
        orbitDistance = glm::clamp(orbitDistance, MIN_DISTANCE, MAX_DISTANCE);

        // Convert spherical coordinates to Cartesian
        float yawRad = glm::radians(orbitYaw);
        float pitchRad = glm::radians(orbitPitch);

        float x = orbitDistance * cos(pitchRad) * sin(yawRad);
        float y = orbitDistance * sin(pitchRad);
        float z = orbitDistance * cos(pitchRad) * cos(yawRad);

        position = orbitTarget + glm::vec3(x, y, z);
        target = orbitTarget;
    }

    /**
     * Orbit the camera (mouse drag rotation)
     * @param deltaYaw Horizontal rotation delta in degrees
     * @param deltaPitch Vertical rotation delta in degrees
     */
    void orbit(float deltaYaw, float deltaPitch) {
        orbitYaw += deltaYaw;
        orbitPitch += deltaPitch;
        updateFromOrbit();
    }

    /**
     * Pan the camera (shift+drag)
     * @param deltaX Horizontal pan in world units
     * @param deltaY Vertical pan in world units
     */
    void pan(float deltaX, float deltaY) {
        // Calculate right and up vectors in world space
        glm::vec3 forward = glm::normalize(target - position);
        glm::vec3 right = glm::normalize(glm::cross(forward, up));
        glm::vec3 worldUp = glm::normalize(glm::cross(right, forward));

        // Move target (orbit center)
        orbitTarget -= right * deltaX;
        orbitTarget += worldUp * deltaY;
        updateFromOrbit();
    }

    /**
     * Zoom the camera (scroll wheel)
     * @param delta Zoom delta (positive = zoom in, negative = zoom out)
     */
    void zoom(float delta) {
        orbitDistance -= delta;
        updateFromOrbit();
    }

    /**
     * Reset camera to default position
     */
    void reset() {
        orbitYaw = 0.0f;
        orbitPitch = 20.0f;
        orbitDistance = 5.0f;
        orbitTarget = glm::vec3(0.0f, 0.5f, 0.0f);
        fov = 45.0f;
        updateFromOrbit();
    }

    /**
     * Set camera to front view preset
     */
    void setFrontView() {
        orbitYaw = 0.0f;
        orbitPitch = 0.0f;
        orbitDistance = 5.0f;
        orbitTarget = glm::vec3(0.0f, 0.5f, 0.0f);
        updateFromOrbit();
    }

    /**
     * Set camera to top view preset
     */
    void setTopView() {
        orbitYaw = 0.0f;
        orbitPitch = 89.0f;
        orbitDistance = 5.0f;
        orbitTarget = glm::vec3(0.0f, 0.0f, 0.0f);
        updateFromOrbit();
    }

    /**
     * Set camera to side view preset
     */
    void setSideView() {
        orbitYaw = 90.0f;
        orbitPitch = 0.0f;
        orbitDistance = 5.0f;
        orbitTarget = glm::vec3(0.0f, 0.5f, 0.0f);
        updateFromOrbit();
    }
};

} // namespace entity
