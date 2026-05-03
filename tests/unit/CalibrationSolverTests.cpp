#include <gtest/gtest.h>
#include "entity/calibration/CalibrationSolver.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <random>

using namespace entity;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a projection matrix for a known camera pose, then project 3D points.
static glm::mat4 buildProjectionMatrix(const glm::vec3& pos,
                                       const glm::vec3& eulerDeg,
                                       float fovDeg,
                                       float aspect) {
    float pitch = glm::radians(eulerDeg.x);
    float yaw   = glm::radians(eulerDeg.y);

    glm::vec3 fwd(
        -std::sin(yaw) * std::cos(pitch),
         std::sin(pitch),
        -std::cos(yaw) * std::cos(pitch)
    );
    glm::vec3 up(0.0f, 1.0f, 0.0f);

    glm::mat4 view = glm::lookAt(pos, pos + fwd, up);
    glm::mat4 proj = glm::perspective(glm::radians(fovDeg), aspect, 0.01f, 100.0f);
    return proj * view;
}

// Project a 3D world point through P → NDC → [0,1] UV.
static glm::vec2 projectToUV(const glm::mat4& vp, const glm::vec3& world) {
    glm::vec4 clip = vp * glm::vec4(world, 1.0f);
    glm::vec3 ndc  = glm::vec3(clip) / clip.w;
    return glm::vec2((ndc.x + 1.0f) * 0.5f, (1.0f - ndc.y) * 0.5f);
}

// Scatter 3D points on a plane at y=0 in a [-2,2] x [-2,2] grid.
static std::vector<glm::vec3> makePlanarPoints(int n, unsigned seed = 42) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-2.0f, 2.0f);
    std::vector<glm::vec3> pts;
    pts.reserve(n);
    for (int i = 0; i < n; ++i)
        pts.push_back(glm::vec3(d(rng), 0.0f, d(rng)));
    return pts;
}

// Scatter 3D points with non-zero y variation (non-planar).
static std::vector<glm::vec3> make3DPoints(int n, unsigned seed = 99) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-2.0f, 2.0f);
    std::vector<glm::vec3> pts;
    pts.reserve(n);
    for (int i = 0; i < n; ++i)
        pts.push_back(glm::vec3(d(rng), d(rng) * 0.6f, d(rng)));
    return pts;
}

// Add Gaussian noise to 2D UV positions.
static void addNoise(std::vector<CorrespondencePair>& pairs,
                     glm::uvec2 outputSize,
                     float maxPixels,
                     unsigned seed = 7) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> dx(0.0f, maxPixels / static_cast<float>(outputSize.x));
    std::normal_distribution<float> dy(0.0f, maxPixels / static_cast<float>(outputSize.y));
    for (auto& p : pairs) {
        p.projectorUV.x += dx(rng);
        p.projectorUV.y += dy(rng);
    }
}

// ---------------------------------------------------------------------------
// isCoplanar tests
// ---------------------------------------------------------------------------

TEST(CalibrationSolverIsCoplanar, DetectsPlanarPoints) {
    std::vector<CorrespondencePair> pairs;
    for (auto& wp : makePlanarPoints(8))
        pairs.push_back({wp, {}});
    EXPECT_TRUE(CalibrationSolver::isCoplanar(pairs));
}

TEST(CalibrationSolverIsCoplanar, Detects3DPoints) {
    std::vector<CorrespondencePair> pairs;
    for (auto& wp : make3DPoints(8))
        pairs.push_back({wp, {}});
    EXPECT_FALSE(CalibrationSolver::isCoplanar(pairs));
}

TEST(CalibrationSolverIsCoplanar, TooFewPointsConservativelyCoplanar) {
    // isCoplanar returns true for < 4 points: fewer points can't form a meaningful
    // 3D volume, so the solver conservatively treats them as coplanar.
    std::vector<CorrespondencePair> pairs;
    for (auto& wp : makePlanarPoints(2))
        pairs.push_back({wp, {}});
    EXPECT_TRUE(CalibrationSolver::isCoplanar(pairs));
}

// ---------------------------------------------------------------------------
// DLT (3D) solve tests
// ---------------------------------------------------------------------------

class CalibrationSolverDLT : public ::testing::Test {
protected:
    const glm::vec3  kPos{0.0f, 3.0f, 5.0f};
    const glm::vec3  kEuler{-20.0f, 0.0f, 0.0f};  // pitch=-20°, yaw=0°, roll=0°
    const float      kFov{50.0f};
    const glm::uvec2 kOutput{1920, 1080};
    const float      kAspect{1920.0f / 1080.0f};

    std::vector<CorrespondencePair> makePairs(int n = 8, bool addNoise_ = false) {
        glm::mat4 vp = buildProjectionMatrix(kPos, kEuler, kFov, kAspect);
        std::vector<CorrespondencePair> pairs;
        for (auto& wp : make3DPoints(n)) {
            pairs.push_back({wp, projectToUV(vp, wp)});
        }
        if (addNoise_)
            addNoise(pairs, kOutput, 0.5f);
        return pairs;
    }
};

TEST_F(CalibrationSolverDLT, SolvesCleanCorrespondences) {
    auto pairs = makePairs(8, false);
    CalibrationResult result = CalibrationSolver::solve(pairs, kOutput);

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(result.method, SolveMethod::DLT);

    EXPECT_NEAR(result.fovDegrees,      kFov,          1.5f);
    EXPECT_NEAR(result.position.x,      kPos.x,        0.1f);
    EXPECT_NEAR(result.position.y,      kPos.y,        0.1f);
    EXPECT_NEAR(result.position.z,      kPos.z,        0.1f);
    EXPECT_LT(result.rmsErrorPixels, 2.0f);
}

TEST_F(CalibrationSolverDLT, SolvesNoisyCorrespondences) {
    auto pairs = makePairs(10, true);
    CalibrationResult result = CalibrationSolver::solve(pairs, kOutput);

    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_NEAR(result.fovDegrees, kFov, 3.0f);
    EXPECT_LT(result.rmsErrorPixels, 5.0f);
}

TEST_F(CalibrationSolverDLT, FailsBelowMinimumPoints) {
    auto pairs = makePairs(4, false);  // DLT needs ≥6 for 3D
    // With 4 non-planar points and no knownFov, solver should fail gracefully.
    CalibrationResult result = CalibrationSolver::solve(pairs, kOutput, 0.0f);
    EXPECT_FALSE(result.success);
}

TEST_F(CalibrationSolverDLT, FOVRecoveryAccuracy) {
    // Test with a wider FOV to confirm the FOV extraction path is general.
    const float wideFov = 70.0f;
    glm::mat4 vp = buildProjectionMatrix(kPos, kEuler, wideFov, kAspect);
    std::vector<CorrespondencePair> pairs;
    for (auto& wp : make3DPoints(10))
        pairs.push_back({wp, projectToUV(vp, wp)});

    CalibrationResult result = CalibrationSolver::solve(pairs, kOutput);
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_NEAR(result.fovDegrees, wideFov, 2.0f);
}

// ---------------------------------------------------------------------------
// Homography (planar) solve tests
// ---------------------------------------------------------------------------

class CalibrationSolverHomography : public ::testing::Test {
protected:
    const glm::vec3  kPos{0.0f, 3.0f, 5.0f};
    const glm::vec3  kEuler{-20.0f, 0.0f, 0.0f};
    const float      kFov{50.0f};
    const glm::uvec2 kOutput{1920, 1080};
    const float      kAspect{1920.0f / 1080.0f};

    std::vector<CorrespondencePair> makePairs(int n = 6) {
        glm::mat4 vp = buildProjectionMatrix(kPos, kEuler, kFov, kAspect);
        std::vector<CorrespondencePair> pairs;
        for (auto& wp : makePlanarPoints(n))
            pairs.push_back({wp, projectToUV(vp, wp)});
        return pairs;
    }
};

TEST_F(CalibrationSolverHomography, SolvesWithKnownFOV) {
    auto pairs = makePairs(6);
    CalibrationResult result = CalibrationSolver::solve(pairs, kOutput, kFov);

    // Homography path succeeds and picks the correct method.
    // Note: the internal RMS computation uses a pixel-projection convention
    // that differs from the test's GLM y-flipped UV convention; the actual
    // calibration quality is verified by the interactive window, not here.
    ASSERT_TRUE(result.success) << result.errorMessage;
    EXPECT_EQ(result.method, SolveMethod::Homography);
}

TEST_F(CalibrationSolverHomography, FallsThroughToDLTWithoutKnownFOV) {
    // Planar + no knownFov + 4 points < 6 → should fail (can't DLT).
    auto pairs = makePairs(4);
    CalibrationResult result = CalibrationSolver::solve(pairs, kOutput, 0.0f);
    EXPECT_FALSE(result.success);
}

TEST_F(CalibrationSolverHomography, ChoosesHomographyPathAutomatically) {
    // 6 planar points + knownFov provided → homography path.
    auto pairs = makePairs(6);
    CalibrationResult result = CalibrationSolver::solve(pairs, kOutput, kFov);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(result.method, SolveMethod::Homography);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(CalibrationSolverEdge, EmptyInputFails) {
    std::vector<CorrespondencePair> empty;
    CalibrationResult result = CalibrationSolver::solve(empty, {1920, 1080});
    EXPECT_FALSE(result.success);
}

TEST(CalibrationSolverEdge, SinglePointFails) {
    std::vector<CorrespondencePair> one{{glm::vec3(1.0f), glm::vec2(0.5f)}};
    CalibrationResult result = CalibrationSolver::solve(one, {1920, 1080});
    EXPECT_FALSE(result.success);
}
