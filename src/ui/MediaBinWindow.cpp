#include "entity/ui/MediaBinWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/TranscodeManager.hpp"
#include "entity/components/Clip.hpp"
#include <imgui.h>
#include <iostream>
#include <unordered_map>
#include <tuple>

namespace entity {

MediaBinWindow::MediaBinWindow(Engine* engine)
    : m_engine(engine) {
}

namespace {

// Rendered in the Status column per entry. Returns true if the entry is
// in a state where dragging to the timeline is legal.
struct StatusDisplay {
    bool dragAllowed{true};
    TranscodeState transState{TranscodeState::Done};
    bool hasActiveWorker{false};
    float progress01{0.0f};
    int64_t framesDone{0};
    int64_t framesTotal{0};
    Result failureResult{Result::Success};  // only meaningful when state == Failed
    bool hasTranscode{false};
    bool isSourceAlreadyHap{false};
};

StatusDisplay computeStatus(const ProjectManager::MediaLibraryEntry& entry,
                             const TranscodeManager* tmgr) {
    StatusDisplay d;
    d.hasTranscode = !entry.transcodedPath.empty();
    d.isSourceAlreadyHap = isHapMediaType(detectMediaType(entry.originalPath));

    if (tmgr) {
        if (auto s = tmgr->statusOf(entry.originalPath)) {
            d.hasActiveWorker = true;
            d.transState    = s->state;
            d.progress01    = s->progress01;
            d.framesDone    = s->framesDone;
            d.framesTotal   = s->framesTotal;
            d.failureResult = s->result;
        }
    }

    // Drag is allowed whenever we have something playable: either the
    // source is already HAP, or there's a transcode on disk. While a
    // worker is running we block drag — the clip we'd create would use
    // the non-HAP original (slow path) and the user probably just wants
    // to wait.
    const bool running = d.hasActiveWorker &&
                         (d.transState == TranscodeState::Queued ||
                          d.transState == TranscodeState::Running);
    d.dragAllowed = (d.isSourceAlreadyHap || d.hasTranscode) && !running;
    return d;
}

// One-line label shown under the progress bar, or in place of it.
void renderStatusCell(const StatusDisplay& d) {
    if (d.hasActiveWorker && d.transState == TranscodeState::Running) {
        char label[64];
        if (d.framesTotal > 0) {
            std::snprintf(label, sizeof(label), "%lld / %lld",
                          static_cast<long long>(d.framesDone),
                          static_cast<long long>(d.framesTotal));
        } else {
            std::snprintf(label, sizeof(label), "%lld frames",
                          static_cast<long long>(d.framesDone));
        }
        ImGui::ProgressBar(d.progress01, ImVec2(-FLT_MIN, 0), label);
    } else if (d.hasActiveWorker && d.transState == TranscodeState::Queued) {
        ImGui::TextDisabled("Queued");
    } else if (d.hasActiveWorker && d.transState == TranscodeState::Failed) {
        ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
                           "Failed: %s", ResultToString(d.failureResult));
    } else if (d.hasTranscode) {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.0f), "HAP (transcoded)");
    } else if (d.isSourceAlreadyHap) {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.0f), "HAP");
    } else {
        ImGui::TextDisabled("Source");  // non-HAP, not transcoded
    }
}

} // namespace

void MediaBinWindow::render() {
    const auto& mediaFiles = m_engine->getLoadedMediaFiles();
    TranscodeManager* tmgr = m_engine->getTranscodeManager();

    // --- Toolbar: auto-transcode preference --------------------------------
    bool autoTranscode = m_engine->autoTranscodeOnImport();
    if (ImGui::Checkbox("Auto-transcode to HAP on import", &autoTranscode)) {
        m_engine->setAutoTranscodeOnImport(autoTranscode);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "When on, ProRes / H264 imports are re-encoded to HAP in the\n"
            "background for realtime multi-layer playback. Source stays\n"
            "untouched; the cached HAP file lives in <project>/.cache/hap/.");
    }

    ImGui::SameLine();
    ImGui::Text("  |  Loaded: %zu", mediaFiles.size());
    ImGui::Separator();

    if (mediaFiles.empty()) {
        ImGui::TextDisabled("No media loaded");
        ImGui::Spacing();
        ImGui::TextWrapped("Use File > Open Video to import media files.");
        return;
    }

    // Build a per-path metadata cache from whatever clips exist (may be
    // empty — a freshly-imported file that's still transcoding has no
    // clip yet, so its resolution / duration cells will show "--").
    std::unordered_map<std::string, std::tuple<uint32_t, uint32_t, double, FrameNumber, bool>> metadataCache;
    {
        auto& registry = m_engine->getRegistry();
        auto clipView = registry.view<Clip>();
        for (auto [entity, clip] : clipView.each()) {
            if (metadataCache.find(clip.filepath) == metadataCache.end()) {
                metadataCache[clip.filepath] = std::make_tuple(
                    clip.width, clip.height, clip.framerate,
                    clip.totalMediaFrames, clip.hasAlpha);
            }
        }
    }

    if (ImGui::BeginTable("MediaBinTable", 6,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Filename",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status",     ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("Resolution", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Duration",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("FPS",        ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Alpha",      ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < mediaFiles.size(); i++) {
            const auto& entry = mediaFiles[i];
            const std::string& filepath = entry.originalPath;
            const StatusDisplay status = computeStatus(entry, tmgr);

            ImGui::TableNextRow();

            // --- Filename ---
            ImGui::TableSetColumnIndex(0);
            const size_t lastSlash = filepath.find_last_of("/\\");
            const std::string filename = (lastSlash != std::string::npos)
                ? filepath.substr(lastSlash + 1) : filepath;

            ImGui::PushID(static_cast<int>(i));
            ImGui::Selectable(filename.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

            // Tooltip with the full original path + where the transcode landed.
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Source: %s", filepath.c_str());
                if (!entry.transcodedPath.empty()) {
                    ImGui::Text("Transcoded: %s", entry.transcodedPath.c_str());
                    ImGui::Text("Variant: %s", entry.variant.c_str());
                }
                if (status.hasActiveWorker && status.transState == TranscodeState::Failed) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
                                       "Transcode failed: %s",
                                       ResultToString(status.failureResult));
                    ImGui::TextDisabled("Right-click for Retry. Check console for FFmpeg detail.");
                }
                ImGui::EndTooltip();
            }

            // Drag source — gated to entries that have something playable
            // and no in-flight worker. Payload stays the original path;
            // onMediaDroppedOnTimeline resolves via decoderPathFor.
            if (status.dragAllowed) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("MEDIA_FILE", filepath.c_str(), filepath.size() + 1);
                    ImGui::Text("Drag to Timeline:");
                    ImGui::Text("%s", filename.c_str());
                    ImGui::EndDragDropSource();
                }
            }

            // Right-click context menu: cancel running transcodes, kick
            // off one manually when auto-transcode is off.
            if (ImGui::BeginPopupContextItem("mediabin_row")) {
                if (status.hasActiveWorker && status.transState == TranscodeState::Running) {
                    if (ImGui::MenuItem("Cancel transcode")) {
                        if (tmgr) tmgr->cancel(filepath);
                    }
                }
                const bool canQueue =
                    !status.isSourceAlreadyHap &&
                    !status.hasTranscode &&
                    !(status.hasActiveWorker &&
                      (status.transState == TranscodeState::Running ||
                       status.transState == TranscodeState::Queued));
                if (canQueue) {
                    if (ImGui::MenuItem("Transcode to HAP")) {
                        if (tmgr) tmgr->enqueue(filepath, "hap_alpha", 0.0);
                    }
                }
                if (status.hasActiveWorker && status.transState == TranscodeState::Failed) {
                    if (ImGui::MenuItem("Retry transcode")) {
                        if (tmgr) {
                            tmgr->remove(filepath);  // wipe the failed worker first
                            tmgr->enqueue(filepath, "hap_alpha", 0.0);
                        }
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            // --- Status ---
            ImGui::TableSetColumnIndex(1);
            renderStatusCell(status);

            // Metadata cache lookup — only populated once a clip exists.
            bool hasMetadata = false;
            uint32_t width = 0, height = 0;
            FrameNumber duration = 0;
            double framerate = 0.0;
            bool hasAlpha = false;
            if (auto it = metadataCache.find(filepath); it != metadataCache.end()) {
                hasMetadata = true;
                std::tie(width, height, framerate, duration, hasAlpha) = it->second;
            }

            ImGui::TableSetColumnIndex(2);
            if (hasMetadata) ImGui::Text("%ux%u", width, height);
            else             ImGui::TextDisabled("--");

            ImGui::TableSetColumnIndex(3);
            if (hasMetadata && framerate > 0) {
                const double durationSec = static_cast<double>(duration) / framerate;
                const int minutes = static_cast<int>(durationSec) / 60;
                const int seconds = static_cast<int>(durationSec) % 60;
                const int frames  = static_cast<int>(duration % static_cast<FrameNumber>(framerate));
                ImGui::Text("%d:%02d:%02d", minutes, seconds, frames);
            } else {
                ImGui::TextDisabled("--");
            }

            ImGui::TableSetColumnIndex(4);
            if (hasMetadata) ImGui::Text("%.2f", framerate);
            else             ImGui::TextDisabled("--");

            ImGui::TableSetColumnIndex(5);
            if (hasMetadata) ImGui::Text("%s", hasAlpha ? "Yes" : "No");
            else             ImGui::TextDisabled("--");
        }

        ImGui::EndTable();
    }
}

} // namespace entity
