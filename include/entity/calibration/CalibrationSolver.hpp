#pragma once

#include <string>
#include <vector>
#include <glm/glm.hpp>

namespace entity {

struct CorrespondencePair {
    glm::vec3 worldPos;    // 3D point on mesh in world space
    glm::vec2 projectorUV; // corresponding [0,1] position in projector output
};

enum class SolveMethod {
    Homography, // coplanar case (needs known FOV), min 4 points
    DLT,        // 3D case, min 6 points
};

struct CalibrationResult {
    glm::vec3 position;        // recovered projector world position
    glm::vec3 rotationEuler;   // Euler degrees: pitch(X), yaw(Y), roll(Z)
    float     fovDegrees{50.f};
    // Radial lens distortion (Brown-Conrady, 2-term). Applied to normalized
    // image coords as (1 + k1·r² + k2·r⁴). 0 = ideal pinhole.
    float     distortionK1{0.f};
    float     distortionK2{0.f};
    float     rmsErrorPixels{0.f};
    SolveMethod method{SolveMethod::DLT};
    bool      success{false};
    std::string errorMessage;
};

class CalibrationSolver {
public:
    // Solve for projector camera parameters.
    //
    // outputSize: physical projector resolution in pixels.
    // knownFovDegrees: if > 0 and points are coplanar, uses homography decomposition
    //   with K built from this FOV instead of the full DLT. Pass the Projector component's
    //   current fovDegrees here for flat-surface calibration.
    //
    // Minimum: 4 points when coplanar + known FOV; 6 points otherwise.
    static CalibrationResult solve(const std::vector<CorrespondencePair>& pairs,
                                   glm::uvec2 outputSize,
                                   float knownFovDegrees = 0.0f);

    // Refine a projector pose by minimizing reprojection error via
    // Levenberg-Marquardt nonlinear optimization. Optimizes 7 DOF (position +
    // pitch/yaw/roll Euler + FOV) simultaneously starting from the supplied
    // initial guess.
    //
    // Compared to the analytical solve():
    //   - No DLT/homography sign ambiguity — converges to the local minimum
    //     near the initial pose, naturally picking the correct side of the plane.
    //   - Same coordinate convention end-to-end (matches buildProjectorCamera).
    //   - Tolerates noisy or sparse correspondences ("works-ish" if it can't
    //     be exact); reports the residual RMS error.
    //
    // Initial guess: pass the projector's current pose. For first-time calibration,
    // place the projector roughly in the right region first via PropertyWindow.
    static CalibrationResult solveRefine(const std::vector<CorrespondencePair>& pairs,
                                          glm::uvec2 outputSize,
                                          const glm::vec3& initialPos,
                                          const glm::vec3& initialRotEuler,
                                          float initialFovDegrees);

    // Return true if the 3D world points are approximately coplanar.
    // Uses SVD of the centered point cloud; coplanar when the smallest
    // singular value < tolerance * largest.
    static bool isCoplanar(const std::vector<CorrespondencePair>& pairs,
                           float tolerance = 0.01f);
};

} // namespace entity
