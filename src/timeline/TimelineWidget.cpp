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

    // Note: Window Begin/End is now handled by TimelineWindow wrapper
    // This render() method only renders the timeline content

    ImVec2 contentRegion = ImGui::GetContentRegionAvail();

    // Calculate timeline dimensions
    int trackCount = static_cast<int>(m_timeline->getTrackCount());
    float tracksHeight = trackCount * (TRACK_HEIGHT + TRACK_PADDING);
    float durationSeconds = m_timeline->getDuration() / 1000.0f;
    float timelineWidth = durationSeconds * m_pixelsPerSecond;

    // Reserve space for controls at bottom (approximately 40 pixels)
    float controlsHeight = 40.0f;
    float availableHeight = contentRegion.y - controlsHeight;

    // === STICKY RULER (Fixed at top) ===
    ImGui::BeginChild("TimelineRuler", ImVec2(0, RULER_HEIGHT), false,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Set scroll to synced position
    ImGui::SetScrollX(m_syncScrollX);

    // Save ruler position for playhead rendering
    m_rulerScreenPos = ImGui::GetCursorScreenPos();

    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##rulerArea", ImVec2(timelineWidth, RULER_HEIGHT));
    ImGui::SetCursorPos(ImVec2(0, 0));

    renderTimeRuler();
    handleRulerInteraction();

    ImGui::EndChild();

    // === SCROLLABLE TRACKS (Vertically scrollable) ===
    float tracksWindowHeight = availableHeight - RULER_HEIGHT;
    ImGui::BeginChild("TimelineTracks", ImVec2(0, tracksWindowHeight), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // Set scroll to synced position
    ImGui::SetScrollX(m_syncScrollX);

    // Save tracks position for playhead rendering
    m_tracksScreenPos = ImGui::GetCursorScreenPos();

    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##tracksArea", ImVec2(timelineWidth, tracksHeight));
    ImGui::SetCursorPos(ImVec2(0, 0));

    renderTracks();
    handleTracksInteraction();

    // Save scroll position for next frame (from whichever window was scrolled)
    m_syncScrollX = ImGui::GetScrollX();
    m_tracksHeight = tracksHeight;

    ImGui::EndChild();

    // Render playhead (spans across both ruler and tracks)
    renderPlayhead();

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

    // Note: ImGui::End() is now handled by TimelineWindow wrapper
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

    // Calculate track bounds (no RULER_HEIGHT offset - tracks are in separate child window)
    float trackY = windowPos.y + trackIndex * (TRACK_HEIGHT + TRACK_PADDING);
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

    // Calculate clip position and size (no RULER_HEIGHT offset - clips are in tracks child window)
    float trackY = windowPos.y + trackIndex * (TRACK_HEIGHT + TRACK_PADDING);

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

    // Use foreground draw list to render on top of child windows
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    // Calculate playhead X position (accounting for scroll)
    Timecode currentTime = m_timeline->getCurrentTime();
    float playheadPixel = timeToPixel(currentTime) - m_syncScrollX;

    // Playhead X in ruler space
    float playheadXRuler = m_rulerScreenPos.x + playheadPixel;

    // Playhead X in tracks space
    float playheadXTracks = m_tracksScreenPos.x + playheadPixel;

    // Draw playhead line spanning from ruler through tracks
    drawList->AddLine(
        ImVec2(playheadXRuler, m_rulerScreenPos.y),
        ImVec2(playheadXTracks, m_tracksScreenPos.y + m_tracksHeight),
        IM_COL32(255, 100, 100, 255),
        2.0f
    );

    // Draw playhead triangle at top of ruler
    ImVec2 trianglePoints[3] = {
        ImVec2(playheadXRuler, m_rulerScreenPos.y),
        ImVec2(playheadXRuler - 6.0f, m_rulerScreenPos.y + 10.0f),
        ImVec2(playheadXRuler + 6.0f, m_rulerScreenPos.y + 10.0f)
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

        // Calculate track Y bounds (no RULER_HEIGHT - called from tracks child window)
        float trackY = windowPos.y + i * (TRACK_HEIGHT + TRACK_PADDING);

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

void TimelineWidget::handleRulerInteraction() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImVec2 mousePos = ImGui::GetMousePos();
    ImGuiIO& io = ImGui::GetIO();

    // Check if mouse is over the ruler window
    bool overRuler = (mousePos.x >= windowPos.x &&
                      mousePos.x <= windowPos.x + windowSize.x &&
                      mousePos.y >= windowPos.y &&
                      mousePos.y <= windowPos.y + RULER_HEIGHT);

    // Handle Alt + Mouse Wheel for zoom
    if (overRuler && io.MouseWheel != 0.0f && io.KeyAlt) {
        float zoomFactor = io.MouseWheel > 0.0f ? 1.2f : 0.8f;
        float newZoom = m_pixelsPerSecond * zoomFactor;

        // Clamp zoom to reasonable range
        newZoom = std::max(10.0f, std::min(500.0f, newZoom));

        m_pixelsPerSecond = newZoom;
    }

    // Handle ruler click and drag for seeking/scrubbing
    if (overRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_isDraggingRuler = true;
        float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
        Timecode newTime = pixelToTime(relativeX);
        m_timeline->seek(newTime);
    }

    if (m_isDraggingRuler) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Continue dragging - update time as mouse moves
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
            Timecode newTime = pixelToTime(relativeX);
            m_timeline->seek(newTime);
        } else {
            // Mouse released - stop dragging
            m_isDraggingRuler = false;
        }
    }
}

void TimelineWidget::handleTracksInteraction() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 mousePos = ImGui::GetMousePos();

    // Handle clip dragging
    if (m_isDraggingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate desired new clip position based on mouse
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX - m_dragOffsetX;
            Timecode desiredStartTime = pixelToTime(relativeX);

            // Clamp to valid range (>= 0)
            if (desiredStartTime < 0) desiredStartTime = 0;

            // Check for collisions and snap to valid position
            Timecode newStartTime = checkClipCollision(m_selectedClip, desiredStartTime, m_selectedClipTrackIndex);

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
            m_selectedClipTrackIndex = -1;
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
                m_selectedClipTrackIndex = trackIndex;

                // Calculate drag offset (where in the clip the user clicked)
                auto& registry = m_timeline->getRegistry();
                if (registry.valid(clipUnderMouse)) {
                    auto* clip = registry.try_get<Clip>(clipUnderMouse);
                    if (clip) {
                        float startSeconds = clip->startFrame / clip->framerate;
                        Timecode clipStartTime = static_cast<Timecode>(startSeconds * 1000.0f);
                        float clipX = windowPos.x + timeToPixel(clipStartTime) - m_syncScrollX;
                        m_dragOffsetX = mousePos.x - clipX;
                        m_clipDragStartTime = clipStartTime;
                    }
                }
            } else {
                // Clicked on empty space - deselect
                m_selectedClip = entt::null;
                m_selectedClipTrackIndex = -1;
            }
        }
    }
}

Timecode TimelineWidget::checkClipCollision(entt::entity clipEntity, Timecode newStartTime, int trackIndex) {
    if (!m_timeline) return newStartTime;

    auto& registry = m_timeline->getRegistry();
    const auto* movingClip = registry.try_get<Clip>(clipEntity);
    if (!movingClip) return newStartTime;

    // Calculate moving clip's time range at new position
    float durationSeconds = movingClip->duration / movingClip->framerate;
    Timecode newEndTime = newStartTime + static_cast<Timecode>(durationSeconds * 1000.0f);

    // Get the track
    const auto& tracks = m_timeline->getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) {
        return newStartTime;
    }

    entt::entity trackEntity = tracks[trackIndex];
    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return newStartTime;

    // Check for collisions with other clips on the same track
    Timecode snapPosition = newStartTime;
    float minDistance = std::numeric_limits<float>::max();

    for (entt::entity otherClipEntity : track->clips) {
        // Skip the clip we're moving
        if (otherClipEntity == clipEntity) continue;

        const auto* otherClip = registry.try_get<Clip>(otherClipEntity);
        if (!otherClip) continue;

        // Calculate other clip's time range
        float otherStartSeconds = otherClip->startFrame / otherClip->framerate;
        float otherDurationSeconds = otherClip->duration / otherClip->framerate;
        Timecode otherStartTime = static_cast<Timecode>(otherStartSeconds * 1000.0f);
        Timecode otherEndTime = otherStartTime + static_cast<Timecode>(otherDurationSeconds * 1000.0f);

        // Check for overlap
        bool overlaps = (newStartTime < otherEndTime && newEndTime > otherStartTime);

        if (overlaps) {
            // Calculate snap positions (before or after the other clip)
            Timecode snapBefore = otherStartTime - static_cast<Timecode>(durationSeconds * 1000.0f);
            Timecode snapAfter = otherEndTime;

            // Choose the snap position closest to the desired position
            float distBefore = std::abs(static_cast<float>(snapBefore - newStartTime));
            float distAfter = std::abs(static_cast<float>(snapAfter - newStartTime));

            Timecode candidateSnap = (distBefore < distAfter) ? snapBefore : snapAfter;
            float candidateDistance = std::min(distBefore, distAfter);

            // Keep track of the closest valid position
            if (candidateDistance < minDistance) {
                minDistance = candidateDistance;
                snapPosition = candidateSnap;
            }
        }
    }

    // Clamp snap position to valid range (>= 0)
    if (snapPosition < 0) snapPosition = 0;

    return snapPosition;
}

} // namespace entity
