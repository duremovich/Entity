#include "entity/systems/CameraControlSystem.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace entity {

void CameraControlSystem::updateFromOrbit(Camera& camera) {
    camera.orbitPitch    = glm::clamp(camera.orbitPitch,    Camera::MIN_PITCH,    Camera::MAX_PITCH);
    camera.orbitDistance = glm::clamp(camera.orbitDistance, Camera::MIN_DISTANCE, Camera::MAX_DISTANCE);

    const float yawRad   = glm::radians(camera.orbitYaw);
    const float pitchRad = glm::radians(camera.orbitPitch);

    const float x = camera.orbitDistance * std::cos(pitchRad) * std::sin(yawRad);
    const float y = camera.orbitDistance * std::sin(pitchRad);
    const float z = camera.orbitDistance * std::cos(pitchRad) * std::cos(yawRad);

    camera.position = camera.orbitTarget + glm::vec3(x, y, z);
    camera.target   = camera.orbitTarget;
}

void CameraControlSystem::orbit(Camera& camera, float deltaYaw, float deltaPitch) {
    camera.orbitYaw   += deltaYaw;
    camera.orbitPitch += deltaPitch;
    updateFromOrbit(camera);
}

void CameraControlSystem::pan(Camera& camera, float deltaX, float deltaY) {
    const glm::vec3 forward = glm::normalize(camera.target - camera.position);
    const glm::vec3 right   = glm::normalize(glm::cross(forward, camera.up));
    const glm::vec3 worldUp = glm::normalize(glm::cross(right, forward));

    camera.orbitTarget -= right * deltaX;
    camera.orbitTarget += worldUp * deltaY;
    updateFromOrbit(camera);
}

void CameraControlSystem::zoom(Camera& camera, float delta) {
    camera.orbitDistance -= delta;
    updateFromOrbit(camera);
}

void CameraControlSystem::reset(Camera& camera) {
    camera.orbitYaw      = 0.0f;
    camera.orbitPitch    = 20.0f;
    camera.orbitDistance = 5.0f;
    camera.orbitTarget   = glm::vec3(0.0f, 0.5f, 0.0f);
    camera.fov           = 45.0f;
    updateFromOrbit(camera);
}

void CameraControlSystem::setFrontView(Camera& camera) {
    camera.orbitYaw      = 0.0f;
    camera.orbitPitch    = 0.0f;
    camera.orbitDistance = 5.0f;
    camera.orbitTarget   = glm::vec3(0.0f, 0.5f, 0.0f);
    updateFromOrbit(camera);
}

void CameraControlSystem::setTopView(Camera& camera) {
    camera.orbitYaw      = 0.0f;
    camera.orbitPitch    = 89.0f;
    camera.orbitDistance = 5.0f;
    camera.orbitTarget   = glm::vec3(0.0f, 0.0f, 0.0f);
    updateFromOrbit(camera);
}

void CameraControlSystem::setSideView(Camera& camera) {
    camera.orbitYaw      = 90.0f;
    camera.orbitPitch    = 0.0f;
    camera.orbitDistance = 5.0f;
    camera.orbitTarget   = glm::vec3(0.0f, 0.5f, 0.0f);
    updateFromOrbit(camera);
}

} // namespace entity
