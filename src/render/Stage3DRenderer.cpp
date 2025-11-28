#include "entity/render/Stage3DRenderer.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

namespace entity {

Stage3DRenderer::Stage3DRenderer() {
    // Initialize camera to a good default view
    m_camera.reset();
    m_camera.updateFromOrbit();
}

ImVec2 Stage3DRenderer::projectPoint(const glm::vec3& worldPos,
                                      ImVec2 screenPos, ImVec2 screenSize) const {
    // Update camera aspect ratio
    Camera tempCamera = m_camera;
    tempCamera.aspectRatio = screenSize.x / screenSize.y;

    // Get view-projection matrix
    glm::mat4 vp = tempCamera.getViewProjectionMatrix();

    // Transform to clip space
    glm::vec4 clipPos = vp * glm::vec4(worldPos, 1.0f);

    // Check if behind camera
    if (clipPos.w <= 0.001f) {
        return ImVec2(-1, -1);
    }

    // Perspective divide to NDC
    glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;

    // Check if outside frustum
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z < 0.0f || ndc.z > 1.0f) {
        // Allow some tolerance for grid lines that extend past viewport
        // return ImVec2(-1, -1);
    }

    // Convert NDC to screen coordinates
    // NDC: -1,-1 = bottom-left, +1,+1 = top-right
    // Screen: 0,0 = top-left, w,h = bottom-right
    float x = screenPos.x + (ndc.x + 1.0f) * 0.5f * screenSize.x;
    float y = screenPos.y + (1.0f - ndc.y) * 0.5f * screenSize.y;  // Flip Y

    return ImVec2(x, y);
}

void Stage3DRenderer::drawLine3D(ImDrawList* drawList, const glm::vec3& p1, const glm::vec3& p2,
                                  ImVec2 screenPos, ImVec2 screenSize, ImU32 color, float thickness) {
    ImVec2 sp1 = projectPoint(p1, screenPos, screenSize);
    ImVec2 sp2 = projectPoint(p2, screenPos, screenSize);

    // Skip if either point is behind camera
    if (sp1.x < 0 && sp1.y < 0) return;
    if (sp2.x < 0 && sp2.y < 0) return;

    // Clip to screen bounds with some margin
    float margin = 1000.0f;
    float minX = screenPos.x - margin;
    float maxX = screenPos.x + screenSize.x + margin;
    float minY = screenPos.y - margin;
    float maxY = screenPos.y + screenSize.y + margin;

    // Simple bounds check
    if ((sp1.x < minX && sp2.x < minX) || (sp1.x > maxX && sp2.x > maxX) ||
        (sp1.y < minY && sp2.y < minY) || (sp1.y > maxY && sp2.y > maxY)) {
        return;
    }

    drawList->AddLine(sp1, sp2, color, thickness);
}

void Stage3DRenderer::drawFloorGrid(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize) {
    float halfSize = gridSize * 0.5f;

    // Draw minor grid lines
    float minorSpacing = gridSpacing / gridSubdivisions;
    for (float x = -halfSize; x <= halfSize; x += minorSpacing) {
        // Skip if on major line
        if (fmod(fabs(x), gridSpacing) < 0.001f) continue;
        drawLine3D(drawList, glm::vec3(x, 0, -halfSize), glm::vec3(x, 0, halfSize),
                   screenPos, screenSize, gridMinorColor, 1.0f);
    }
    for (float z = -halfSize; z <= halfSize; z += minorSpacing) {
        if (fmod(fabs(z), gridSpacing) < 0.001f) continue;
        drawLine3D(drawList, glm::vec3(-halfSize, 0, z), glm::vec3(halfSize, 0, z),
                   screenPos, screenSize, gridMinorColor, 1.0f);
    }

    // Draw major grid lines
    for (float x = -halfSize; x <= halfSize; x += gridSpacing) {
        ImU32 color = (fabs(x) < 0.001f) ? gridAxisZColor : gridMajorColor;
        float thickness = (fabs(x) < 0.001f) ? 2.0f : 1.0f;
        drawLine3D(drawList, glm::vec3(x, 0, -halfSize), glm::vec3(x, 0, halfSize),
                   screenPos, screenSize, color, thickness);
    }
    for (float z = -halfSize; z <= halfSize; z += gridSpacing) {
        ImU32 color = (fabs(z) < 0.001f) ? gridAxisXColor : gridMajorColor;
        float thickness = (fabs(z) < 0.001f) ? 2.0f : 1.0f;
        drawLine3D(drawList, glm::vec3(-halfSize, 0, z), glm::vec3(halfSize, 0, z),
                   screenPos, screenSize, color, thickness);
    }
}

void Stage3DRenderer::drawScreenQuad(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize,
                                      ImTextureID textureID) {
    // Screen quad in world space
    // Screen is centered at origin, elevated above floor
    float hw = screenWidth * 0.5f;
    float hh = screenHeight * 0.5f;
    float y = screenElevation;

    // Subdivide the quad for perspective-correct texture mapping
    // ImGui's AddImageQuad does linear interpolation in 2D which causes warping
    const int subdivisionsX = 16;
    const int subdivisionsY = 16;

    if (textureID) {
        // Draw subdivided textured quad
        for (int sy = 0; sy < subdivisionsY; ++sy) {
            for (int sx = 0; sx < subdivisionsX; ++sx) {
                // Calculate normalized coordinates for this sub-quad
                float u0 = static_cast<float>(sx) / subdivisionsX;
                float u1 = static_cast<float>(sx + 1) / subdivisionsX;
                float v0 = static_cast<float>(sy) / subdivisionsY;
                float v1 = static_cast<float>(sy + 1) / subdivisionsY;

                // Calculate world positions for this sub-quad
                float x0 = -hw + u0 * 2.0f * hw;
                float x1 = -hw + u1 * 2.0f * hw;
                float y0 = y + hh - v0 * 2.0f * hh;  // Top
                float y1 = y + hh - v1 * 2.0f * hh;  // Bottom

                glm::vec3 subCorners[4] = {
                    glm::vec3(x0, y0, 0),  // Top-left
                    glm::vec3(x1, y0, 0),  // Top-right
                    glm::vec3(x1, y1, 0),  // Bottom-right
                    glm::vec3(x0, y1, 0)   // Bottom-left
                };

                // Project corners to screen space
                ImVec2 screenSubCorners[4];
                bool allValid = true;
                for (int i = 0; i < 4; ++i) {
                    screenSubCorners[i] = projectPoint(subCorners[i], screenPos, screenSize);
                    if (screenSubCorners[i].x < 0 && screenSubCorners[i].y < 0) {
                        allValid = false;
                        break;
                    }
                }

                if (!allValid) continue;

                // UV coordinates for this sub-quad
                ImVec2 uvs[4] = {
                    ImVec2(u0, v0),  // Top-left
                    ImVec2(u1, v0),  // Top-right
                    ImVec2(u1, v1),  // Bottom-right
                    ImVec2(u0, v1)   // Bottom-left
                };

                drawList->AddImageQuad(
                    textureID,
                    screenSubCorners[0], screenSubCorners[1], screenSubCorners[2], screenSubCorners[3],
                    uvs[0], uvs[1], uvs[2], uvs[3],
                    IM_COL32_WHITE
                );
            }
        }
    } else {
        // Draw placeholder quad (no subdivision needed for solid color)
        glm::vec3 corners[4] = {
            glm::vec3(-hw, y + hh, 0),  // Top-left
            glm::vec3( hw, y + hh, 0),  // Top-right
            glm::vec3( hw, y - hh, 0),  // Bottom-right
            glm::vec3(-hw, y - hh, 0)   // Bottom-left
        };

        ImVec2 screenCorners[4];
        for (int i = 0; i < 4; ++i) {
            screenCorners[i] = projectPoint(corners[i], screenPos, screenSize);
            if (screenCorners[i].x < 0 && screenCorners[i].y < 0) {
                return;
            }
        }

        drawList->AddQuadFilled(
            screenCorners[0], screenCorners[1], screenCorners[2], screenCorners[3],
            IM_COL32(40, 40, 45, 255)
        );

        // Draw "No Video" text
        ImVec2 center;
        center.x = (screenCorners[0].x + screenCorners[2].x) * 0.5f;
        center.y = (screenCorners[0].y + screenCorners[2].y) * 0.5f;
        const char* text = "No Video";
        ImVec2 textSize = ImGui::CalcTextSize(text);
        drawList->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                          IM_COL32(150, 150, 150, 255), text);
    }

    // Draw screen frame/border (use corner points for frame)
    glm::vec3 frameCorners[4] = {
        glm::vec3(-hw, y + hh, 0),
        glm::vec3( hw, y + hh, 0),
        glm::vec3( hw, y - hh, 0),
        glm::vec3(-hw, y - hh, 0)
    };
    ImVec2 screenFrameCorners[4];
    for (int i = 0; i < 4; ++i) {
        screenFrameCorners[i] = projectPoint(frameCorners[i], screenPos, screenSize);
    }

    ImU32 frameColor = IM_COL32(100, 100, 100, 255);
    drawList->AddLine(screenFrameCorners[0], screenFrameCorners[1], frameColor, 2.0f);
    drawList->AddLine(screenFrameCorners[1], screenFrameCorners[2], frameColor, 2.0f);
    drawList->AddLine(screenFrameCorners[2], screenFrameCorners[3], frameColor, 2.0f);
    drawList->AddLine(screenFrameCorners[3], screenFrameCorners[0], frameColor, 2.0f);
}

void Stage3DRenderer::drawAxes(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize) {
    float axisLength = 0.5f;

    // X axis (red)
    drawLine3D(drawList, glm::vec3(0, 0.01f, 0), glm::vec3(axisLength, 0.01f, 0),
               screenPos, screenSize, IM_COL32(255, 80, 80, 255), 3.0f);

    // Y axis (green)
    drawLine3D(drawList, glm::vec3(0, 0, 0), glm::vec3(0, axisLength, 0),
               screenPos, screenSize, IM_COL32(80, 255, 80, 255), 3.0f);

    // Z axis (blue)
    drawLine3D(drawList, glm::vec3(0, 0.01f, 0), glm::vec3(0, 0.01f, axisLength),
               screenPos, screenSize, IM_COL32(80, 80, 255, 255), 3.0f);
}

void Stage3DRenderer::render(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize,
                              ImTextureID composeTextureID) {
    // Update camera aspect ratio
    m_camera.aspectRatio = screenSize.x / screenSize.y;

    // Draw background
    drawList->AddRectFilled(screenPos,
                            ImVec2(screenPos.x + screenSize.x, screenPos.y + screenSize.y),
                            IM_COL32(25, 25, 30, 255));

    // Push clip rect
    drawList->PushClipRect(screenPos,
                           ImVec2(screenPos.x + screenSize.x, screenPos.y + screenSize.y),
                           true);

    // Draw floor grid
    drawFloorGrid(drawList, screenPos, screenSize);

    // Draw coordinate axes
    drawAxes(drawList, screenPos, screenSize);

    // Draw the screen quad with composited texture
    drawScreenQuad(drawList, screenPos, screenSize, composeTextureID);

    // Pop clip rect
    drawList->PopClipRect();

    // Draw camera info overlay
    char cameraInfo[128];
    snprintf(cameraInfo, sizeof(cameraInfo), "Yaw: %.0f  Pitch: %.0f  Dist: %.1f",
             m_camera.orbitYaw, m_camera.orbitPitch, m_camera.orbitDistance);
    drawList->AddText(ImVec2(screenPos.x + 5, screenPos.y + screenSize.y - 20),
                      IM_COL32(150, 150, 150, 255), cameraInfo);
}

void Stage3DRenderer::handleInput(ImVec2 mousePos, ImVec2 screenPos, ImVec2 screenSize,
                                   bool leftDown, bool rightDown, bool middleDown,
                                   bool shiftDown, float scrollDelta) {
    // Check if mouse is within render area
    bool inBounds = (mousePos.x >= screenPos.x && mousePos.x < screenPos.x + screenSize.x &&
                     mousePos.y >= screenPos.y && mousePos.y < screenPos.y + screenSize.y);

    // Handle scroll wheel for zoom
    if (inBounds && scrollDelta != 0.0f) {
        m_camera.zoom(scrollDelta * 0.5f);
    }

    // Handle mouse drag
    if (leftDown || middleDown || rightDown) {
        if (m_isDragging) {
            // Calculate mouse delta
            float deltaX = (mousePos.x - m_lastMousePos.x) * 0.5f;
            float deltaY = (mousePos.y - m_lastMousePos.y) * 0.5f;

            if (middleDown || (leftDown && shiftDown)) {
                // Pan (middle button or shift+left)
                m_camera.pan(deltaX * 0.01f, deltaY * 0.01f);
            } else if (leftDown) {
                // Orbit (left button)
                m_camera.orbit(-deltaX, -deltaY);
            } else if (rightDown) {
                // Zoom (right button drag up/down)
                m_camera.zoom(-deltaY * 0.05f);
            }
        } else if (inBounds) {
            // Start dragging
            m_isDragging = true;
        }
        m_lastMousePos = mousePos;
    } else {
        m_isDragging = false;
    }
}

} // namespace entity
