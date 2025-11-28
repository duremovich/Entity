#include "entity/ui/StageWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/Screen.hpp"
#include "entity/media/FrameRingBuffer.hpp"  // For DecodedFrame definition
#include <algorithm>  // For std::sort
#include <vector>

namespace entity {

StageWindow::StageWindow(Engine* engine)
    : m_engine(engine)
    , m_3dRenderer(std::make_unique<Stage3DRenderer>()) {
    // Initialize camera to default position
    m_3dRenderer->getCamera().reset();
    m_3dRenderer->getCamera().updateFromOrbit();
}

void StageWindow::render() {
    // Get the available content region
    ImVec2 contentSize = ImGui::GetContentRegionAvail();

    // Reserve space for toolbar (if needed)
    float toolbarHeight = 25.0f;
    ImVec2 viewSize(contentSize.x, contentSize.y - toolbarHeight);

    // Render the view based on current mode
    if (m_viewMode == StageViewMode::View3D) {
        render3DView();
    } else {
        render2DView();
    }

    // Render toolbar at bottom
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + viewSize.y);
    renderToolbar();
}

void StageWindow::render2DView() {
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    contentSize.y -= 25.0f;  // Reserve toolbar space

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetCursorScreenPos();

    D3D12Renderer* renderer = m_engine ? m_engine->getRenderer() : nullptr;
    Timeline* timeline = m_engine ? m_engine->getTimeline() : nullptr;

    // Try to display the composited result from CompositorSystem
    if (renderer && renderer->isComposeTargetReady()) {
        void* textureID = renderer->getComposeTargetTextureID();

        if (textureID) {
            // Get compose target dimensions
            uint32_t composeWidth = renderer->getComposeTargetWidth();
            uint32_t composeHeight = renderer->getComposeTargetHeight();

            // Calculate aspect ratio fit
            float composeAspect = static_cast<float>(composeWidth) / static_cast<float>(composeHeight);
            float windowAspect = contentSize.x / contentSize.y;

            ImVec2 imageSize;
            ImVec2 imageOffset(0.0f, 0.0f);

            if (composeAspect > windowAspect) {
                // Compose target is wider than window - fit to width
                imageSize.x = contentSize.x;
                imageSize.y = contentSize.x / composeAspect;
                imageOffset.y = (contentSize.y - imageSize.y) * 0.5f;
            } else {
                // Compose target is taller than window - fit to height
                imageSize.y = contentSize.y;
                imageSize.x = contentSize.y * composeAspect;
                imageOffset.x = (contentSize.x - imageSize.x) * 0.5f;
            }

            // Draw black letterbox/pillarbox background
            drawList->AddRectFilled(
                windowPos,
                ImVec2(windowPos.x + contentSize.x, windowPos.y + contentSize.y),
                IM_COL32(0, 0, 0, 255)
            );

            // Display the composited texture (transforms already applied by CompositorSystem)
            ImVec2 imagePos(windowPos.x + imageOffset.x, windowPos.y + imageOffset.y);
            ImGui::SetCursorScreenPos(imagePos);
            ImGui::Image(
                textureID,
                imageSize,
                ImVec2(0.0f, 0.0f),  // UV start
                ImVec2(1.0f, 1.0f),  // UV end
                ImVec4(1.0f, 1.0f, 1.0f, 1.0f),  // Full opacity (compositing already done)
                ImVec4(0.0f, 0.0f, 0.0f, 0.0f)   // Border (none)
            );

            // Overlay frame info in corner
            if (timeline) {
                FrameNumber currentFrame = timeline->getCurrentFrame();
                char frameInfo[64];
                snprintf(frameInfo, sizeof(frameInfo), "Frame %u | %ux%u",
                         currentFrame, composeWidth, composeHeight);

                ImVec2 infoPos(windowPos.x + 5.0f, windowPos.y + 5.0f);
                drawList->AddText(infoPos, IM_COL32(255, 255, 255, 180), frameInfo);
            }

            return;  // Successfully rendered compose target
        }
    }

    // Fallback: No compose target ready, show placeholder
    {
        // No video loaded, display test pattern
        drawList->AddRectFilledMultiColor(
            windowPos,
            ImVec2(windowPos.x + contentSize.x, windowPos.y + contentSize.y),
            IM_COL32(40, 40, 45, 255),   // Top-left
            IM_COL32(40, 40, 45, 255),   // Top-right
            IM_COL32(25, 25, 30, 255),   // Bottom-right
            IM_COL32(25, 25, 30, 255)    // Bottom-left
        );

        // Draw center crosshair
        ImVec2 center(windowPos.x + contentSize.x * 0.5f, windowPos.y + contentSize.y * 0.5f);
        float crosshairSize = 20.0f;

        drawList->AddLine(
            ImVec2(center.x - crosshairSize, center.y),
            ImVec2(center.x + crosshairSize, center.y),
            IM_COL32(100, 100, 100, 255),
            1.0f
        );

        drawList->AddLine(
            ImVec2(center.x, center.y - crosshairSize),
            ImVec2(center.x, center.y + crosshairSize),
            IM_COL32(100, 100, 100, 255),
            1.0f
        );

        // Draw instruction text
        const char* instructionText = "No Video Loaded\n\nFile > Import Video to load PNG sequence or video file";
        ImVec2 textSize = ImGui::CalcTextSize(instructionText);
        ImVec2 textPos(center.x - textSize.x * 0.5f, center.y + crosshairSize + 10.0f);

        drawList->AddText(textPos, IM_COL32(150, 150, 150, 255), instructionText);
    }
}

void StageWindow::render3DView() {
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    contentSize.y -= 25.0f;  // Reserve toolbar space

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetCursorScreenPos();

    D3D12Renderer* renderer = m_engine ? m_engine->getRenderer() : nullptr;
    Timeline* timeline = m_engine ? m_engine->getTimeline() : nullptr;

    // Get selected screen for highlighting
    entt::entity selectedScreen = timeline ? timeline->getSelectedScreen() : entt::null;

    // Begin 3D rendering (draws background, grid, axes)
    m_3dRenderer->beginRender(drawList, windowPos, contentSize);

    // Collect visible screens and sort by distance from camera (painter's algorithm)
    // Draw farthest screens first so nearer screens are drawn on top
    if (m_engine) {
        auto& registry = m_engine->getRegistry();
        auto screenView = registry.view<Screen>();

        // Get camera position for depth sorting
        glm::vec3 cameraPos = m_3dRenderer->getCamera().position;

        // Collect screen data for sorting
        struct ScreenDrawData {
            entt::entity entity;
            glm::vec3 position;
            glm::vec3 rotation;
            glm::vec3 scale;
            bool isSelected;
            float distanceFromCamera;
            ImTextureID textureID;  // Per-screen compose target texture
        };
        std::vector<ScreenDrawData> screensToDraw;

        for (auto [entity, screen] : screenView.each()) {
            if (screen.visible) {
                glm::vec3 position(screen.position[0], screen.position[1], screen.position[2]);
                glm::vec3 rotation(screen.rotation[0], screen.rotation[1], screen.rotation[2]);
                glm::vec3 scale(screen.scale[0], screen.scale[1], screen.scale[2]);
                bool isSelected = (entity == selectedScreen);

                // Calculate distance from camera to screen center
                // Add default elevation offset (screens are centered at y=0.5 by default)
                glm::vec3 screenCenter = position + glm::vec3(0.0f, m_3dRenderer->screenElevation, 0.0f);
                float dist = glm::length(screenCenter - cameraPos);

                // Get THIS screen's compose target texture
                ImTextureID screenTextureID = nullptr;
                if (renderer && screen.renderTargetValid &&
                    screen.renderTargetSlot != UINT32_MAX) {
                    screenTextureID = static_cast<ImTextureID>(
                        renderer->getComposeTargetTextureID(screen.renderTargetSlot));
                }

                screensToDraw.push_back({entity, position, rotation, scale, isSelected, dist, screenTextureID});
            }
        }

        // Sort by distance (farthest first for painter's algorithm)
        std::sort(screensToDraw.begin(), screensToDraw.end(),
                  [](const ScreenDrawData& a, const ScreenDrawData& b) {
                      return a.distanceFromCamera > b.distanceFromCamera;
                  });

        // Draw screens in sorted order (farthest to nearest)
        for (const auto& screenData : screensToDraw) {
            m_3dRenderer->drawScreen(drawList, windowPos, contentSize,
                                     screenData.position, screenData.rotation, screenData.scale,
                                     screenData.textureID, screenData.isSelected);
        }
    }

    // End 3D rendering (draws overlays)
    m_3dRenderer->endRender(drawList, windowPos, contentSize);

    // Handle input
    ImGuiIO& io = ImGui::GetIO();
    bool isHovered = ImGui::IsWindowHovered();

    if (isHovered) {
        // Handle camera controls
        m_3dRenderer->handleInput(
            io.MousePos,
            windowPos, contentSize,
            io.MouseDown[0],  // Left button
            io.MouseDown[1],  // Right button
            io.MouseDown[2],  // Middle button
            io.KeyShift,
            io.MouseWheel
        );

        // TODO: Implement proper click-to-select for screens using ray-screen intersection
        // For now, users can select screens from the Screens window
    }

    // Advance cursor past the 3D view area
    ImGui::Dummy(contentSize);
}

void StageWindow::renderToolbar() {
    ImGui::Separator();

    // View mode buttons
    ImGui::BeginGroup();

    bool is2D = (m_viewMode == StageViewMode::View2D);
    bool is3D = (m_viewMode == StageViewMode::View3D);

    if (is2D) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::SmallButton("2D")) {
        m_viewMode = StageViewMode::View2D;
    }
    if (is2D) ImGui::PopStyleColor();

    ImGui::SameLine();

    if (is3D) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::SmallButton("3D")) {
        m_viewMode = StageViewMode::View3D;
    }
    if (is3D) ImGui::PopStyleColor();

    // Camera presets (only in 3D mode)
    if (m_viewMode == StageViewMode::View3D && m_3dRenderer) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::SmallButton("Reset")) {
            m_3dRenderer->getCamera().reset();
            m_3dRenderer->getCamera().updateFromOrbit();
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Front")) {
            m_3dRenderer->getCamera().setFrontView();
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Top")) {
            m_3dRenderer->getCamera().setTopView();
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Side")) {
            m_3dRenderer->getCamera().setSideView();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("| LMB: Orbit  MMB: Pan  Scroll: Zoom");
    }

    ImGui::EndGroup();
}

} // namespace entity
