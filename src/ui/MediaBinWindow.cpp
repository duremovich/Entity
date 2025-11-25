#include "entity/ui/MediaBinWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/media/Decoder.hpp"
#include <imgui.h>
#include <iostream>

namespace entity {

MediaBinWindow::MediaBinWindow(Engine* engine)
    : m_engine(engine) {
}

void MediaBinWindow::render() {
    const auto& mediaFiles = m_engine->getLoadedMediaFiles();

    // Display media count
    ImGui::Text("Loaded Media: %zu", mediaFiles.size());
    ImGui::Separator();

    if (mediaFiles.empty()) {
        // Show placeholder when no media is loaded
        ImGui::TextDisabled("No media loaded");
        ImGui::Spacing();
        ImGui::TextWrapped("Use File > Open Video to import media files.");
    } else {
        // Display media list in a table
        if (ImGui::BeginTable("MediaBinTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            // Setup columns
            ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Resolution", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("FPS", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Alpha", ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGui::TableSetupScrollFreeze(0, 1);  // Freeze header row
            ImGui::TableHeadersRow();

            // Get decoder for metadata (currently we only support one loaded file)
            const Decoder* decoder = m_engine->getDecoder();

            // Render each media item
            for (size_t i = 0; i < mediaFiles.size(); i++) {
                const std::string& filepath = mediaFiles[i];

                ImGui::TableNextRow();

                // Filename column
                ImGui::TableSetColumnIndex(0);
                // Extract just the filename from the full path
                size_t lastSlash = filepath.find_last_of("/\\");
                std::string filename = (lastSlash != std::string::npos)
                    ? filepath.substr(lastSlash + 1)
                    : filepath;

                // Make the row selectable and a drag source
                ImGui::Selectable(filename.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

                // Drag source for dragging media to timeline
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    // Store the file path as payload
                    ImGui::SetDragDropPayload("MEDIA_FILE", filepath.c_str(), filepath.size() + 1);

                    // Show preview while dragging
                    ImGui::Text("Drag to Timeline:");
                    ImGui::Text("%s", filename.c_str());

                    ImGui::EndDragDropSource();
                }

                // Check if this file matches the current decoder
                bool hasMetadata = decoder && decoder->isOpen() && decoder->getFilePath() == filepath;

                // Resolution column
                ImGui::TableSetColumnIndex(1);
                if (hasMetadata) {
                    ImGui::Text("%ux%u", decoder->getWidth(), decoder->getHeight());
                } else {
                    ImGui::TextDisabled("--");
                }

                // Duration column
                ImGui::TableSetColumnIndex(2);
                if (hasMetadata) {
                    double durationSec = static_cast<double>(decoder->getDuration()) / decoder->getFrameRate();
                    int minutes = static_cast<int>(durationSec) / 60;
                    int seconds = static_cast<int>(durationSec) % 60;
                    int frames = decoder->getDuration() % static_cast<int64_t>(decoder->getFrameRate());
                    ImGui::Text("%d:%02d:%02lld", minutes, seconds, static_cast<long long>(frames));
                } else {
                    ImGui::TextDisabled("--");
                }

                // FPS column
                ImGui::TableSetColumnIndex(3);
                if (hasMetadata) {
                    ImGui::Text("%.2f", decoder->getFrameRate());
                } else {
                    ImGui::TextDisabled("--");
                }

                // Alpha column
                ImGui::TableSetColumnIndex(4);
                if (hasMetadata) {
                    ImGui::Text("%s", decoder->hasAlpha() ? "Yes" : "No");
                } else {
                    ImGui::TextDisabled("--");
                }
            }

            ImGui::EndTable();
        }
    }
}

} // namespace entity
