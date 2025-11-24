/**
 * TimelineWidget Implementation
 *
 * ImGui-based widget for rendering and interacting with the timeline.
 */

#include "entity/timeline/TimelineWidget.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>

namespace entity {

TimelineWidget::TimelineWidget(Timeline* timeline)
    : m_timeline(timeline)
{
}

void TimelineWidget::render() {
    if (!m_timeline) {
        ImGui::Text("No timeline set");
        return;
    }

    // NoMove flag prevents dragging the window from anywhere except the title bar
    ImGui::Begin("Timeline", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Calculate timeline dimensions
    float totalHeight = RULER_HEIGHT;
    int trackCount = static_cast<int>(m_timeline->getTrackCount());
    totalHeight += trackCount * (TRACK_HEIGHT + TRACK_PADDING);

    // Create a child window for scrolling
    ImGui::BeginChild("TimelineScroll", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // Calculate timeline width based on duration
    float durationSeconds = m_timeline->getDuration() / 1000.0f;
    float timelineWidth = durationSeconds * m_pixelsPerSecond;

    // Set cursor position for custom drawing
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();

    // Make space for our custom content
    ImGui::Dummy(ImVec2(timelineWidth, totalHeight));

    // Render components
    renderTimeRuler();
    renderTracks();
    renderPlayhead();

    // Handle interaction
    handleInteraction();

    ImGui::EndChild();

    // Controls below timeline
    ImGui::Separator();

    // Playback controls
    if (ImGui::Button("Play")) {
        m_timeline->play();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause")) {
        m_timeline->pause();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        m_timeline->stop();
    }
    ImGui::SameLine();

    // Display current time
    Timecode currentTime = m_timeline->getCurrentTime();
    float currentSeconds = currentTime / 1000.0f;
    int minutes = static_cast<int>(currentSeconds / 60.0f);
    float seconds = currentSeconds - (minutes * 60.0f);
    ImGui::Text("Time: %02d:%06.3f", minutes, seconds);

    ImGui::SameLine();

    // Zoom controls
    ImGui::Text("Zoom:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::SliderFloat("##zoom", &m_pixelsPerSecond, 10.0f, 500.0f, "%.0f px/s");

    ImGui::End();
}

void TimelineWidget::renderTimeRuler() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Calculate ruler bounds
    ImVec2 rulerMin = windowPos;
    ImVec2 rulerMax = ImVec2(windowPos.x + windowSize.x, windowPos.y + RULER_HEIGHT);

    // Draw ruler background
    drawList->AddRectFilled(rulerMin, rulerMax, IM_COL32(40, 40, 40, 255));

    // Draw time markers
    float durationSeconds = m_timeline->getDuration() / 1000.0f;

    // Determine marker interval based on zoom
    float markerIntervalSeconds = 1.0f;
    if (m_pixelsPerSecond < 50.0f) {
        markerIntervalSeconds = 5.0f;
    } else if (m_pixelsPerSecond > 200.0f) {
        markerIntervalSeconds = 0.5f;
    }

    // Draw markers
    for (float t = 0.0f; t <= durationSeconds; t += markerIntervalSeconds) {
        float x = windowPos.x + timeToPixel(static_cast<Timecode>(t * 1000.0f));

        if (x < windowPos.x || x > rulerMax.x) continue;

        // Draw tick mark
        drawList->AddLine(
            ImVec2(x, rulerMax.y - 10.0f),
            ImVec2(x, rulerMax.y),
            IM_COL32(200, 200, 200, 255),
            1.0f
        );

        // Draw time label
        int minutes = static_cast<int>(t / 60.0f);
        float seconds = t - (minutes * 60.0f);

        std::ostringstream oss;
        oss << std::setfill('0') << std::setw(2) << minutes << ":"
            << std::fixed << std::setprecision(1) << std::setw(4) << seconds;

        drawList->AddText(
            ImVec2(x + 3.0f, rulerMin.y + 5.0f),
            IM_COL32(200, 200, 200, 255),
            oss.str().c_str()
        );
    }
}

void TimelineWidget::renderTracks() {
    if (!m_timeline) return;

    const auto& tracks = m_timeline->getTracks();
    for (size_t i = 0; i < tracks.size(); ++i) {
        renderTrack(tracks[i], static_cast<int>(i));
    }
}

void TimelineWidget::renderTrack(entt::entity trackEntity, int trackIndex) {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto& registry = m_timeline->getRegistry();
    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return;

    // Calculate track bounds
    float trackY = windowPos.y + RULER_HEIGHT + trackIndex * (TRACK_HEIGHT + TRACK_PADDING);
    ImVec2 trackMin = ImVec2(windowPos.x, trackY);
    ImVec2 trackMax = ImVec2(windowPos.x + windowSize.x, trackY + TRACK_HEIGHT);

    // Draw track background (alternating colors)
    ImU32 trackColor = (trackIndex % 2 == 0)
        ? IM_COL32(50, 50, 50, 255)
        : IM_COL32(45, 45, 45, 255);
    drawList->AddRectFilled(trackMin, trackMax, trackColor);

    // Draw track border
    drawList->AddRect(trackMin, trackMax, IM_COL32(70, 70, 70, 255));

    // Draw track label
    std::ostringstream labelStream;
    labelStream << "Track " << (trackIndex + 1);
    drawList->AddText(
        ImVec2(trackMin.x + 10.0f, trackMin.y + TRACK_HEIGHT / 2.0f - 7.0f),
        IM_COL32(180, 180, 180, 255),
        labelStream.str().c_str()
    );

    // Render clips in this track
    for (entt::entity clipEntity : track->clips) {
        renderClip(clipEntity, trackIndex);
    }
}

void TimelineWidget::renderClip(entt::entity clipEntity, int trackIndex) {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto& registry = m_timeline->getRegistry();
    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) return;

    // Calculate clip position and size
    float trackY = windowPos.y + RULER_HEIGHT + trackIndex * (TRACK_HEIGHT + TRACK_PADDING);

    // Convert frame timing to milliseconds
    float startSeconds = clip->startFrame / clip->framerate;
    float durationSeconds = clip->duration / clip->framerate;
    Timecode startTime = static_cast<Timecode>(startSeconds * 1000.0f);
    Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000.0f);

    float clipX = windowPos.x + timeToPixel(startTime);
    float clipWidth = timeToPixel(endTime - startTime);

    ImVec2 clipMin = ImVec2(clipX, trackY + CLIP_PADDING);
    ImVec2 clipMax = ImVec2(clipX + clipWidth, trackY + TRACK_HEIGHT - CLIP_PADDING);

    // Choose clip color based on selection
    bool isSelected = (clipEntity == m_selectedClip);
    ImU32 clipColor = isSelected
        ? IM_COL32(100, 150, 255, 255)
        : IM_COL32(80, 120, 180, 255);

    // Draw clip rectangle
    drawList->AddRectFilled(clipMin, clipMax, clipColor, 3.0f);
    drawList->AddRect(clipMin, clipMax, IM_COL32(120, 160, 220, 255), 3.0f, 0, 2.0f);

    // Draw clip label (filename without path)
    size_t lastSlash = clip->filepath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos)
        ? clip->filepath.substr(lastSlash + 1)
        : clip->filepath;

    // Truncate filename if too long
    if (filename.length() > 20) {
        filename = filename.substr(0, 17) + "...";
    }

    drawList->AddText(
        ImVec2(clipMin.x + 5.0f, clipMin.y + TRACK_HEIGHT / 2.0f - 12.0f),
        IM_COL32(255, 255, 255, 255),
        filename.c_str()
    );
}

void TimelineWidget::renderPlayhead() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Calculate playhead position
    Timecode currentTime = m_timeline->getCurrentTime();
    float playheadX = windowPos.x + timeToPixel(currentTime);

    // Calculate total height
    int trackCount = static_cast<int>(m_timeline->getTrackCount());
    float totalHeight = RULER_HEIGHT + trackCount * (TRACK_HEIGHT + TRACK_PADDING);

    // Draw playhead line
    drawList->AddLine(
        ImVec2(playheadX, windowPos.y),
        ImVec2(playheadX, windowPos.y + totalHeight),
        IM_COL32(255, 100, 100, 255),
        2.0f
    );

    // Draw playhead triangle at top
    ImVec2 trianglePoints[3] = {
        ImVec2(playheadX, windowPos.y),
        ImVec2(playheadX - 6.0f, windowPos.y + 10.0f),
        ImVec2(playheadX + 6.0f, windowPos.y + 10.0f)
    };
    drawList->AddTriangleFilled(trianglePoints[0], trianglePoints[1], trianglePoints[2],
                                IM_COL32(255, 100, 100, 255));
}

void TimelineWidget::handleInteraction() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImVec2 mousePos = ImGui::GetMousePos();
    ImGuiIO& io = ImGui::GetIO();

    // Check if mouse is over the timeline window
    bool overTimeline = (mousePos.x >= windowPos.x &&
                         mousePos.x <= windowPos.x + windowSize.x &&
                         mousePos.y >= windowPos.y);

    // Handle Alt + Mouse Wheel for zoom
    if (overTimeline && io.MouseWheel != 0.0f && io.KeyAlt) {
        float zoomFactor = io.MouseWheel > 0.0f ? 1.2f : 0.8f;
        float newZoom = m_pixelsPerSecond * zoomFactor;

        // Clamp zoom to reasonable range
        newZoom = std::max(10.0f, std::min(500.0f, newZoom));

        m_pixelsPerSecond = newZoom;
    }

    // Check if mouse is over the time ruler
    bool overRuler = (mousePos.x >= windowPos.x &&
                      mousePos.x <= windowPos.x + windowSize.x &&
                      mousePos.y >= windowPos.y &&
                      mousePos.y <= windowPos.y + RULER_HEIGHT);

    // Handle ruler click and drag for seeking/scrubbing
    if (overRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_isDraggingRuler = true;
        float relativeX = mousePos.x - windowPos.x;
        Timecode newTime = pixelToTime(relativeX);
        m_timeline->seek(newTime);
    }

    if (m_isDraggingRuler) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Continue dragging - update time as mouse moves
            float relativeX = mousePos.x - windowPos.x;
            Timecode newTime = pixelToTime(relativeX);
            m_timeline->seek(newTime);
        } else {
            // Mouse released - stop dragging
            m_isDraggingRuler = false;
        }
    }

    // Handle clip dragging
    if (m_isDraggingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate new clip position based on mouse
            float relativeX = mousePos.x - windowPos.x - m_dragOffsetX;
            Timecode newStartTime = pixelToTime(relativeX);

            // Clamp to valid range (>= 0)
            if (newStartTime < 0) newStartTime = 0;

            // Update clip's start frame
            auto& registry = m_timeline->getRegistry();
            if (registry.valid(m_selectedClip)) {
                auto* clip = registry.try_get<Clip>(m_selectedClip);
                if (clip) {
                    float newStartSeconds = newStartTime / 1000.0f;
                    clip->startFrame = static_cast<FrameNumber>(newStartSeconds * clip->framerate);
                }
            }
        } else {
            // Mouse released - stop dragging
            m_isDraggingClip = false;
        }
    } else if (!m_isDraggingRuler) {
        // Only check for clip selection if not dragging ruler or clip
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int trackIndex = -1;
            entt::entity clipUnderMouse = findClipAtPosition(mousePos, windowPos, trackIndex);

            if (clipUnderMouse != entt::null) {
                // Start dragging this clip
                m_selectedClip = clipUnderMouse;
                m_isDraggingClip = true;

                // Calculate drag offset (where in the clip the user clicked)
                auto& registry = m_timeline->getRegistry();
                if (registry.valid(clipUnderMouse)) {
                    auto* clip = registry.try_get<Clip>(clipUnderMouse);
                    if (clip) {
                        float startSeconds = clip->startFrame / clip->framerate;
                        Timecode clipStartTime = static_cast<Timecode>(startSeconds * 1000.0f);
                        float clipX = windowPos.x + timeToPixel(clipStartTime);
                        m_dragOffsetX = mousePos.x - clipX;
                        m_clipDragStartTime = clipStartTime;
                    }
                }
            } else {
                // Clicked on empty space - deselect
                m_selectedClip = entt::null;
            }
        }
    }
}

entt::entity TimelineWidget::findClipAtPosition(ImVec2 mousePos, ImVec2 windowPos, int& outTrackIndex) {
    if (!m_timeline) return entt::null;

    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();

    // Check each track
    for (size_t i = 0; i < tracks.size(); ++i) {
        entt::entity trackEntity = tracks[i];
        const auto* track = registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;

        // Calculate track Y bounds
        float trackY = windowPos.y + RULER_HEIGHT + i * (TRACK_HEIGHT + TRACK_PADDING);

        // Check if mouse Y is within this track
        if (mousePos.y < trackY || mousePos.y > trackY + TRACK_HEIGHT) {
            continue;
        }

        // Check each clip in this track
        for (entt::entity clipEntity : track->clips) {
            const auto* clip = registry.try_get<Clip>(clipEntity);
            if (!clip) continue;

            // Calculate clip bounds
            float startSeconds = clip->startFrame / clip->framerate;
            float durationSeconds = clip->duration / clip->framerate;
            Timecode startTime = static_cast<Timecode>(startSeconds * 1000.0f);
            Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000.0f);

            float clipX = windowPos.x + timeToPixel(startTime);
            float clipWidth = timeToPixel(endTime - startTime);

            // Check if mouse is within clip bounds
            if (mousePos.x >= clipX && mousePos.x <= clipX + clipWidth) {
                outTrackIndex = static_cast<int>(i);
                return clipEntity;
            }
        }
    }

    outTrackIndex = -1;
    return entt::null;
}

float TimelineWidget::timeToPixel(Timecode time) const {
    float seconds = time / 1000.0f;
    return seconds * m_pixelsPerSecond;
}

Timecode TimelineWidget::pixelToTime(float pixel) const {
    float seconds = pixel / m_pixelsPerSecond;
    return static_cast<Timecode>(seconds * 1000.0f);
}

} // namespace entity
