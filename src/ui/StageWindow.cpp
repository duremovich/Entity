#include "entity/ui/StageWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/render/D3D12Renderer.hpp"
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
    D3D12Renderer* renderer = m_engine ? m_engine->getRenderer() : nullptr;

    if (frame && frame->valid && !frame->data.empty() && renderer) {
        // Upload frame to GPU texture
        void* textureID = renderer->uploadVideoFrame(
            frame->data.data(),
            frame->width,
            frame->height
        );

        if (textureID) {
            // Calculate aspect ratio fit
            float videoAspect = static_cast<float>(frame->width) / static_cast<float>(frame->height);
            float windowAspect = contentSize.x / contentSize.y;

            ImVec2 imageSize;
            ImVec2 imageOffset(0.0f, 0.0f);

            if (videoAspect > windowAspect) {
                // Video is wider than window - fit to width
                imageSize.x = contentSize.x;
                imageSize.y = contentSize.x / videoAspect;
                imageOffset.y = (contentSize.y - imageSize.y) * 0.5f;
            } else {
                // Video is taller than window - fit to height
                imageSize.y = contentSize.y;
                imageSize.x = contentSize.y * videoAspect;
                imageOffset.x = (contentSize.x - imageSize.x) * 0.5f;
            }

            // Draw black letterbox/pillarbox background
            drawList->AddRectFilled(
                windowPos,
                ImVec2(windowPos.x + contentSize.x, windowPos.y + contentSize.y),
                IM_COL32(0, 0, 0, 255)
            );

            // Display the video texture
            ImVec2 imagePos(windowPos.x + imageOffset.x, windowPos.y + imageOffset.y);
            ImGui::SetCursorScreenPos(imagePos);
            ImGui::Image(
                textureID,
                imageSize,
                ImVec2(0.0f, 0.0f),  // UV start
                ImVec2(1.0f, 1.0f),  // UV end
                ImVec4(1.0f, 1.0f, 1.0f, 1.0f),  // Tint (white = no tint)
                ImVec4(0.0f, 0.0f, 0.0f, 0.0f)   // Border (none)
            );

            // Overlay frame info in corner (optional - can be toggled)
            char frameInfo[64];
            snprintf(frameInfo, sizeof(frameInfo), "Frame %lld | %ux%u",
                     static_cast<long long>(frame->frameNumber),
                     frame->width, frame->height);

            ImVec2 infoPos(windowPos.x + 5.0f, windowPos.y + 5.0f);
            drawList->AddText(infoPos, IM_COL32(255, 255, 255, 180), frameInfo);
        } else {
            // Texture upload failed, show error
            drawList->AddRectFilled(
                windowPos,
                ImVec2(windowPos.x + contentSize.x, windowPos.y + contentSize.y),
                IM_COL32(80, 0, 0, 255)  // Red to indicate error
            );

            const char* errorText = "Texture Upload Failed";
            ImVec2 textSize = ImGui::CalcTextSize(errorText);
            ImVec2 center(windowPos.x + contentSize.x * 0.5f, windowPos.y + contentSize.y * 0.5f);
            drawList->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y), IM_COL32(255, 255, 255, 255), errorText);
        }
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
