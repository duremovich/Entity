#pragma once

#include "../components/Camera.hpp"
#include "../media/ObjLoader.hpp"
#include <imgui.h>
#include <glm/glm.hpp>
#include <vector>

namespace entity {

/**
 * Stage3DRenderer - Renders a 3D stage environment using ImGui's DrawList.
 *
 * Provides a simple 3D visualization of the stage with:
 * - Floor grid with major/minor lines
 * - Screen quad displaying the composited output
 * - Camera controls (orbit, pan, zoom)
 *
 * Uses software projection via glm matrices and draws with ImGui primitives.
 * This approach is simple and portable, suitable for an editor preview.
 */
class Stage3DRenderer {
public:
    Stage3DRenderer();
    ~Stage3DRenderer() = default;

    /**
     * Render the 3D stage to the given ImGui draw list (single screen version).
     * @param drawList The ImGui draw list to render to
     * @param screenPos Top-left corner of the render area
     * @param screenSize Size of the render area
     * @param composeTextureID GPU texture ID for the composited output (can be nullptr)
     */
    void render(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize,
                ImTextureID composeTextureID = 0);

    /**
     * Begin rendering the 3D stage (draws background, grid, axes).
     * Call this once, then call drawScreen() for each screen.
     */
    void beginRender(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize);

    /**
     * End rendering the 3D stage (pops clip rect, draws overlays).
     */
    void endRender(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize);

    /**
     * Draw a single screen with the given transform.
     * Call between beginRender() and endRender().
     * @param position World position (x, y, z)
     * @param rotation Rotation in degrees (pitch, yaw, roll)
     * @param scale Scale factors (x, y, z)
     * @param textureID Texture to display on the screen
     * @param isSelected Whether this screen is selected (draws highlight)
     * @param mesh If non-null, renders the mesh geometry instead of a flat quad.
     *             Uses the mesh's OBJ UV coordinates; V is flipped (OBJ V=0 at bottom).
     */
    void drawScreen(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize,
                    const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale,
                    ImTextureID textureID, bool isSelected = false,
                    const MeshData* mesh = nullptr);

    /**
     * Draw a projector frustum gizmo.
     * Call between beginRender() and endRender().
     */
    void drawProjector(ImDrawList* drawList, ImVec2 renderPos, ImVec2 renderSize,
                       const glm::vec3& position, const glm::vec3& rotation,
                       float fovDegrees, bool enabled, bool isSelected,
                       const char* label = nullptr);

    /**
     * Per-screen transform input for content-aware framing.
     * Mirrors the (position, rotation, scale) data drawScreen() consumes
     * so frameScreens() can compute an AABB that exactly matches what's
     * drawn.
     */
    struct ScreenFrameInput {
        glm::vec3 position;
        glm::vec3 rotation;  // Euler degrees (yaw=y, pitch=x, roll=z)
        glm::vec3 scale;
        const MeshData* mesh{nullptr};  // if non-null, use mesh bounds for AABB
    };

    /**
     * Reposition the camera to frame all given screens in view.
     * Computes a world-space AABB of every screen quad's 4 corners,
     * sets orbitTarget to its center, and chooses orbitDistance so the
     * bounding sphere fits inside both the vertical and horizontal FOV
     * with a small margin. Preserves orbitYaw/orbitPitch.
     * If `screens` is empty, falls back to the default Reset position.
     */
    void frameScreens(const std::vector<ScreenFrameInput>& screens);

    /**
     * Handle mouse input for camera control.
     * @param mousePos Current mouse position
     * @param screenPos Top-left corner of the render area
     * @param screenSize Size of the render area
     * @param leftDown Left mouse button is down
     * @param rightDown Right mouse button is down
     * @param middleDown Middle mouse button is down
     * @param shiftDown Shift key is down
     * @param scrollDelta Mouse wheel scroll delta
     */
    void handleInput(ImVec2 mousePos, ImVec2 screenPos, ImVec2 screenSize,
                     bool leftDown, bool rightDown, bool middleDown,
                     bool shiftDown, float scrollDelta);

    /**
     * Test whether a 2D mouse position falls within a projected screen.
     * Uses the same transform logic as drawScreen(); call after beginRender().
     * @param mesh If non-null, tests against the projected mesh triangles.
     */
    bool hitTestScreen(ImVec2 mousePos, ImVec2 renderPos, ImVec2 renderSize,
                       const glm::vec3& position, const glm::vec3& rotation,
                       const glm::vec3& scale,
                       const MeshData* mesh = nullptr) const;

    /**
     * Get the camera for external manipulation.
     */
    Camera& getCamera() { return m_camera; }
    const Camera& getCamera() const { return m_camera; }

    /**
     * Override the internal camera for one or more draw calls, then restore.
     * Used by ProjectorCalibrationWindow to render from the projector's viewpoint.
     */
    void setCamera(const Camera& cam) { m_camera = cam; }

    /**
     * Build a Camera from projector parameters (position, Euler degrees, fov, aspect).
     * Convention: rotation[0]=pitch(X), rotation[1]=yaw(Y), rotation[2]=roll(Z).
     * The resulting camera can be passed to setCamera() to render from the projector's POV.
     */
    static Camera buildProjectorCamera(const glm::vec3& position,
                                       const glm::vec3& eulerDeg,
                                       float fovDeg,
                                       float aspect,
                                       float nearClip = 0.1f,
                                       float farClip = 50.0f);

    // Grid settings
    float gridSize{20.0f};          // Total grid size (e.g., 20x20 units)
    float gridSpacing{1.0f};        // Distance between major grid lines
    int gridSubdivisions{4};        // Minor divisions per major cell
    ImU32 gridMajorColor{IM_COL32(80, 80, 80, 255)};
    ImU32 gridMinorColor{IM_COL32(50, 50, 50, 255)};
    ImU32 gridAxisXColor{IM_COL32(180, 60, 60, 255)};   // Red for X
    ImU32 gridAxisZColor{IM_COL32(60, 60, 180, 255)};   // Blue for Z

    // Screen settings (base size, transform is applied on top)
    float screenWidth{16.0f / 9.0f};   // Screen width in world units (16:9 aspect at height 1)
    float screenHeight{1.0f};           // Screen height in world units
    float screenElevation{0.5f};        // Height of screen center above floor (default)

    // Screen transform (set from Screen component)
    glm::vec3 screenPosition{0.0f, 0.0f, 0.0f};   // Position offset (x, y, z)
    glm::vec3 screenRotation{0.0f, 0.0f, 0.0f};   // Rotation in degrees (pitch, yaw, roll)
    glm::vec3 screenScale{1.0f, 1.0f, 1.0f};      // Scale factors

    // Optional radial lens distortion applied in projectPoint after the
    // perspective divide. 0 = ideal pinhole (default for the editor scene
    // view). Set non-zero by ProjectorCalibrationWindow's right pane to
    // simulate the physical projector lens, so the rendered preview matches
    // what the user sees through the real projector.
    float lensK1{0.0f};
    float lensK2{0.0f};

private:
    /**
     * Project a 3D world point to 2D screen coordinates.
     * @param worldPos Position in world space
     * @param screenPos Top-left corner of render area
     * @param screenSize Size of render area
     * @return Screen coordinates, or (-1,-1) if behind camera
     */
    ImVec2 projectPoint(const glm::vec3& worldPos, ImVec2 screenPos, ImVec2 screenSize) const;

    /**
     * Draw a line in 3D space.
     */
    void drawLine3D(ImDrawList* drawList, const glm::vec3& p1, const glm::vec3& p2,
                    ImVec2 screenPos, ImVec2 screenSize, ImU32 color, float thickness = 1.0f);

    /**
     * Draw the floor grid.
     */
    void drawFloorGrid(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize);

    /**
     * Draw the screen quad.
     */
    void drawScreenQuad(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize,
                        ImTextureID textureID);

    /**
     * Draw coordinate axes at origin.
     */
    void drawAxes(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize);

private:
    Camera m_camera;

    // Interaction state
    bool m_isDragging{false};
    ImVec2 m_lastMousePos{0, 0};
};

} // namespace entity
