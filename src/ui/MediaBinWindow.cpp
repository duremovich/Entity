#include "entity/ui/MediaBinWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/components/Clip.hpp"
#include <imgui.h>
#include <iostream>
#include <unordered_map>
#include <tuple>

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
        // Build metadata cache once per frame to avoid O(n²) lookup
        // Cache structure: filepath -> (width, height, framerate, totalMediaFrames, hasAlpha)
        // Use totalMediaFrames (source frames) for displaying actual media duration
        std::unordered_map<std::string, std::tuple<uint32_t, uint32_t, double, FrameNumber, bool>> metadataCache;

        auto& registry = m_engine->getRegistry();
        auto clipView = registry.view<Clip>();
        for (auto [entity, clip] : clipView.each()) {
            // Only cache the first occurrence of each filepath (clips may share media files)
            if (metadataCache.find(clip.filepath) == metadataCache.end()) {
                metadataCache[clip.filepath] = std::make_tuple(
                    clip.width,
                    clip.height,
                    clip.framerate,
                    clip.totalMediaFrames,  // Source frames, not timeline frames
                    clip.hasAlpha
                );
            }
        }

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

            // Render each media item
            for (size_t i = 0; i < mediaFiles.size(); i++) {
                const std::string& filepath = mediaFiles[i].originalPath;

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

                // Look up metadata from cache (O(1) instead of O(n))
                bool hasMetadata = false;
                uint32_t width = 0, height = 0;
                FrameNumber duration = 0;
                double framerate = 0.0;
                bool hasAlpha = false;

                auto it = metadataCache.find(filepath);
                if (it != metadataCache.end()) {
                    hasMetadata = true;
                    std::tie(width, height, framerate, duration, hasAlpha) = it->second;
                }

                // Resolution column
                ImGui::TableSetColumnIndex(1);
                if (hasMetadata) {
                    ImGui::Text("%ux%u", width, height);
                } else {
                    ImGui::TextDisabled("--");
                }

                // Duration column
                ImGui::TableSetColumnIndex(2);
                if (hasMetadata && framerate > 0) {
                    double durationSec = static_cast<double>(duration) / framerate;
                    int minutes = static_cast<int>(durationSec) / 60;
                    int seconds = static_cast<int>(durationSec) % 60;
                    int frames = duration % static_cast<FrameNumber>(framerate);
                    ImGui::Text("%d:%02d:%02d", minutes, seconds, static_cast<int>(frames));
                } else {
                    ImGui::TextDisabled("--");
                }

                // FPS column
                ImGui::TableSetColumnIndex(3);
                if (hasMetadata) {
                    ImGui::Text("%.2f", framerate);
                } else {
                    ImGui::TextDisabled("--");
                }

                // Alpha column
                ImGui::TableSetColumnIndex(4);
                if (hasMetadata) {
                    ImGui::Text("%s", hasAlpha ? "Yes" : "No");
                } else {
                    ImGui::TextDisabled("--");
                }
            }

            ImGui::EndTable();
        }
    }
}

} // namespace entity
