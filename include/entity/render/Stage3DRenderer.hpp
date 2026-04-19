#pragma once

#include "../components/Camera.hpp"
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
     * Draw a single screen quad with the given transform.
     * Call between beginRender() and endRender().
     * @param position World position (x, y, z)
     * @param rotation Rotation in degrees (pitch, yaw, roll)
     * @param scale Scale factors (x, y, z)
     * @param textureID Texture to display on the screen
     * @param isSelected Whether this screen is selected (draws highlight)
     */
    void drawScreen(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize,
                    const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale,
                    ImTextureID textureID, bool isSelected = false);

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
     * Get the camera for external manipulation.
     */
    Camera& getCamera() { return m_camera; }
    const Camera& getCamera() const { return m_camera; }

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
