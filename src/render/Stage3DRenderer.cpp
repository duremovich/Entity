#include "entity/render/Stage3DRenderer.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/systems/CameraControlSystem.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace entity {

Stage3DRenderer::Stage3DRenderer() {
    // Initialize camera to a good default view
    CameraControlSystem::reset(m_camera);
}

// Projection pipeline: pose → aspect-aware k1/k2 lens distortion →
// optional IDW residual warp → screen pixel. The math here MUST match
// what OutputManager applies when rendering to the physical projector;
// the calibration window's right pane uses this function to simulate
// the projector. See ADR-0011 for the framebuffer↔world chain and the
// list of sites that must move together when the lens model changes.
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

    // Optional radial lens distortion (Brown-Conrady, 2-term). Applied in
    // normalized image coords (the lens's actual radially-symmetric coordinate
    // system), NOT directly in NDC — for non-square aspect ratios these
    // differ. When both coefficients are 0 (the default), this is a no-op.
    if (lensK1 != 0.0f || lensK2 != 0.0f) {
        const float aspect = screenSize.x / std::max(screenSize.y, 1.0f);
        const float tanHalfFov = std::tan(glm::radians(tempCamera.fov) * 0.5f);
        const float sx = aspect * tanHalfFov;
        const float sy = tanHalfFov;
        float nx = ndc.x * sx;
        float ny = ndc.y * sy;
        float r2 = nx * nx + ny * ny;
        float scale = 1.0f + lensK1 * r2 + lensK2 * r2 * r2;
        nx *= scale;
        ny *= scale;
        ndc.x = nx / sx;
        ndc.y = ny / sy;
    }

    // Optional post-fit residual warp via inverse-distance weighting. Mirrors
    // OutputManager's warp so the right-pane preview matches the physical
    // projection when "Warp to Points" is enabled.
    if (!warpResiduals.empty()) {
        glm::vec2 uv((ndc.x + 1.0f) * 0.5f, (1.0f - ndc.y) * 0.5f);
        glm::vec2 weighted(0.0f);
        float wSum = 0.0f;
        bool exact = false;
        glm::vec2 exactRes(0.0f);
        for (const auto& [predUV, res] : warpResiduals) {
            glm::vec2 d = uv - predUV;
            float dist2 = d.x * d.x + d.y * d.y;
            if (dist2 < 1e-8f) { exact = true; exactRes = res; break; }
            float w = 1.0f / (dist2 + 1e-6f);
            weighted += res * w;
            wSum += w;
        }
        if (exact) uv += exactRes;
        else if (wSum > 1e-6f) uv += weighted / wSum;
        ndc.x = uv.x * 2.0f - 1.0f;
        ndc.y = 1.0f - uv.y * 2.0f;
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
    float hw = screenWidth * 0.5f * screenScale.x;
    float hh = screenHeight * 0.5f * screenScale.y;
    float baseY = screenElevation;

    // Build transformation matrix from screen transform
    // Order: Scale -> Rotate -> Translate
    glm::mat4 transform = glm::mat4(1.0f);

    // Translation (position offset + base elevation)
    transform = glm::translate(transform, glm::vec3(screenPosition.x, screenPosition.y + baseY, screenPosition.z));

    // Rotation (degrees to radians) - apply in Y, X, Z order (yaw, pitch, roll)
    transform = glm::rotate(transform, glm::radians(screenRotation.y), glm::vec3(0.0f, 1.0f, 0.0f));  // Yaw
    transform = glm::rotate(transform, glm::radians(screenRotation.x), glm::vec3(1.0f, 0.0f, 0.0f));  // Pitch
    transform = glm::rotate(transform, glm::radians(screenRotation.z), glm::vec3(0.0f, 0.0f, 1.0f));  // Roll

    // Helper lambda to transform a point
    auto transformPoint = [&transform](const glm::vec3& p) -> glm::vec3 {
        glm::vec4 result = transform * glm::vec4(p, 1.0f);
        return glm::vec3(result);
    };

    // Subdivide the quad for perspective-correct texture mapping
    // ImGui's AddImageQuad does linear interpolation in 2D which causes warping
    const int subdivisionsX = 32;
    const int subdivisionsY = 32;

    if (textureID) {
        // Draw subdivided textured quad
        for (int sy = 0; sy < subdivisionsY; ++sy) {
            for (int sx = 0; sx < subdivisionsX; ++sx) {
                // Calculate normalized coordinates for this sub-quad
                float u0 = static_cast<float>(sx) / subdivisionsX;
                float u1 = static_cast<float>(sx + 1) / subdivisionsX;
                float v0 = static_cast<float>(sy) / subdivisionsY;
                float v1 = static_cast<float>(sy + 1) / subdivisionsY;

                // Calculate local positions for this sub-quad (centered at origin)
                float x0 = -hw + u0 * 2.0f * hw;
                float x1 = -hw + u1 * 2.0f * hw;
                float y0 = hh - v0 * 2.0f * hh;  // Top
                float y1 = hh - v1 * 2.0f * hh;  // Bottom

                // Transform corners to world space
                glm::vec3 subCorners[4] = {
                    transformPoint(glm::vec3(x0, y0, 0)),  // Top-left
                    transformPoint(glm::vec3(x1, y0, 0)),  // Top-right
                    transformPoint(glm::vec3(x1, y1, 0)),  // Bottom-right
                    transformPoint(glm::vec3(x0, y1, 0))   // Bottom-left
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
            transformPoint(glm::vec3(-hw, hh, 0)),  // Top-left
            transformPoint(glm::vec3( hw, hh, 0)),  // Top-right
            transformPoint(glm::vec3( hw, -hh, 0)), // Bottom-right
            transformPoint(glm::vec3(-hw, -hh, 0))  // Bottom-left
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
        transformPoint(glm::vec3(-hw, hh, 0)),
        transformPoint(glm::vec3( hw, hh, 0)),
        transformPoint(glm::vec3( hw, -hh, 0)),
        transformPoint(glm::vec3(-hw, -hh, 0))
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

ImTextureID Stage3DRenderer::renderMeshes(D3D12Renderer* renderer,
                                          const Camera& camera,
                                          const std::vector<StageMeshDraw>& draws,
                                          uint32_t viewportWidth,
                                          uint32_t viewportHeight) {
    if (!renderer) return 0;

    bool hasAny = false;
    for (const auto& d : draws) {
        if (d.meshSlot != UINT32_MAX) { hasAny = true; break; }
    }
    if (!hasAny) return 0;

    ImTextureID texId = static_cast<ImTextureID>(renderer->ensureStageTarget(viewportWidth, viewportHeight));
    if (!texId) return 0;

    glm::mat4 viewProj = camera.getViewProjectionMatrix();

    renderer->beginStageTarget();
    renderer->setStageCamera(viewProj);

    for (const auto& d : draws) {
        if (d.meshSlot == UINT32_MAX) continue;

        glm::mat4 model = glm::mat4(1.0f);
        const float yLift = d.isScreen ? screenElevation : 0.0f;
        model = glm::translate(model, glm::vec3(d.position.x, d.position.y + yLift, d.position.z));
        model = glm::rotate(model, glm::radians(d.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(d.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        model = glm::rotate(model, glm::radians(d.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, d.scale);

        renderer->drawStageMesh(d.meshSlot, model, d.tint, d.renderMode, d.srvSlot);
    }

    renderer->endStageTarget();
    return texId;
}

void Stage3DRenderer::beginRender(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize) {
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
}

void Stage3DRenderer::endRender(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize) {
    // Pop clip rect
    drawList->PopClipRect();

    // Draw camera info overlay
    char cameraInfo[128];
    snprintf(cameraInfo, sizeof(cameraInfo), "Yaw: %.0f  Pitch: %.0f  Dist: %.1f",
             m_camera.orbitYaw, m_camera.orbitPitch, m_camera.orbitDistance);
    drawList->AddText(ImVec2(screenPos.x + 5, screenPos.y + screenSize.y - 20),
                      IM_COL32(150, 150, 150, 255), cameraInfo);
}

void Stage3DRenderer::drawScreen(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize,
                                  const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale,
                                  ImTextureID textureID, bool isSelected, const MeshData* mesh) {
    const ImU32 frameColor = isSelected ? IM_COL32(255, 180, 50, 255) : IM_COL32(100, 100, 100, 255);
    const float frameThickness = isSelected ? 3.0f : 2.0f;

    // Mesh screens are rendered via the GPU pass (renderMeshes). This path handles
    // flat-quad screens only.
    (void)mesh;

    // --- Flat quad path ---
    float hw = screenWidth * 0.5f * scale.x;
    float hh = screenHeight * 0.5f * scale.y;

    glm::mat4 transform = glm::mat4(1.0f);
    transform = glm::translate(transform, glm::vec3(position.x, position.y + screenElevation, position.z));
    transform = glm::rotate(transform, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    transform = glm::rotate(transform, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    transform = glm::rotate(transform, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

    auto transformPoint = [&transform](const glm::vec3& p) -> glm::vec3 {
        return glm::vec3(transform * glm::vec4(p, 1.0f));
    };

    const int subdivisionsX = 32;
    const int subdivisionsY = 32;

    if (textureID) {
        for (int sy = 0; sy < subdivisionsY; ++sy) {
            for (int sx = 0; sx < subdivisionsX; ++sx) {
                float u0 = static_cast<float>(sx) / subdivisionsX;
                float u1 = static_cast<float>(sx + 1) / subdivisionsX;
                float v0 = static_cast<float>(sy) / subdivisionsY;
                float v1 = static_cast<float>(sy + 1) / subdivisionsY;

                float x0 = -hw + u0 * 2.0f * hw;
                float x1 = -hw + u1 * 2.0f * hw;
                float y0 = hh - v0 * 2.0f * hh;
                float y1 = hh - v1 * 2.0f * hh;

                glm::vec3 subCorners[4] = {
                    transformPoint(glm::vec3(x0, y0, 0)),
                    transformPoint(glm::vec3(x1, y0, 0)),
                    transformPoint(glm::vec3(x1, y1, 0)),
                    transformPoint(glm::vec3(x0, y1, 0))
                };

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

                ImVec2 uvs[4] = {
                    ImVec2(u0, v0), ImVec2(u1, v0), ImVec2(u1, v1), ImVec2(u0, v1)
                };

                drawList->AddImageQuad(textureID,
                    screenSubCorners[0], screenSubCorners[1], screenSubCorners[2], screenSubCorners[3],
                    uvs[0], uvs[1], uvs[2], uvs[3], IM_COL32_WHITE);
            }
        }
    } else {
        glm::vec3 corners[4] = {
            transformPoint(glm::vec3(-hw, hh, 0)),
            transformPoint(glm::vec3( hw, hh, 0)),
            transformPoint(glm::vec3( hw, -hh, 0)),
            transformPoint(glm::vec3(-hw, -hh, 0))
        };

        ImVec2 screenCorners[4];
        for (int i = 0; i < 4; ++i) {
            screenCorners[i] = projectPoint(corners[i], screenPos, screenSize);
            if (screenCorners[i].x < 0 && screenCorners[i].y < 0) return;
        }

        drawList->AddQuadFilled(screenCorners[0], screenCorners[1], screenCorners[2], screenCorners[3],
                                IM_COL32(40, 40, 45, 255));

        ImVec2 center;
        center.x = (screenCorners[0].x + screenCorners[2].x) * 0.5f;
        center.y = (screenCorners[0].y + screenCorners[2].y) * 0.5f;
        const char* text = "No Video";
        ImVec2 textSize = ImGui::CalcTextSize(text);
        drawList->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                          IM_COL32(150, 150, 150, 255), text);
    }

    // Draw frame/border
    glm::vec3 frameCorners[4] = {
        transformPoint(glm::vec3(-hw, hh, 0)),
        transformPoint(glm::vec3( hw, hh, 0)),
        transformPoint(glm::vec3( hw, -hh, 0)),
        transformPoint(glm::vec3(-hw, -hh, 0))
    };
    ImVec2 screenFrameCorners[4];
    for (int i = 0; i < 4; ++i) {
        screenFrameCorners[i] = projectPoint(frameCorners[i], screenPos, screenSize);
    }

    drawList->AddLine(screenFrameCorners[0], screenFrameCorners[1], frameColor, frameThickness);
    drawList->AddLine(screenFrameCorners[1], screenFrameCorners[2], frameColor, frameThickness);
    drawList->AddLine(screenFrameCorners[2], screenFrameCorners[3], frameColor, frameThickness);
    drawList->AddLine(screenFrameCorners[3], screenFrameCorners[0], frameColor, frameThickness);
}

void Stage3DRenderer::frameScreens(const std::vector<ScreenFrameInput>& screens,
                                    const std::vector<PropFrameInput>& props) {
    if (screens.empty() && props.empty()) {
        CameraControlSystem::reset(m_camera);
        return;
    }

    // Accumulate world-space AABB of every screen quad's 4 corners and
    // every prop mesh's vertices using the same transforms drawScreen()
    // and drawProp() apply.
    glm::vec3 minB(std::numeric_limits<float>::max());
    glm::vec3 maxB(std::numeric_limits<float>::lowest());

    for (const auto& s : screens) {
        glm::mat4 transform = glm::mat4(1.0f);
        transform = glm::translate(transform,
            glm::vec3(s.position.x, s.position.y + screenElevation, s.position.z));
        transform = glm::rotate(transform, glm::radians(s.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        transform = glm::rotate(transform, glm::radians(s.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        transform = glm::rotate(transform, glm::radians(s.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

        if (s.mesh && s.mesh->isValid()) {
            glm::mat4 meshTransform = glm::scale(transform, s.scale);
            for (const auto& v : s.mesh->vertices) {
                glm::vec3 wc = glm::vec3(meshTransform * glm::vec4(v.position[0], v.position[1], v.position[2], 1.0f));
                minB = glm::min(minB, wc);
                maxB = glm::max(maxB, wc);
            }
        } else {
            const float hw = screenWidth * 0.5f * s.scale.x;
            const float hh = screenHeight * 0.5f * s.scale.y;
            const glm::vec3 localCorners[4] = {
                glm::vec3(-hw,  hh, 0.0f),
                glm::vec3( hw,  hh, 0.0f),
                glm::vec3( hw, -hh, 0.0f),
                glm::vec3(-hw, -hh, 0.0f),
            };
            for (const auto& lc : localCorners) {
                glm::vec3 wc = glm::vec3(transform * glm::vec4(lc, 1.0f));
                minB = glm::min(minB, wc);
                maxB = glm::max(maxB, wc);
            }
        }
    }

    // Props: same chain minus the screenElevation lift, and require a mesh
    // (props without geometry contribute nothing to the AABB).
    for (const auto& p : props) {
        if (!p.mesh || !p.mesh->isValid()) continue;
        glm::mat4 transform = glm::mat4(1.0f);
        transform = glm::translate(transform, p.position);
        transform = glm::rotate(transform, glm::radians(p.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
        transform = glm::rotate(transform, glm::radians(p.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
        transform = glm::rotate(transform, glm::radians(p.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
        transform = glm::scale(transform, p.scale);
        for (const auto& v : p.mesh->vertices) {
            glm::vec3 wc = glm::vec3(transform * glm::vec4(v.position[0], v.position[1], v.position[2], 1.0f));
            minB = glm::min(minB, wc);
            maxB = glm::max(maxB, wc);
        }
    }

    const glm::vec3 center  = (minB + maxB) * 0.5f;
    const glm::vec3 extents = (maxB - minB) * 0.5f;
    const float radius = glm::length(extents);

    // Compute distance so the bounding sphere fits inside the smaller of
    // vertical and horizontal FOV. fov is the vertical FOV in degrees.
    const float vFov = glm::radians(m_camera.fov);
    const float hFov = 2.0f * std::atan(std::tan(vFov * 0.5f) * m_camera.aspectRatio);
    const float halfFov = std::min(vFov, hFov) * 0.5f;

    constexpr float kMargin = 1.4f;  // 40% breathing room
    float distance = (radius / std::sin(halfFov)) * kMargin;
    distance = glm::clamp(distance, Camera::MIN_DISTANCE, Camera::MAX_DISTANCE);

    m_camera.orbitTarget = center;
    m_camera.orbitDistance = distance;
    CameraControlSystem::updateFromOrbit(m_camera);
}

void Stage3DRenderer::render(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize,
                              ImTextureID composeTextureID) {
    // Use new begin/draw/end pattern for backwards compatibility
    beginRender(drawList, screenPos, screenSize);
    drawScreen(drawList, screenPos, screenSize, screenPosition, screenRotation, screenScale, composeTextureID, false);
    endRender(drawList, screenPos, screenSize);
}

void Stage3DRenderer::handleInput(ImVec2 mousePos, ImVec2 screenPos, ImVec2 screenSize,
                                   bool leftDown, bool rightDown, bool middleDown,
                                   bool shiftDown, float scrollDelta) {
    // Check if mouse is within render area
    bool inBounds = (mousePos.x >= screenPos.x && mousePos.x < screenPos.x + screenSize.x &&
                     mousePos.y >= screenPos.y && mousePos.y < screenPos.y + screenSize.y);

    // Handle scroll wheel for zoom
    if (inBounds && scrollDelta != 0.0f) {
        CameraControlSystem::zoom(m_camera, scrollDelta * 0.5f);
    }

    // Handle mouse drag
    if (leftDown || middleDown || rightDown) {
        if (m_isDragging) {
            // Calculate mouse delta
            float deltaX = (mousePos.x - m_lastMousePos.x) * 0.5f;
            float deltaY = (mousePos.y - m_lastMousePos.y) * 0.5f;

            if (middleDown || (leftDown && shiftDown)) {
                // Pan (middle button or shift+left)
                CameraControlSystem::pan(m_camera, deltaX * 0.01f, deltaY * 0.01f);
            } else if (leftDown) {
                // Orbit (left button)
                CameraControlSystem::orbit(m_camera, -deltaX, -deltaY);
            } else if (rightDown) {
                // Zoom (right button drag up/down)
                CameraControlSystem::zoom(m_camera, -deltaY * 0.05f);
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

bool Stage3DRenderer::hitTestScreen(ImVec2 mousePos, ImVec2 renderPos, ImVec2 renderSize,
                                     const glm::vec3& position, const glm::vec3& rotation,
                                     const glm::vec3& scale, const MeshData* mesh) const {
    glm::mat4 transform = glm::mat4(1.0f);
    transform = glm::translate(transform, glm::vec3(position.x, position.y + screenElevation, position.z));
    transform = glm::rotate(transform, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    transform = glm::rotate(transform, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    transform = glm::rotate(transform, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

    auto cross2D = [](ImVec2 o, ImVec2 a, ImVec2 p) {
        return (a.x - o.x) * (p.y - o.y) - (a.y - o.y) * (p.x - o.x);
    };

    if (mesh && mesh->isValid()) {
        glm::mat4 meshTransform = glm::scale(transform, scale);
        for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
            ImVec2 pts[3];
            bool valid = true;
            for (int j = 0; j < 3; ++j) {
                const auto& v = mesh->vertices[mesh->indices[i + j]];
                glm::vec4 world = meshTransform * glm::vec4(v.position[0], v.position[1], v.position[2], 1.0f);
                pts[j] = projectPoint(glm::vec3(world), renderPos, renderSize);
                if (pts[j].x < 0.0f && pts[j].y < 0.0f) { valid = false; break; }
            }
            if (!valid) continue;
            float d0 = cross2D(pts[0], pts[1], mousePos);
            float d1 = cross2D(pts[1], pts[2], mousePos);
            float d2 = cross2D(pts[2], pts[0], mousePos);
            if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0))
                return true;
        }
        return false;
    }

    // Flat quad path
    float hw = screenWidth * 0.5f * scale.x;
    float hh = screenHeight * 0.5f * scale.y;

    auto tp = [&](const glm::vec3& p) {
        return glm::vec3(transform * glm::vec4(p, 1.0f));
    };

    ImVec2 sc[4] = {
        projectPoint(tp(glm::vec3(-hw,  hh, 0)), renderPos, renderSize),
        projectPoint(tp(glm::vec3( hw,  hh, 0)), renderPos, renderSize),
        projectPoint(tp(glm::vec3( hw, -hh, 0)), renderPos, renderSize),
        projectPoint(tp(glm::vec3(-hw, -hh, 0)), renderPos, renderSize),
    };

    for (int i = 0; i < 4; ++i) {
        if (sc[i].x < 0 && sc[i].y < 0) return false;
    }

    float c0 = cross2D(sc[0], sc[1], mousePos);
    float c1 = cross2D(sc[1], sc[2], mousePos);
    float c2 = cross2D(sc[2], sc[3], mousePos);
    float c3 = cross2D(sc[3], sc[0], mousePos);
    return (c0 >= 0 && c1 >= 0 && c2 >= 0 && c3 >= 0) ||
           (c0 <= 0 && c1 <= 0 && c2 <= 0 && c3 <= 0);
}

void Stage3DRenderer::drawProp(ImDrawList* drawList, ImVec2 screenPos, ImVec2 screenSize,
                                const glm::vec3& position, const glm::vec3& rotation, const glm::vec3& scale,
                                const glm::vec4& displayColor, bool isSelected,
                                const MeshData* mesh) {
    const ImU32 frameColor = isSelected ? IM_COL32(255, 180, 50, 255) : IM_COL32(120, 120, 120, 200);
    const float frameThickness = isSelected ? 3.0f : 1.5f;

    // Prop meshes are rendered via the GPU pass (renderMeshes). This path handles
    // the no-mesh placeholder only.
    (void)rotation; (void)scale; (void)displayColor;
    if (mesh && mesh->isValid()) return;

    // No mesh: draw a small placeholder cross at the prop's origin so the
    // user can still see and click an empty prop slot.
    ImVec2 origin = projectPoint(position, screenPos, screenSize);
    if (origin.x >= 0.0f || origin.y >= 0.0f) {
        const float r = 6.0f;
        drawList->AddLine(ImVec2(origin.x - r, origin.y), ImVec2(origin.x + r, origin.y),
                          frameColor, frameThickness);
        drawList->AddLine(ImVec2(origin.x, origin.y - r), ImVec2(origin.x, origin.y + r),
                          frameColor, frameThickness);
        const char* txt = "(no mesh)";
        ImVec2 ts = ImGui::CalcTextSize(txt);
        drawList->AddText(ImVec2(origin.x - ts.x * 0.5f, origin.y + r + 2.0f),
                          IM_COL32(150, 150, 150, 200), txt);
    }
}

bool Stage3DRenderer::hitTestProp(ImVec2 mousePos, ImVec2 renderPos, ImVec2 renderSize,
                                   const glm::vec3& position, const glm::vec3& rotation,
                                   const glm::vec3& scale, const MeshData* mesh) const {
    if (!mesh || !mesh->isValid()) return false;

    // Same transform chain as drawProp (no screenElevation).
    glm::mat4 transform = glm::mat4(1.0f);
    transform = glm::translate(transform, position);
    transform = glm::rotate(transform, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    transform = glm::rotate(transform, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    transform = glm::rotate(transform, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
    transform = glm::scale(transform, scale);

    auto cross2D = [](ImVec2 o, ImVec2 a, ImVec2 p) {
        return (a.x - o.x) * (p.y - o.y) - (a.y - o.y) * (p.x - o.x);
    };

    for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
        ImVec2 pts[3];
        bool valid = true;
        for (int j = 0; j < 3; ++j) {
            const auto& v = mesh->vertices[mesh->indices[i + j]];
            glm::vec4 world = transform * glm::vec4(v.position[0], v.position[1], v.position[2], 1.0f);
            pts[j] = projectPoint(glm::vec3(world), renderPos, renderSize);
            if (pts[j].x < 0.0f && pts[j].y < 0.0f) { valid = false; break; }
        }
        if (!valid) continue;
        float d0 = cross2D(pts[0], pts[1], mousePos);
        float d1 = cross2D(pts[1], pts[2], mousePos);
        float d2 = cross2D(pts[2], pts[0], mousePos);
        if ((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0))
            return true;
    }
    return false;
}

void Stage3DRenderer::drawProjector(ImDrawList* drawList, ImVec2 renderPos, ImVec2 renderSize,
                                     const glm::vec3& position, const glm::vec3& rotation,
                                     float fovDegrees, bool enabled, bool isSelected,
                                     const char* label) {
    // Build rotation matrix (same Y→X→Z convention as screens)
    glm::mat4 rotMat = glm::mat4(1.0f);
    rotMat = glm::rotate(rotMat, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    rotMat = glm::rotate(rotMat, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    rotMat = glm::rotate(rotMat, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

    glm::vec3 forward = glm::vec3(rotMat * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    glm::vec3 right   = glm::vec3(rotMat * glm::vec4(1.0f, 0.0f,  0.0f, 0.0f));
    glm::vec3 up      = glm::vec3(rotMat * glm::vec4(0.0f, 1.0f,  0.0f, 0.0f));

    // Near plane at a fixed visual distance for the gizmo (independent of nearClip)
    const float vizDist = 0.6f;
    float halfH = vizDist * std::tan(glm::radians(fovDegrees * 0.5f));
    float halfW = halfH * (16.0f / 9.0f);

    glm::vec3 nearCenter = position + forward * vizDist;
    glm::vec3 nearTL = nearCenter + up * halfH - right * halfW;
    glm::vec3 nearTR = nearCenter + up * halfH + right * halfW;
    glm::vec3 nearBR = nearCenter - up * halfH + right * halfW;
    glm::vec3 nearBL = nearCenter - up * halfH - right * halfW;

    ImU32 color;
    if (isSelected)
        color = IM_COL32(255, 200, 80, 255);
    else if (!enabled)
        color = IM_COL32(200, 160, 60, 80);
    else
        color = IM_COL32(200, 160, 60, 200);
    float thickness = isSelected ? 2.0f : 1.5f;

    // Apex to near corners
    drawLine3D(drawList, position, nearTL, renderPos, renderSize, color, thickness);
    drawLine3D(drawList, position, nearTR, renderPos, renderSize, color, thickness);
    drawLine3D(drawList, position, nearBR, renderPos, renderSize, color, thickness);
    drawLine3D(drawList, position, nearBL, renderPos, renderSize, color, thickness);

    // Near plane rectangle
    drawLine3D(drawList, nearTL, nearTR, renderPos, renderSize, color, thickness);
    drawLine3D(drawList, nearTR, nearBR, renderPos, renderSize, color, thickness);
    drawLine3D(drawList, nearBR, nearBL, renderPos, renderSize, color, thickness);
    drawLine3D(drawList, nearBL, nearTL, renderPos, renderSize, color, thickness);

    // Small forward indicator from apex
    drawLine3D(drawList, position, position + forward * 0.2f, renderPos, renderSize, color, thickness);

    // Label at apex
    if (label) {
        ImVec2 apex2D = projectPoint(position, renderPos, renderSize);
        if (!(apex2D.x < 0.0f && apex2D.y < 0.0f)) {
            drawList->AddText(ImVec2(apex2D.x + 6.0f, apex2D.y - 8.0f), color, label);
        }
    }
}

Camera Stage3DRenderer::buildProjectorCamera(const glm::vec3& position,
                                             const glm::vec3& eulerDeg,
                                             float fovDeg,
                                             float aspect,
                                             float nearClip,
                                             float farClip) {
    // Convention: rotation[0]=pitch(X), rotation[1]=yaw(Y), rotation[2]=roll(Z)
    // Rotation order: Ry * Rx * Rz (yaw applied first in world space)
    // forward = Ry*Rx*Rz * (0, 0, -1)
    // With roll=0: forward = (-sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch))
    float pitchRad = glm::radians(eulerDeg.x);
    float yawRad   = glm::radians(eulerDeg.y);
    float rollRad  = glm::radians(eulerDeg.z);

    // Apply full Ry*Rx*Rz to the default forward direction (0, 0, -1)
    glm::mat4 rot(1.0f);
    rot = glm::rotate(rot, yawRad,   glm::vec3(0.0f, 1.0f, 0.0f));
    rot = glm::rotate(rot, pitchRad, glm::vec3(1.0f, 0.0f, 0.0f));
    rot = glm::rotate(rot, rollRad,  glm::vec3(0.0f, 0.0f, 1.0f));
    glm::vec3 forward = glm::vec3(rot * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
    glm::vec3 up      = glm::vec3(rot * glm::vec4(0.0f, 1.0f,  0.0f, 0.0f));

    Camera cam;
    cam.position    = position;
    cam.target      = position + forward;
    cam.up          = up;
    cam.fov         = fovDeg;
    cam.aspectRatio = aspect;
    cam.nearPlane   = nearClip;
    cam.farPlane    = farClip;
    return cam;
}

} // namespace entity
