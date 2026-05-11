#pragma once

#include "entity/components/Camera.hpp"

namespace entity {

/**
 * CameraControlSystem - Interactive control logic for editor Camera entities.
 *
 * Owns mouse-input-driven manipulation (orbit / pan / zoom), preset views,
 * and the spherical-to-Cartesian conversion that ties orbit parameters back
 * to the camera's position/target fields.
 *
 * Camera is kept as POD data + trivial matrix accessors
 * (`getViewMatrix` / `getProjectionMatrix`); anything that reaches across
 * the camera's own state to do work — clamping orbit limits, computing
 * pan basis vectors, applying presets — lives here.
 *
 * All methods are stateless static utilities. Call them directly:
 *   CameraControlSystem::orbit(camera, deltaYaw, deltaPitch);
 */
class CameraControlSystem {
public:
    /**
     * Recompute camera.position from the orbit parameters
     * (orbitYaw / orbitPitch / orbitDistance / orbitTarget).
     * Clamps pitch and distance to Camera::MIN_/MAX_ limits.
     */
    static void updateFromOrbit(Camera& camera);

    /**
     * Orbit the camera around its target (mouse drag rotation).
     * @param deltaYaw   Horizontal rotation delta in degrees.
     * @param deltaPitch Vertical rotation delta in degrees.
     */
    static void orbit(Camera& camera, float deltaYaw, float deltaPitch);

    /**
     * Pan the camera (shift+drag or middle-drag) by translating the
     * orbit target in screen-relative world space.
     * @param deltaX Horizontal pan in world units.
     * @param deltaY Vertical pan in world units.
     */
    static void pan(Camera& camera, float deltaX, float deltaY);

    /**
     * Zoom the camera (scroll wheel or right-drag) by adjusting
     * orbit distance.
     * @param delta Zoom delta (positive = zoom in, negative = zoom out).
     */
    static void zoom(Camera& camera, float delta);

    /**
     * Reset camera to the default orbit pose.
     */
    static void reset(Camera& camera);

    /** Preset: front-facing view. */
    static void setFrontView(Camera& camera);

    /** Preset: top-down view. */
    static void setTopView(Camera& camera);

    /** Preset: side view. */
    static void setSideView(Camera& camera);
};

} // namespace entity
