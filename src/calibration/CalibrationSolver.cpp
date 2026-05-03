#include "entity/calibration/CalibrationSolver.hpp"

#include <Eigen/Dense>
#include <Eigen/SVD>
#include <unsupported/Eigen/NonLinearOptimization>
#include <unsupported/Eigen/NumericalDiff>

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <tuple>
#include <vector>

namespace entity {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static Eigen::Matrix3d antiDiagIdentity() {
    Eigen::Matrix3d J = Eigen::Matrix3d::Zero();
    J(0, 2) = J(1, 1) = J(2, 0) = 1.0;
    return J;
}

// RQ decomposition of 3×3 matrix M = K * Q
// K upper-triangular, Q orthogonal.
// Returns false if M is (near-)singular.
static bool rqDecompose3x3(const Eigen::Matrix3d& M,
                            Eigen::Matrix3d& K,
                            Eigen::Matrix3d& Q) {
    Eigen::Matrix3d J = antiDiagIdentity();
    // Flip M around both axes; QR of the transposed flip gives LQ of M.
    Eigen::Matrix3d A = J * M * J;
    Eigen::HouseholderQR<Eigen::Matrix3d> qr(A.transpose());
    Eigen::Matrix3d R0 = qr.matrixQR().triangularView<Eigen::Upper>();
    Eigen::Matrix3d Q0 = qr.householderQ();

    K = J * R0.transpose() * J;
    Q = J * Q0.transpose() * J;

    // Make K[2,2] = 1 (homogeneous normalisation)
    double scale = K(2, 2);
    if (std::abs(scale) < 1e-10) return false;
    K /= scale;
    Q *= scale;

    // Ensure positive diagonal in K (flip columns/rows if needed)
    for (int i = 0; i < 2; ++i) {
        if (K(i, i) < 0.0) {
            K.col(i) = -K.col(i);
            Q.row(i) = -Q.row(i);
        }
    }
    return true;
}

// Hartley normalisation for 2D pixel points.
// Returns the 3×3 normalisation matrix T such that T*[u;v;1] → normalised.
static Eigen::Matrix3d normalise2D(const std::vector<glm::vec2>& pts,
                                   std::vector<Eigen::Vector2d>& out) {
    double mx = 0, my = 0;
    for (auto& p : pts) { mx += p.x; my += p.y; }
    mx /= pts.size(); my /= pts.size();

    double mean_dist = 0;
    for (auto& p : pts)
        mean_dist += std::sqrt((p.x - mx) * (p.x - mx) + (p.y - my) * (p.y - my));
    mean_dist /= pts.size();
    if (mean_dist < 1e-10) mean_dist = 1.0;

    double s = std::sqrt(2.0) / mean_dist;
    out.resize(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        out[i] = Eigen::Vector2d((pts[i].x - mx) * s, (pts[i].y - my) * s);

    Eigen::Matrix3d T = Eigen::Matrix3d::Identity();
    T(0, 0) = s;  T(0, 2) = -s * mx;
    T(1, 1) = s;  T(1, 2) = -s * my;
    return T;
}

// Hartley normalisation for 3D world points.
// Returns the 4×4 normalisation matrix T such that T*[X;Y;Z;1] → normalised.
static Eigen::Matrix4d normalise3D(const std::vector<glm::vec3>& pts,
                                   std::vector<Eigen::Vector4d>& out) {
    double mx = 0, my = 0, mz = 0;
    for (auto& p : pts) { mx += p.x; my += p.y; mz += p.z; }
    mx /= pts.size(); my /= pts.size(); mz /= pts.size();

    double mean_dist = 0;
    for (auto& p : pts) {
        double dx = p.x - mx, dy = p.y - my, dz = p.z - mz;
        mean_dist += std::sqrt(dx*dx + dy*dy + dz*dz);
    }
    mean_dist /= pts.size();
    if (mean_dist < 1e-10) mean_dist = 1.0;

    double s = std::sqrt(3.0) / mean_dist;
    out.resize(pts.size());
    for (size_t i = 0; i < pts.size(); ++i)
        out[i] = Eigen::Vector4d((pts[i].x - mx) * s,
                                 (pts[i].y - my) * s,
                                 (pts[i].z - mz) * s,
                                 1.0);

    Eigen::Matrix4d T = Eigen::Matrix4d::Identity();
    T(0, 0) = s;  T(0, 3) = -s * mx;
    T(1, 1) = s;  T(1, 3) = -s * my;
    T(2, 2) = s;  T(2, 3) = -s * mz;
    return T;
}

// Extract pitch(X), yaw(Y) Euler angles in degrees from a view-matrix rotation R_view.
// R_view is the world-to-camera rotation from glm::lookAt() convention:
//   R_view[2,:] = (sin(yaw)*cos(pitch), -sin(pitch), cos(yaw)*cos(pitch))
// Roll is returned as 0 (V1 assumption).
static glm::vec3 eulerFromViewMatrix(const Eigen::Matrix3d& R_view) {
    // Clamp to avoid asin domain errors from floating-point drift
    double sinPitch = -std::clamp(R_view(1, 2), -1.0, 1.0);
    double pitch    = std::asin(sinPitch);
    double cosPitch = std::cos(pitch);

    double yaw = 0.0;
    if (std::abs(cosPitch) > 1e-6) {
        // R_view(2,0) = sin(yaw)*cos(pitch)
        // R_view(2,2) = cos(yaw)*cos(pitch)
        yaw = std::atan2(R_view(2, 0), R_view(2, 2));
    }
    return glm::vec3(
        static_cast<float>(glm::degrees(pitch)),
        static_cast<float>(glm::degrees(yaw)),
        0.0f);
}

// Compute RMS reprojection error in pixels.
static float computeRmsError(const Eigen::Matrix<double, 3, 4>& P,
                              const std::vector<CorrespondencePair>& pairs,
                              glm::uvec2 outputSize) {
    double sumSq = 0.0;
    for (auto& c : pairs) {
        Eigen::Vector4d X(c.worldPos.x, c.worldPos.y, c.worldPos.z, 1.0);
        Eigen::Vector3d x = P * X;
        if (std::abs(x(2)) < 1e-10) continue;
        double u_proj = x(0) / x(2);  // pixels
        double v_proj = x(1) / x(2);
        double u_meas = c.projectorUV.x * outputSize.x;
        double v_meas = c.projectorUV.y * outputSize.y;
        sumSq += (u_proj - u_meas) * (u_proj - u_meas) +
                 (v_proj - v_meas) * (v_proj - v_meas);
    }
    return static_cast<float>(std::sqrt(sumSq / pairs.size()));
}

// ---------------------------------------------------------------------------
// DLT solver (general 3D, min 6 correspondences)
// ---------------------------------------------------------------------------

static CalibrationResult solveDLT(const std::vector<CorrespondencePair>& pairs,
                                   glm::uvec2 outputSize) {
    CalibrationResult result;
    result.method = SolveMethod::DLT;
    if (pairs.size() < 6) {
        result.errorMessage = "DLT requires at least 6 correspondences";
        return result;
    }

    const int N = static_cast<int>(pairs.size());
    const double W = outputSize.x;
    const double H = outputSize.y;

    // Convert projUV [0,1] → pixels
    std::vector<glm::vec2> pts2D(N);
    for (int i = 0; i < N; ++i)
        pts2D[i] = glm::vec2(pairs[i].projectorUV.x * W, pairs[i].projectorUV.y * H);

    std::vector<glm::vec3> pts3D(N);
    for (int i = 0; i < N; ++i)
        pts3D[i] = pairs[i].worldPos;

    // Hartley normalisation
    std::vector<Eigen::Vector2d> norm2D;
    std::vector<Eigen::Vector4d> norm3D;
    Eigen::Matrix3d T2D = normalise2D(pts2D, norm2D);
    Eigen::Matrix4d T3D = normalise3D(pts3D, norm3D);

    // Build 2N×12 DLT matrix A
    Eigen::MatrixXd A(2 * N, 12);
    for (int i = 0; i < N; ++i) {
        double u = norm2D[i](0), v = norm2D[i](1);
        double X = norm3D[i](0), Y = norm3D[i](1), Z = norm3D[i](2), W1 = norm3D[i](3);

        A.row(2 * i)     << -X, -Y, -Z, -W1,  0,  0,  0,  0,  u*X,  u*Y,  u*Z,  u*W1;
        A.row(2 * i + 1) <<  0,  0,  0,   0, -X, -Y, -Z, -W1,  v*X,  v*Y,  v*Z,  v*W1;
    }

    // Null vector = last column of V (smallest singular value)
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeFullV);
    Eigen::VectorXd h = svd.matrixV().col(11);

    // Reshape to 3×4 projection matrix (normalised space)
    Eigen::Matrix<double, 3, 4> P_norm;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            P_norm(r, c) = h(r * 4 + c);

    // Denormalise: P = T2D_inv * P_norm * T3D
    Eigen::Matrix<double, 3, 4> P = T2D.inverse() * P_norm * T3D;

    // Ensure positive depth (det(P[:3,:3]) > 0)
    if (P.leftCols(3).determinant() < 0.0)
        P = -P;

    // RQ decompose M = P.leftCols(3) → K (intrinsics), R_view (rotation)
    Eigen::Matrix3d K_mat, R_view;
    if (!rqDecompose3x3(P.leftCols(3), K_mat, R_view)) {
        result.errorMessage = "RQ decomposition failed (degenerate point set?)";
        return result;
    }

    // R_view carries the DLT's overall homogeneous scale μ (from rqDecompose3x3's
    // K-normalisation step, which multiplies Q by the same factor it divides K by).
    // Without correction, t_cam = μ·t_true and pos = μ²·camera_pos.
    // Recover μ from any row of R_view (each should have unit norm for true rotation).
    double mu = R_view.row(0).norm();
    if (mu < 1e-10) {
        result.errorMessage = "Degenerate projection matrix (zero R_view norm)";
        return result;
    }
    R_view /= mu;  // now orthogonal

    // World position: -R_view^T * t  where t = K^{-1} * P[:,3]
    Eigen::Vector3d t_cam = K_mat.inverse() * P.col(3) / mu;
    Eigen::Vector3d pos = -R_view.transpose() * t_cam;

    // FOV from K[1,1] (fy): fovY = 2 * atan(H / (2*fy))
    double fy = K_mat(1, 1);
    float fovDeg = static_cast<float>(glm::degrees(2.0 * std::atan(H / (2.0 * fy))));
    fovDeg = std::clamp(fovDeg, 5.0f, 170.0f);

    // Euler angles from R_view
    glm::vec3 euler = eulerFromViewMatrix(R_view);

    // Reprojection error
    float rms = computeRmsError(P, pairs, outputSize);

    result.position      = glm::vec3(pos(0), pos(1), pos(2));
    result.rotationEuler = euler;
    result.fovDegrees    = fovDeg;
    result.rmsErrorPixels = rms;
    result.success       = true;
    return result;
}

// ---------------------------------------------------------------------------
// Homography solver (planar, min 4 correspondences, requires known FOV)
// ---------------------------------------------------------------------------

static CalibrationResult solveHomography(const std::vector<CorrespondencePair>& pairs,
                                          glm::uvec2 outputSize,
                                          float knownFovDegrees) {
    CalibrationResult result;
    result.method = SolveMethod::Homography;
    if (pairs.size() < 4) {
        result.errorMessage = "Homography requires at least 4 correspondences";
        return result;
    }

    const int N = static_cast<int>(pairs.size());
    const double W = outputSize.x;
    const double H = outputSize.y;

    // Build K from known FOV (vertical FOV convention, matching Camera.hpp / glm::perspective)
    double fy = (H / 2.0) / std::tan(glm::radians(knownFovDegrees) / 2.0);
    double fx = fy; // square pixels
    double cx = W / 2.0;
    double cy = H / 2.0;
    Eigen::Matrix3d K;
    K << fx, 0, cx,
          0, fy, cy,
          0,  0,  1;

    // Project 3D points onto their best-fit plane and normalise to 2D
    // For the homography DLT we use (X, Y) as the 2D plane coords,
    // assuming the dominant plane is Z ≈ const (or we use the full XYZ projected onto the plane).
    // Simple approach: use X and Y directly (works when the plane is mostly XZ-parallel or XY-parallel).
    // For robustness, project onto the best-fit plane via SVD.
    // Compute centroid and best-fit plane normal
    Eigen::MatrixXd M3(pairs.size(), 3);
    for (size_t i = 0; i < pairs.size(); ++i)
        M3.row(i) = Eigen::Vector3d(pairs[i].worldPos.x, pairs[i].worldPos.y, pairs[i].worldPos.z);
    Eigen::Vector3d centroid = M3.colwise().mean();
    M3.rowwise() -= centroid.transpose();

    Eigen::JacobiSVD<Eigen::MatrixXd> planeSvd(M3, Eigen::ComputeFullV);
    // The two dominant singular vectors span the plane; the 3rd is the normal.
    Eigen::Vector3d u = planeSvd.matrixV().col(0);
    Eigen::Vector3d v = planeSvd.matrixV().col(1);

    // 2D plane coordinates
    std::vector<glm::vec2> src2D(pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i) {
        Eigen::Vector3d p(pairs[i].worldPos.x - centroid(0),
                          pairs[i].worldPos.y - centroid(1),
                          pairs[i].worldPos.z - centroid(2));
        src2D[i] = glm::vec2(static_cast<float>(u.dot(p)),
                             static_cast<float>(v.dot(p)));
    }

    // Destination: pixel coords
    std::vector<glm::vec2> dst2D(pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i)
        dst2D[i] = glm::vec2(pairs[i].projectorUV.x * W, pairs[i].projectorUV.y * H);

    // Hartley normalise both sets
    std::vector<Eigen::Vector2d> normSrc, normDst;
    Eigen::Matrix3d Ts = normalise2D(src2D, normSrc);
    Eigen::Matrix3d Td = normalise2D(dst2D, normDst);

    // Build 2N×9 DLT matrix
    Eigen::MatrixXd A(2 * N, 9);
    for (int i = 0; i < N; ++i) {
        double x = normSrc[i](0), y = normSrc[i](1);
        double u2 = normDst[i](0), v2 = normDst[i](1);

        A.row(2 * i)     << -x, -y, -1,  0,  0,  0,  u2*x,  u2*y,  u2;
        A.row(2 * i + 1) <<  0,  0,  0, -x, -y, -1,  v2*x,  v2*y,  v2;
    }

    Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeFullV);
    Eigen::VectorXd h = svd.matrixV().col(8);
    Eigen::Matrix3d H_norm;
    H_norm << h(0), h(1), h(2),
              h(3), h(4), h(5),
              h(6), h(7), h(8);

    // Denormalise: H = Td_inv * H_norm * Ts
    Eigen::Matrix3d H_mat = Td.inverse() * H_norm * Ts;

    // Decompose H = K * [r1, r2, t_plane]
    Eigen::Matrix3d M_dec = K.inverse() * H_mat;
    double lambda = (M_dec.col(0).norm() + M_dec.col(1).norm()) / 2.0;
    if (std::abs(lambda) < 1e-10) {
        result.errorMessage = "Homography decomposition failed (degenerate)";
        return result;
    }

    Eigen::Vector3d r1 = M_dec.col(0) / lambda;
    Eigen::Vector3d r2 = M_dec.col(1) / lambda;
    Eigen::Vector3d r3 = r1.cross(r2);
    Eigen::Vector3d t_plane = M_dec.col(2) / lambda;

    // Orthonormalise via SVD of [r1, r2, r3]
    Eigen::Matrix3d R_raw;
    R_raw.col(0) = r1; R_raw.col(1) = r2; R_raw.col(2) = r3;
    Eigen::JacobiSVD<Eigen::Matrix3d> rSvd(R_raw, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d R_view = rSvd.matrixV() * rSvd.matrixU().transpose();
    if (R_view.determinant() < 0) R_view = -R_view;

    // t_plane is in terms of the plane's (u, v) basis centred at centroid.
    // Recover world-space t:
    //   x_cam = R_raw * [u_plane; v_plane; 0] + t_plane
    // but R_raw's columns are in world space r1, r2, r3.
    // The camera position in world space: -R_view^T * t_cam
    // t_cam ≈ t_plane (the scale/lambda makes this the 3D camera translation)
    Eigen::Vector3d pos = -R_view.transpose() * t_plane;
    // Translate back from plane centroid
    pos += centroid;

    glm::vec3 euler = eulerFromViewMatrix(R_view);

    // Reprojection error: project 3D points through K * [R_view | t_cam]
    Eigen::Vector3d t_cam = -R_view * (pos - centroid);
    Eigen::Matrix<double, 3, 4> P;
    P.leftCols(3) = K * R_view;
    P.col(3) = K * t_cam;

    // Full world projection (without centroid subtraction since t_cam already accounts for it)
    // Recompute with actual world coords
    Eigen::Vector3d t_world = -R_view * Eigen::Vector3d(pos(0), pos(1), pos(2));
    Eigen::Matrix<double, 3, 4> P_world;
    P_world.leftCols(3) = K * R_view;
    P_world.col(3) = K * t_world;

    float rms = computeRmsError(P_world, pairs, outputSize);

    result.position       = glm::vec3(static_cast<float>(pos(0)),
                                      static_cast<float>(pos(1)),
                                      static_cast<float>(pos(2)));
    result.rotationEuler  = euler;
    result.fovDegrees     = knownFovDegrees;
    result.rmsErrorPixels = rms;
    result.success        = true;
    return result;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool CalibrationSolver::isCoplanar(const std::vector<CorrespondencePair>& pairs,
                                    float tolerance) {
    if (pairs.size() < 4) return true;

    Eigen::MatrixXd M(3, pairs.size());
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    for (size_t i = 0; i < pairs.size(); ++i)
        centroid += Eigen::Vector3d(pairs[i].worldPos.x, pairs[i].worldPos.y, pairs[i].worldPos.z);
    centroid /= static_cast<double>(pairs.size());

    for (size_t i = 0; i < pairs.size(); ++i) {
        M(0, i) = pairs[i].worldPos.x - centroid(0);
        M(1, i) = pairs[i].worldPos.y - centroid(1);
        M(2, i) = pairs[i].worldPos.z - centroid(2);
    }

    Eigen::JacobiSVD<Eigen::MatrixXd> svd(M);
    auto sv = svd.singularValues();
    if (sv(0) < 1e-10) return true;
    return sv(2) < tolerance * sv(0);
}

CalibrationResult CalibrationSolver::solve(const std::vector<CorrespondencePair>& pairs,
                                            glm::uvec2 outputSize,
                                            float knownFovDegrees) {
    CalibrationResult fail;

    if (pairs.size() < 4) {
        fail.errorMessage = "Need at least 4 correspondence points";
        return fail;
    }

    bool coplanar = isCoplanar(pairs);

    // Use homography when: points are coplanar AND caller provided a known FOV AND >= 4 pts
    if (coplanar && knownFovDegrees > 0.0f && pairs.size() >= 4) {
        auto res = solveHomography(pairs, outputSize, knownFovDegrees);
        if (res.success) return res;
        // Fall through to DLT if homography fails
    }

    if (pairs.size() < 6) {
        fail.errorMessage = coplanar
            ? "Coplanar surface with no known FOV: need 6+ non-coplanar points, "
              "or set the projector FOV and use 4+ coplanar points"
            : "3D surface requires at least 6 correspondence points";
        return fail;
    }

    return solveDLT(pairs, outputSize);
}

// ---------------------------------------------------------------------------
// LM-based refinement (V2 — direct nonlinear optimization of reprojection error)
// ---------------------------------------------------------------------------

// Functor for Eigen::LevenbergMarquardt. Variables: 7
// (px, py, pz, pitchDeg, yawDeg, rollDeg, fovDeg). Residuals: 2 per
// correspondence (du_pixels, dv_pixels). Uses the same projection math as
// Stage3DRenderer::buildProjectorCamera so the optimizer is consistent with
// the rendering convention.
struct ProjectorRefineFunctor {
    using Scalar = double;
    using InputType = Eigen::VectorXd;
    using ValueType = Eigen::VectorXd;
    using JacobianType = Eigen::MatrixXd;
    enum {
        InputsAtCompileTime = Eigen::Dynamic,
        ValuesAtCompileTime = Eigen::Dynamic
    };

    std::vector<CorrespondencePair> pairs;
    glm::uvec2 outputSize{1920, 1080};
    float aspect{16.0f / 9.0f};
    float nearClip{0.1f};
    float farClip{100.0f};

    int inputs() const { return 9; }  // pos(3) + euler(3) + fov(1) + k1, k2(2)
    int values() const { return 2 * static_cast<int>(pairs.size()); }

    int operator()(const Eigen::VectorXd& x, Eigen::VectorXd& fvec) const {
        glm::vec3 pos(static_cast<float>(x(0)), static_cast<float>(x(1)), static_cast<float>(x(2)));
        float pitchRad = glm::radians(static_cast<float>(x(3)));
        float yawRad   = glm::radians(static_cast<float>(x(4)));
        float rollRad  = glm::radians(static_cast<float>(x(5)));
        float fov      = std::clamp(static_cast<float>(x(6)), 5.0f, 170.0f);
        double k1      = x(7);
        double k2      = x(8);

        // Forward direction matches Stage3DRenderer::buildProjectorCamera.
        glm::vec3 fwd(
            -std::sin(yawRad) * std::cos(pitchRad),
             std::sin(pitchRad),
            -std::cos(yawRad) * std::cos(pitchRad)
        );
        glm::vec3 up(0.0f, 1.0f, 0.0f);
        if (std::abs(rollRad) > 1e-5f) {
            float c = std::cos(rollRad), s = std::sin(rollRad);
            glm::vec3 right = glm::normalize(glm::cross(fwd, up));
            up = glm::normalize(c * up + s * right);
        }
        glm::mat4 view = glm::lookAt(pos, pos + fwd, up);
        glm::mat4 proj = glm::perspective(glm::radians(fov), aspect, nearClip, farClip);
        glm::mat4 vp   = proj * view;

        for (size_t i = 0; i < pairs.size(); ++i) {
            glm::vec4 clip = vp * glm::vec4(pairs[i].worldPos, 1.0f);
            if (clip.w < 1e-5f) {
                fvec(2 * i)     = 1.0e4;
                fvec(2 * i + 1) = 1.0e4;
                continue;
            }
            double ndc_x = clip.x / clip.w;
            double ndc_y = clip.y / clip.w;

            // Radial lens distortion (Brown-Conrady, 2-term):
            //   x_d = x · (1 + k1·r² + k2·r⁴),   r² = x² + y²  in normalized coords
            // The physical projector lens applies this; we model it here so the
            // solver finds the *ideal pinhole* pose that, after lens distortion,
            // maps world points to the user-measured UV positions.
            double r2 = ndc_x * ndc_x + ndc_y * ndc_y;
            double scale = 1.0 + k1 * r2 + k2 * r2 * r2;
            double dist_x = ndc_x * scale;
            double dist_y = ndc_y * scale;

            double u_pred = (dist_x + 1.0) * 0.5;
            double v_pred = (1.0 - dist_y) * 0.5;
            fvec(2 * i)     = (u_pred - pairs[i].projectorUV.x) * outputSize.x;
            fvec(2 * i + 1) = (v_pred - pairs[i].projectorUV.y) * outputSize.y;
        }
        return 0;
    }
};

// Single-pass LM refinement from one initial guess. 9-DOF: 6-DOF pose +
// FOV + 2 radial-distortion coefficients (k1, k2).
static CalibrationResult runLMOnce(const std::vector<CorrespondencePair>& pairs,
                                    glm::uvec2 outputSize,
                                    const glm::vec3& initialPos,
                                    const glm::vec3& initialRotEuler,
                                    float initialFovDegrees,
                                    float initialK1 = 0.0f,
                                    float initialK2 = 0.0f)
{
    CalibrationResult result;
    result.method = SolveMethod::DLT;

    ProjectorRefineFunctor functor;
    functor.pairs      = pairs;
    functor.outputSize = outputSize;
    functor.aspect     = outputSize.y > 0
                         ? static_cast<float>(outputSize.x) / static_cast<float>(outputSize.y)
                         : 16.0f / 9.0f;

    Eigen::NumericalDiff<ProjectorRefineFunctor> numDiff(functor, 1e-8);
    Eigen::LevenbergMarquardt<Eigen::NumericalDiff<ProjectorRefineFunctor>> lm(numDiff);
    lm.parameters.maxfev = 2000;
    lm.parameters.xtol   = 1e-10;
    lm.parameters.ftol   = 1e-10;

    Eigen::VectorXd x(9);
    x << initialPos.x, initialPos.y, initialPos.z,
         initialRotEuler.x, initialRotEuler.y, initialRotEuler.z,
         std::clamp(initialFovDegrees, 5.0f, 170.0f),
         initialK1, initialK2;

    int info = lm.minimize(x);
    if (info <= 0) {
        result.errorMessage = "LM optimization failed (info=" + std::to_string(info) + ")";
        return result;
    }

    result.position      = glm::vec3(static_cast<float>(x(0)),
                                     static_cast<float>(x(1)),
                                     static_cast<float>(x(2)));
    result.rotationEuler = glm::vec3(static_cast<float>(x(3)),
                                     static_cast<float>(x(4)),
                                     static_cast<float>(x(5)));
    result.fovDegrees    = std::clamp(static_cast<float>(x(6)), 5.0f, 170.0f);
    result.distortionK1  = static_cast<float>(x(7));
    result.distortionK2  = static_cast<float>(x(8));

    Eigen::VectorXd residuals(2 * pairs.size());
    functor(x, residuals);
    double sumSq = residuals.squaredNorm();
    result.rmsErrorPixels = static_cast<float>(std::sqrt(sumSq / pairs.size()));
    result.success        = true;

    if (!std::isfinite(result.position.x) || !std::isfinite(result.position.y) ||
        !std::isfinite(result.position.z) ||
        !std::isfinite(result.rotationEuler.x) || !std::isfinite(result.rotationEuler.y) ||
        !std::isfinite(result.rotationEuler.z) ||
        !std::isfinite(result.fovDegrees) || !std::isfinite(result.rmsErrorPixels) ||
        !std::isfinite(result.distortionK1) || !std::isfinite(result.distortionK2)) {
        result.success = false;
        result.errorMessage = "LM produced non-finite values";
    }
    return result;
}

// Compute centroid, plane normal, and extent from the correspondence points.
// Used to build heuristic initial poses for multi-start optimization.
static void computePointCloudStats(const std::vector<CorrespondencePair>& pairs,
                                    glm::vec3& outCentroid,
                                    glm::vec3& outPlaneNormal,
                                    float& outExtent)
{
    Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
    for (const auto& p : pairs) centroid += Eigen::Vector3d(p.worldPos.x, p.worldPos.y, p.worldPos.z);
    centroid /= static_cast<double>(pairs.size());

    Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
    double extent = 0.0;
    for (const auto& p : pairs) {
        Eigen::Vector3d d(p.worldPos.x - centroid.x(),
                          p.worldPos.y - centroid.y(),
                          p.worldPos.z - centroid.z());
        cov += d * d.transpose();
        extent = std::max(extent, d.norm());
    }
    if (extent < 0.01) extent = 0.5;

    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(cov);
    Eigen::Vector3d normal_eig = es.eigenvectors().col(0); // smallest eigenvalue

    outCentroid    = glm::vec3(static_cast<float>(centroid.x()),
                               static_cast<float>(centroid.y()),
                               static_cast<float>(centroid.z()));
    outPlaneNormal = glm::vec3(static_cast<float>(normal_eig.x()),
                               static_cast<float>(normal_eig.y()),
                               static_cast<float>(normal_eig.z()));
    outExtent      = static_cast<float>(extent);
}

// Build a (position, rotation, FOV) seed by placing the camera at
// centroid + distance × `normalDir`, oriented to look at the centroid.
static std::tuple<glm::vec3, glm::vec3, float>
makeLookAtSeed(const glm::vec3& centroid, const glm::vec3& normalDir, float extent, float fov)
{
    float halfFovTan = std::tan(glm::radians(fov) * 0.5f);
    float distance = (halfFovTan > 1e-3f) ? extent / halfFovTan * 1.5f : extent * 4.0f;

    glm::vec3 pos = centroid + distance * normalDir;
    glm::vec3 fwd = glm::normalize(centroid - pos);
    float pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
    float yaw   = std::atan2(-fwd.x, -fwd.z);
    return {pos, glm::vec3(glm::degrees(pitch), glm::degrees(yaw), 0.0f), fov};
}

CalibrationResult CalibrationSolver::solveRefine(
    const std::vector<CorrespondencePair>& pairs,
    glm::uvec2 outputSize,
    const glm::vec3& initialPos,
    const glm::vec3& initialRotEuler,
    float initialFovDegrees)
{
    CalibrationResult result;
    result.method = SolveMethod::DLT;

    if (pairs.size() < 4) {
        result.errorMessage = "Refine requires at least 4 correspondence points";
        return result;
    }

    // ===========================================================================
    // Aggressive multi-start: try ~12 seeds covering the parameter space, then
    // iteratively re-LM the best result. This escapes the local minima that
    // make single-start LM produce poor calibrations.
    // ===========================================================================

    glm::vec3 centroid, planeNormal;
    float     extent;
    computePointCloudStats(pairs, centroid, planeNormal, extent);

    // Sign-disambiguate plane normal: same side as user's initial pose.
    glm::vec3 normalUser = (glm::dot(planeNormal, initialPos - centroid) < 0.0f)
                          ? -planeNormal : planeNormal;

    using Seed = std::tuple<glm::vec3, glm::vec3, float>;
    std::vector<Seed> seeds;
    seeds.reserve(16);

    // 1. User's pose (refinement-friendly when calibration is iterative).
    seeds.emplace_back(initialPos, initialRotEuler, initialFovDegrees);

    // 2. Heuristic — same side as user.
    seeds.push_back(makeLookAtSeed(centroid, normalUser, extent, initialFovDegrees));
    // 3. Heuristic — opposite side (in case user's initial pose is on the wrong side).
    seeds.push_back(makeLookAtSeed(centroid, -normalUser, extent, initialFovDegrees));
    // 4. Heuristic with default FOV in case user's FOV is way off.
    seeds.push_back(makeLookAtSeed(centroid, normalUser, extent, 50.0f));

    // 5. Editor-default pose (catches "user hasn't set up anything yet").
    seeds.emplace_back(glm::vec3(0.0f, 3.0f, 5.0f), glm::vec3(-20.0f, 0.0f, 0.0f), 50.0f);

    // 6-10. Heuristic seeds with sweeping FOV values — projectors have a wide
    // range of throw ratios (zoom lenses, ultra-short-throw, etc.). Without
    // this, LM can get stuck at a wrong-FOV local minimum.
    for (float fov : {25.0f, 35.0f, 70.0f, 90.0f, 110.0f}) {
        seeds.push_back(makeLookAtSeed(centroid, normalUser, extent, fov));
    }

    // 11-30. Random Gaussian perturbations of the user's pose to escape
    // narrow local-minimum valleys. NOTE: roll is perturbed with a much
    // smaller std than pitch/yaw — for symmetric mesh layouts there's a
    // near-equivalent local minimum at roll±180°, and we want to bias
    // strongly toward the user's initial orientation.
    std::mt19937 rng(0xCA1B);
    std::normal_distribution<float> posJ(0.0f, 1.5f);
    std::normal_distribution<float> rotJ(0.0f, 25.0f);
    std::normal_distribution<float> rollJ(0.0f, 3.0f);
    std::normal_distribution<float> fovJ(0.0f, 18.0f);
    for (int i = 0; i < 20; ++i) {
        glm::vec3 p(initialPos.x + posJ(rng), initialPos.y + posJ(rng), initialPos.z + posJ(rng));
        glm::vec3 r(initialRotEuler.x + rotJ(rng),
                    initialRotEuler.y + rotJ(rng),
                    initialRotEuler.z + rollJ(rng));
        float f = std::clamp(initialFovDegrees + fovJ(rng), 10.0f, 150.0f);
        seeds.emplace_back(p, r, f);
    }

    // Run LM from each seed; collect ALL successful results so we can apply
    // a roll-sanity filter before picking the winner. All deterministic seeds
    // start with k1 = k2 = 0 (no distortion) — the LM will discover them.
    // A handful of random k1/k2 seeds help escape if LM stalls at zero.
    std::vector<CalibrationResult> seedResults;
    seedResults.reserve(seeds.size() + 4);

    for (const auto& [pos, rot, fov] : seeds) {
        auto r = runLMOnce(pairs, outputSize, pos, rot, fov, 0.0f, 0.0f);
        if (r.success) seedResults.push_back(r);
    }

    // 4 extra seeds with non-zero distortion priors so LM has somewhere to
    // start if the zero-distortion optima are local minima for a real lens.
    std::normal_distribution<float> k1J(0.0f, 0.08f);
    std::normal_distribution<float> k2J(0.0f, 0.03f);
    for (int i = 0; i < 4; ++i) {
        auto r = runLMOnce(pairs, outputSize, initialPos, initialRotEuler, initialFovDegrees,
                            k1J(rng), k2J(rng));
        if (r.success) seedResults.push_back(r);
    }

    if (seedResults.empty()) {
        CalibrationResult fail;
        fail.method = SolveMethod::DLT;
        fail.errorMessage = "All LM seeds failed to converge";
        return fail;
    }

    // Sort by RMS ascending (best fit first).
    std::sort(seedResults.begin(), seedResults.end(),
              [](const CalibrationResult& a, const CalibrationResult& b) {
                  return a.rmsErrorPixels < b.rmsErrorPixels;
              });

    // Roll sanity filter: pick the lowest-RMS result whose roll is within
    // ±90° of the user's initial roll. This rejects the "180° flipped"
    // local minimum that's mathematically valid for symmetric meshes but
    // visually wrong (mesh rendered upside down). Falls back to absolute-best
    // if no result has acceptable roll (e.g., user's initial roll is wrong).
    auto rollDelta = [&](float candRoll) {
        float d = std::fmod(candRoll - initialRotEuler.z + 540.0f, 360.0f) - 180.0f;
        return std::abs(d);
    };
    CalibrationResult best = seedResults.front();
    for (const auto& r : seedResults) {
        if (rollDelta(r.rotationEuler.z) <= 90.0f) {
            best = r;
            break;
        }
    }

    // Iterative refinement: re-LM from the best result. LM can sometimes do
    // better when restarted with reset internal state.
    for (int iter = 0; iter < 20; ++iter) {
        auto refined = runLMOnce(pairs, outputSize,
                                  best.position, best.rotationEuler, best.fovDegrees,
                                  best.distortionK1, best.distortionK2);
        if (refined.success && refined.rmsErrorPixels < best.rmsErrorPixels - 0.001f) {
            best = refined;
        } else {
            break; // no meaningful improvement
        }
    }

    // Outlier rejection: if any correspondence has a residual much larger than
    // the median, the user probably mis-clicked it — that one bad point pulls
    // the entire solution toward itself, hurting accuracy for the others. Drop
    // worst outliers iteratively as long as RMS keeps improving.
    auto computePerPointResiduals = [&](const std::vector<CorrespondencePair>& pp,
                                         const CalibrationResult& res) -> std::vector<float> {
        ProjectorRefineFunctor f;
        f.pairs      = pp;
        f.outputSize = outputSize;
        f.aspect     = outputSize.y > 0
                       ? static_cast<float>(outputSize.x) / static_cast<float>(outputSize.y)
                       : 16.0f / 9.0f;
        Eigen::VectorXd x(9), residuals(2 * pp.size());
        x << res.position.x, res.position.y, res.position.z,
             res.rotationEuler.x, res.rotationEuler.y, res.rotationEuler.z, res.fovDegrees,
             res.distortionK1, res.distortionK2;
        f(x, residuals);
        std::vector<float> perPoint(pp.size(), 0.0f);
        for (size_t i = 0; i < pp.size(); ++i) {
            float du = static_cast<float>(residuals(2 * i));
            float dv = static_cast<float>(residuals(2 * i + 1));
            perPoint[i] = std::sqrt(du * du + dv * dv);
        }
        return perPoint;
    };

    std::vector<CorrespondencePair> kept = pairs;
    for (int trim = 0; trim < 3; ++trim) {
        if (kept.size() < 5) break; // need at least 4 to solve, keep 1 buffer
        auto perPoint = computePerPointResiduals(kept, best);
        std::vector<float> sorted = perPoint;
        std::sort(sorted.begin(), sorted.end());
        float median = sorted[sorted.size() / 2];
        float threshold = std::max(3.0f * median, 8.0f);

        size_t worstIdx = 0;
        float  worstResidual = -1.0f;
        for (size_t i = 0; i < perPoint.size(); ++i) {
            if (perPoint[i] > worstResidual) {
                worstResidual = perPoint[i];
                worstIdx = i;
            }
        }
        if (worstResidual <= threshold) break; // no clear outlier

        // Try without that one point
        std::vector<CorrespondencePair> trimmed = kept;
        trimmed.erase(trimmed.begin() + worstIdx);

        auto trimmedResult = runLMOnce(trimmed, outputSize,
                                        best.position, best.rotationEuler, best.fovDegrees,
                                        best.distortionK1, best.distortionK2);
        for (int it = 0; it < 10; ++it) {
            auto r = runLMOnce(trimmed, outputSize,
                                trimmedResult.position, trimmedResult.rotationEuler,
                                trimmedResult.fovDegrees,
                                trimmedResult.distortionK1, trimmedResult.distortionK2);
            if (r.success && r.rmsErrorPixels < trimmedResult.rmsErrorPixels - 0.001f)
                trimmedResult = r;
            else break;
        }

        if (trimmedResult.success && trimmedResult.rmsErrorPixels < best.rmsErrorPixels) {
            best = trimmedResult;
            kept = trimmed;
        } else {
            break; // dropping that point didn't help
        }
    }

    return best;
}

} // namespace entity
