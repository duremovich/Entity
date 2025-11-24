#include "entity/ui/StageWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/media/FrameRingBuffer.hpp"  // For DecodedFrame definition

namespace entity {

StageWindow::StageWindow(Engine* engine)
    : m_engine(engine) {
    // Constructor
}

void StageWindow::render() {
    // Get the available content region
    ImVec2 contentSize = ImGui::GetContentRegionAvail();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetCursorScreenPos();

    // Get current video frame from Engine
    const DecodedFrame* frame = m_engine ? m_engine->getCurrentVideoFrame() : nullptr;

    if (frame && frame->valid && !frame->data.empty()) {
        // Display the decoded video frame
        // TODO: Create ImGui texture from RGBA data and display it
        // For now, just show a colored rectangle to indicate frame is loaded

        // Draw frame as colored background (placeholder until texture upload is implemented)
        drawList->AddRectFilled(
            windowPos,
            ImVec2(windowPos.x + contentSize.x, windowPos.y + contentSize.y),
            IM_COL32(0, 80, 120, 255)  // Blue-ish to indicate video is loaded
        );

        // Display frame info
        ImVec2 center(windowPos.x + contentSize.x * 0.5f, windowPos.y + contentSize.y * 0.5f);
        char frameInfo[128];
        snprintf(frameInfo, sizeof(frameInfo),
                 "Video Frame Loaded\nFrame: %lld\nResolution: %ux%u\nData: %.2f MB",
                 static_cast<long long>(frame->frameNumber),
                 frame->width, frame->height,
                 frame->data.size() / (1024.0 * 1024.0));

        ImVec2 textSize = ImGui::CalcTextSize(frameInfo);
        ImVec2 textPos(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f);

        // Draw semi-transparent background behind text
        drawList->AddRectFilled(
            ImVec2(textPos.x - 10.0f, textPos.y - 5.0f),
            ImVec2(textPos.x + textSize.x + 10.0f, textPos.y + textSize.y + 5.0f),
            IM_COL32(0, 0, 0, 180)
        );

        drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), frameInfo);

        // TODO: Phase 5 - Implement texture upload to GPU
        // - Create D3D12 texture resource from frame->data
        // - Upload RGBA data to GPU
        // - Get ImTextureID from renderer
        // - Use ImGui::Image() to display texture with proper aspect ratio
    } else {
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
        const char* instructionText = "No Video Loaded\n\nFile > Open Video to load PNG sequence or video file";
        ImVec2 textSize = ImGui::CalcTextSize(instructionText);
        ImVec2 textPos(center.x - textSize.x * 0.5f, center.y + crosshairSize + 10.0f);

        drawList->AddText(textPos, IM_COL32(150, 150, 150, 255), instructionText);
    }
}

} // namespace entity
