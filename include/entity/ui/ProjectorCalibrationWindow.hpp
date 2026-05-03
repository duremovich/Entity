#pragma once

#include "../render/Stage3DRenderer.hpp"
#include "../calibration/CalibrationSolver.hpp"
#include "../components/Camera.hpp"
#include <imgui.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <vector>
#include <string>

namespace entity {

class Engine;
struct MeshData;

/**
 * ProjectorCalibrationWindow - QuickCal-style projector alignment tool.
 *
 * The user picks 3D mesh points in the left pane (orbitable scene view)
 * and positions crosshair markers in the right pane (projector output
 * preview). After collecting 4–6+ correspondences a DLT/homography solver
 * recovers the projector's exact position, orientation, and FOV and writes
 * them back to the Projector component.
 *
 * The right pane also routes calibration crosshairs to the physical
 * projector output (via the D3D12 calibration overlay API) so the user
 * can see the markers on the real surface.
 */
class ProjectorCalibrationWindow {
public:
    explicit ProjectorCalibrationWindow(Engine* engine);
    ~ProjectorCalibrationWindow();

    void open(entt::entity projectorEntity);
    void close();
    bool isOpen() const { return m_isOpen; }

    // Call every frame while open.
    void render();

private:
    // -----------------------------------------------------------------------
    // Per-frame rendering
    // -----------------------------------------------------------------------
    void renderToolbar();
    void renderScenePane(ImVec2 pos, ImVec2 size);
    void renderProjectorPane(ImVec2 pos, ImVec2 size);
    void renderPointsTable();
    void renderSolveBar();

    // -----------------------------------------------------------------------
    // Interaction helpers
    // -----------------------------------------------------------------------
    // Try to pick a 3D mesh point at the given screen coordinate in the
    // scene pane. Returns true and fills outWorldPos on hit.
    bool pickMeshPoint(ImVec2 mousePos, ImVec2 panePos, ImVec2 paneSize,
                       entt::entity screenEntity, glm::vec3& outWorldPos);

    // Project a 3D world position through the current projector camera.
    // Returns [0,1] UV coords in projector output space.
    glm::vec2 projectThroughProjector(const glm::vec3& worldPos);

    // Rebuild m_projectorCamera from the Projector component's current params.
    void rebuildProjectorCamera(ImVec2 paneSize);

    // Push / pull crosshair positions to D3D12 calibration overlay.
    void syncOverlayPoints();

    // Write current m_points back to the Projector component so they
    // survive close/reopen. Cheap (~10 points × 24 bytes typical).
    void persistPoints();

    // Start routing the calibration overlay to the given OutputDisplay entity.
    void routeToOutput(entt::entity outputEntity);
    void unrouteOutput();

    // Activate a point for adjustment in the right pane.
    void activatePoint(int idx);

    // Confirm current pending 3D point as a new correspondence.
    void addCorrespondencePoint();

    // Apply the last successful solve to the Projector component.
    void applyResult();

    // -----------------------------------------------------------------------
    // Internal state
    // -----------------------------------------------------------------------
    struct CorrespondencePointEx : CorrespondencePair {
        bool isAligned{false}; // user has positioned the crosshair
    };

    Engine*           m_engine{nullptr};
    bool              m_isOpen{false};
    entt::entity      m_projectorEntity{entt::null};
    entt::entity      m_screenEntity{entt::null};   // target screen to calibrate against
    entt::entity      m_routedOutputEntity{entt::null};
    entt::entity      m_savedSourceScreen{entt::null}; // restored on exit

    // 3D scene renderer (left pane, interactive camera)
    Stage3DRenderer   m_renderer;
    Camera            m_sceneCamera;    // left pane camera (orbitable)
    Camera            m_projectorCamera;// right pane camera (built from Projector params)

    // Correspondence points
    std::vector<CorrespondencePointEx> m_points;
    int               m_activePointIdx{-1};  // index of crosshair being positioned
    bool              m_hasPending{false};   // a 3D point has been picked but not confirmed
    glm::vec3         m_pendingWorldPos{};

    // Right pane drag state
    bool              m_rightPaneDragging{false};
    ImVec2            m_rightPaneDragStart{};
    glm::vec2         m_rightPaneDragStartUV{};

    // Solve results
    CalibrationResult m_lastResult{};
    bool              m_hasSolution{false};

    // Cached projector output resolution (from linked OutputDisplay or fallback)
    glm::uvec2        m_outputSize{1920, 1080};

    // Display options
    bool              m_showWireframe{true};  // overlay mesh edges in scene pane
};

} // namespace entity
