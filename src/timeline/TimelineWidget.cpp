/**
 * TimelineWidget Implementation
 *
 * ImGui-based widget for rendering and interacting with the timeline.
 */

#include "entity/timeline/TimelineWidget.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/media/FrameRingBuffer.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>

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
    float durationSeconds = m_timeline->getDuration() / 1000000.0f;
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
    float currentSeconds = currentTime / 1000000.0f;
    int minutes = static_cast<int>(currentSeconds / 60.0f);
    float seconds = currentSeconds - (minutes * 60.0f);
    ImGui::Text("Time: %02d:%06.3f", minutes, seconds);

    ImGui::SameLine();

    // Zoom controls
    ImGui::Text("Zoom:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::SliderFloat("##zoom", &m_pixelsPerSecond, 10.0f, 500.0f, "%.0f px/s");

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Add Track button
    if (ImGui::Button("+ Add Track")) {
        std::ostringstream trackName;
        trackName << "Video Track " << (m_timeline->getTrackCount() + 1);
        m_timeline->createTrack(trackName.str());
    }

    // Buffer status indicator for selected clip
    if (m_selectedClip != entt::null) {
        auto& registry = m_timeline->getRegistry();
        auto* frameBuffer = registry.try_get<FrameBuffer>(m_selectedClip);
        if (frameBuffer && frameBuffer->ringBuffer) {
            ImGui::SameLine();
            ImGui::Separator();
            ImGui::SameLine();

            uint32_t bufferedFrames = frameBuffer->ringBuffer->getCount();
            uint32_t capacity = frameBuffer->ringBuffer->getCapacity();
            float fillPct = frameBuffer->ringBuffer->getFillPercentage();

            // Color based on buffer level
            ImVec4 bufferColor;
            if (fillPct > 0.5f) {
                bufferColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);  // Green - healthy
            } else if (fillPct > 0.2f) {
                bufferColor = ImVec4(0.8f, 0.8f, 0.2f, 1.0f);  // Yellow - low
            } else {
                bufferColor = ImVec4(0.8f, 0.2f, 0.2f, 1.0f);  // Red - critical
            }

            ImGui::TextColored(bufferColor, "Buffer: %u/%u (%.0f%%)",
                bufferedFrames, capacity, fillPct * 100.0f);
        }
    }

    // Handle context menus
    handleContextMenus();

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
    float durationSeconds = m_timeline->getDuration() / 1000000.0f;

    // Determine marker interval based on zoom
    float markerIntervalSeconds = 1.0f;
    if (m_pixelsPerSecond < 50.0f) {
        markerIntervalSeconds = 5.0f;
    } else if (m_pixelsPerSecond > 200.0f) {
        markerIntervalSeconds = 0.5f;
    }

    // Draw markers
    for (float t = 0.0f; t <= durationSeconds; t += markerIntervalSeconds) {
        float x = windowPos.x + timeToPixel(static_cast<Timecode>(t * 1000000.0f));

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

    // Get base window position ONCE at the start
    // This prevents cursor drift from InvisibleButtons affecting subsequent tracks
    ImVec2 baseWindowPos = ImGui::GetCursorScreenPos();

    const auto& tracks = m_timeline->getTracks();
    for (size_t i = 0; i < tracks.size(); ++i) {
        renderTrack(tracks[i], static_cast<int>(i), baseWindowPos);
    }
}

void TimelineWidget::renderTrack(entt::entity trackEntity, int trackIndex, ImVec2 baseWindowPos) {
    if (!m_timeline) return;

    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto& registry = m_timeline->getRegistry();
    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return;

    // Calculate track bounds using base window position (prevents cursor drift)
    float trackY = baseWindowPos.y + trackIndex * (TRACK_HEIGHT + TRACK_PADDING);
    ImVec2 trackMin = ImVec2(baseWindowPos.x, trackY);
    ImVec2 trackMax = ImVec2(baseWindowPos.x + windowSize.x, trackY + TRACK_HEIGHT);

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

    // Render clips in this track FIRST (so we can handle clip clicks before drag-drop)
    for (entt::entity clipEntity : track->clips) {
        renderClip(clipEntity, trackIndex, baseWindowPos);
    }

    // Handle drag-drop target for this track AFTER rendering clips
    // Use InvisibleButton with AllowOverlap to not block clip interaction
    ImGui::SetCursorScreenPos(trackMin);
    std::ostringstream dropTargetId;
    dropTargetId << "##trackDropTarget" << trackIndex;

    // Allow this button to overlap with other items (clip rendering uses draw list, not ImGui widgets)
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton(dropTargetId.str().c_str(), ImVec2(trackMax.x - trackMin.x, TRACK_HEIGHT));

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_FILE")) {
            // Extract the file path from the payload
            const char* droppedPath = static_cast<const char*>(payload->Data);
            std::string filepath(droppedPath);

            // Calculate the drop position in timeline time
            ImVec2 mousePos = ImGui::GetMousePos();
            float relativeX = mousePos.x - baseWindowPos.x + m_syncScrollX;
            Timecode dropTime = pixelToTime(relativeX);
            if (dropTime < 0) dropTime = 0;

            // Call the callback if set
            if (m_mediaDropCallback) {
                m_mediaDropCallback(filepath, trackIndex, dropTime);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void TimelineWidget::renderClip(entt::entity clipEntity, int trackIndex, ImVec2 baseWindowPos) {
    if (!m_timeline) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto& registry = m_timeline->getRegistry();
    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) return;

    // Calculate clip position and size using base window position (prevents cursor drift)
    float trackY = baseWindowPos.y + trackIndex * (TRACK_HEIGHT + TRACK_PADDING);

    // Convert frame timing to microseconds (Timecode units)
    float startSeconds = clip->startFrame / clip->framerate;
    float durationSeconds = clip->duration / clip->framerate;
    Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
    Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

    float clipX = baseWindowPos.x + timeToPixel(startTime);
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

    // Draw buffer progress bar at bottom of clip
    const auto* frameBuffer = registry.try_get<FrameBuffer>(clipEntity);
    if (frameBuffer && frameBuffer->ringBuffer) {
        float fillPct = frameBuffer->ringBuffer->getFillPercentage();

        // Progress bar dimensions
        float barHeight = 4.0f;
        float barY = clipMax.y - barHeight - 2.0f;
        float barWidth = clipWidth - 6.0f;

        // Background bar
        ImVec2 barMin(clipMin.x + 3.0f, barY);
        ImVec2 barMax(clipMin.x + 3.0f + barWidth, barY + barHeight);
        drawList->AddRectFilled(barMin, barMax, IM_COL32(30, 30, 30, 200), 2.0f);

        // Fill bar (color based on level)
        ImU32 fillColor;
        if (fillPct > 0.5f) {
            fillColor = IM_COL32(50, 200, 50, 255);  // Green
        } else if (fillPct > 0.2f) {
            fillColor = IM_COL32(200, 200, 50, 255); // Yellow
        } else {
            fillColor = IM_COL32(200, 50, 50, 255);  // Red
        }

        ImVec2 fillMax(barMin.x + barWidth * fillPct, barMax.y);
        if (fillPct > 0.01f) {
            drawList->AddRectFilled(barMin, fillMax, fillColor, 2.0f);
        }
    }

    // Draw trim handles at clip edges
    // Show handles when clip is selected or when actively trimming this clip
    bool showHandles = isSelected || (m_isTrimmingClip && m_trimClip == clipEntity);

    if (showHandles && clipWidth > TRIM_EDGE_WIDTH * 4) {
        // Handle dimensions
        float handleWidth = TRIM_EDGE_WIDTH - 2.0f;
        float handleHeight = TRACK_HEIGHT - CLIP_PADDING * 4;
        float handleY = clipMin.y + (TRACK_HEIGHT - CLIP_PADDING * 2 - handleHeight) / 2;

        // Left trim handle
        ImVec2 leftHandleMin(clipMin.x + 1.0f, handleY);
        ImVec2 leftHandleMax(clipMin.x + 1.0f + handleWidth, handleY + handleHeight);

        // Highlight if actively trimming this edge
        ImU32 leftHandleColor = (m_isTrimmingClip && m_trimClip == clipEntity && m_trimEdge == ClipEdge::Left)
            ? IM_COL32(255, 200, 100, 255)   // Orange when trimming
            : IM_COL32(200, 200, 200, 180);  // Gray normally

        drawList->AddRectFilled(leftHandleMin, leftHandleMax, leftHandleColor, 2.0f);
        drawList->AddRect(leftHandleMin, leftHandleMax, IM_COL32(255, 255, 255, 200), 2.0f);

        // Right trim handle
        ImVec2 rightHandleMin(clipMax.x - 1.0f - handleWidth, handleY);
        ImVec2 rightHandleMax(clipMax.x - 1.0f, handleY + handleHeight);

        ImU32 rightHandleColor = (m_isTrimmingClip && m_trimClip == clipEntity && m_trimEdge == ClipEdge::Right)
            ? IM_COL32(255, 200, 100, 255)   // Orange when trimming
            : IM_COL32(200, 200, 200, 180);  // Gray normally

        drawList->AddRectFilled(rightHandleMin, rightHandleMax, rightHandleColor, 2.0f);
        drawList->AddRect(rightHandleMin, rightHandleMax, IM_COL32(255, 255, 255, 200), 2.0f);
    }
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

    // Draw snap indicator when actively snapping
    if (m_isSnapping) {
        float snapPixel = timeToPixel(m_snapTargetTime) - m_syncScrollX;
        float snapXRuler = m_rulerScreenPos.x + snapPixel;
        float snapXTracks = m_tracksScreenPos.x + snapPixel;

        // Draw bright cyan snap line (wider than playhead)
        drawList->AddLine(
            ImVec2(snapXRuler, m_rulerScreenPos.y),
            ImVec2(snapXTracks, m_tracksScreenPos.y + m_tracksHeight),
            IM_COL32(0, 255, 255, 200),  // Cyan color
            3.0f
        );

        // Draw small diamonds at top and bottom of snap line
        float diamondSize = 5.0f;
        ImVec2 topDiamond[4] = {
            ImVec2(snapXRuler, m_rulerScreenPos.y - diamondSize),
            ImVec2(snapXRuler - diamondSize, m_rulerScreenPos.y),
            ImVec2(snapXRuler, m_rulerScreenPos.y + diamondSize),
            ImVec2(snapXRuler + diamondSize, m_rulerScreenPos.y)
        };
        drawList->AddQuadFilled(topDiamond[0], topDiamond[1], topDiamond[2], topDiamond[3],
                                IM_COL32(0, 255, 255, 255));
    }
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
                    float newStartSeconds = newStartTime / 1000000.0f;
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
                        Timecode clipStartTime = static_cast<Timecode>(startSeconds * 1000000.0f);
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
            Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
            Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

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

ClipEdge TimelineWidget::findClipEdgeAtPosition(ImVec2 mousePos, ImVec2 windowPos, entt::entity& outClip, int& outTrackIndex) {
    if (!m_timeline) {
        outClip = entt::null;
        outTrackIndex = -1;
        return ClipEdge::None;
    }

    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();

    // Check each track
    for (size_t i = 0; i < tracks.size(); ++i) {
        entt::entity trackEntity = tracks[i];
        const auto* track = registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;

        // Calculate track Y bounds
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
            float startSeconds = clip->startFrame / static_cast<float>(clip->framerate);
            float durationSeconds = clip->duration / static_cast<float>(clip->framerate);
            Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
            Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

            float clipX = windowPos.x + timeToPixel(startTime) - m_syncScrollX;
            float clipWidth = timeToPixel(endTime - startTime);
            float clipEndX = clipX + clipWidth;

            // Check if mouse is near the left edge
            if (mousePos.x >= clipX - TRIM_EDGE_WIDTH / 2 && mousePos.x <= clipX + TRIM_EDGE_WIDTH / 2) {
                outClip = clipEntity;
                outTrackIndex = static_cast<int>(i);
                return ClipEdge::Left;
            }

            // Check if mouse is near the right edge
            if (mousePos.x >= clipEndX - TRIM_EDGE_WIDTH / 2 && mousePos.x <= clipEndX + TRIM_EDGE_WIDTH / 2) {
                outClip = clipEntity;
                outTrackIndex = static_cast<int>(i);
                return ClipEdge::Right;
            }
        }
    }

    outClip = entt::null;
    outTrackIndex = -1;
    return ClipEdge::None;
}

float TimelineWidget::timeToPixel(Timecode time) const {
    float seconds = time / 1000000.0f;  // Timecode is in microseconds
    return seconds * m_pixelsPerSecond;
}

Timecode TimelineWidget::pixelToTime(float pixel) const {
    float seconds = pixel / m_pixelsPerSecond;
    return static_cast<Timecode>(seconds * 1000000.0f);  // Timecode is in microseconds
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

    // Use cached tracks position (saved in render() before cursor is moved by widgets)
    ImVec2 windowPos = m_tracksScreenPos;
    ImVec2 mousePos = ImGui::GetMousePos();

    // Only handle new interactions if this child window is hovered
    bool isWindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    auto& registry = m_timeline->getRegistry();

    // Handle clip trimming (priority over dragging)
    if (m_isTrimmingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate mouse position in timeline time
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
            Timecode mouseTime = pixelToTime(relativeX);
            if (mouseTime < 0) mouseTime = 0;

            // Get the clip being trimmed
            if (registry.valid(m_trimClip)) {
                auto* clip = registry.try_get<Clip>(m_trimClip);
                if (clip) {
                    FrameNumber mouseFrame = static_cast<FrameNumber>(mouseTime / 1000000.0f * clip->framerate);

                    // Find the track this clip is on for collision detection
                    int trimTrackIndex = -1;
                    const auto& tracks = m_timeline->getTracks();
                    for (size_t i = 0; i < tracks.size(); ++i) {
                        const auto* track = registry.try_get<TimelineTrack>(tracks[i]);
                        if (track) {
                            for (entt::entity clipEnt : track->clips) {
                                if (clipEnt == m_trimClip) {
                                    trimTrackIndex = static_cast<int>(i);
                                    break;
                                }
                            }
                        }
                        if (trimTrackIndex >= 0) break;
                    }

                    if (m_trimEdge == ClipEdge::Left) {
                        // Trim left edge - adjust start and duration
                        // Don't allow trimming past original end
                        FrameNumber originalEnd = m_trimOriginalStart + m_trimOriginalDuration;
                        if (mouseFrame < originalEnd - 1) {  // Keep at least 1 frame
                            // Don't allow trimming before 0
                            if (mouseFrame < 0) mouseFrame = 0;

                            // Check for collision with previous clip
                            FrameNumber minStartFrame = 0;
                            if (trimTrackIndex >= 0 && trimTrackIndex < static_cast<int>(tracks.size())) {
                                const auto* track = registry.try_get<TimelineTrack>(tracks[trimTrackIndex]);
                                if (track) {
                                    for (entt::entity otherClip : track->clips) {
                                        if (otherClip == m_trimClip) continue;
                                        const auto* other = registry.try_get<Clip>(otherClip);
                                        if (other) {
                                            FrameNumber otherEnd = other->startFrame + other->duration;
                                            // If other clip ends before our original start, it could limit us
                                            if (otherEnd <= m_trimOriginalStart && otherEnd > minStartFrame) {
                                                minStartFrame = otherEnd;
                                            }
                                        }
                                    }
                                }
                            }

                            // Clamp to minimum (collision boundary)
                            if (mouseFrame < minStartFrame) {
                                mouseFrame = minStartFrame;
                            }

                            FrameNumber framesDelta = mouseFrame - m_trimOriginalStart;
                            clip->startFrame = mouseFrame;
                            clip->duration = m_trimOriginalDuration - framesDelta;

                            // Adjust media start to keep sync
                            clip->mediaStartFrame = m_trimOriginalMediaStart + framesDelta;
                        }
                    } else if (m_trimEdge == ClipEdge::Right) {
                        // Trim right edge - adjust duration only
                        FrameNumber newDuration = mouseFrame - clip->startFrame;
                        if (newDuration >= 1) {  // Keep at least 1 frame
                            // Check for collision with next clip
                            FrameNumber maxEndFrame = std::numeric_limits<FrameNumber>::max();
                            if (trimTrackIndex >= 0 && trimTrackIndex < static_cast<int>(tracks.size())) {
                                const auto* track = registry.try_get<TimelineTrack>(tracks[trimTrackIndex]);
                                if (track) {
                                    for (entt::entity otherClip : track->clips) {
                                        if (otherClip == m_trimClip) continue;
                                        const auto* other = registry.try_get<Clip>(otherClip);
                                        if (other) {
                                            // If other clip starts after our start, it could limit us
                                            if (other->startFrame > clip->startFrame && other->startFrame < maxEndFrame) {
                                                maxEndFrame = other->startFrame;
                                            }
                                        }
                                    }
                                }
                            }

                            // Clamp duration to not exceed collision boundary
                            FrameNumber maxDuration = maxEndFrame - clip->startFrame;
                            if (newDuration > maxDuration) {
                                newDuration = maxDuration;
                            }

                            clip->duration = newDuration;
                        }
                    }
                }
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else {
            // Mouse released - stop trimming
            m_isTrimmingClip = false;
            m_trimEdge = ClipEdge::None;
            m_trimClip = entt::null;
        }
        return;  // Don't process other interactions while trimming
    }

    // Handle clip dragging (continue even if not hovered, to allow drag outside window)
    if (m_isDraggingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate desired new clip position based on mouse
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX - m_dragOffsetX;
            Timecode desiredStartTime = pixelToTime(relativeX);

            // Clamp to valid range (>= 0)
            if (desiredStartTime < 0) desiredStartTime = 0;

            // Snap to playhead and other clips if enabled
            m_isSnapping = false;  // Reset snap state
            if (m_snappingEnabled && registry.valid(m_selectedClip)) {
                auto* clip = registry.try_get<Clip>(m_selectedClip);
                if (clip) {
                    Timecode playheadTime = m_timeline->getCurrentTime();
                    float durationSeconds = clip->duration / static_cast<float>(clip->framerate);
                    Timecode clipDuration = static_cast<Timecode>(durationSeconds * 1000000.0f);
                    Timecode desiredEndTime = desiredStartTime + clipDuration;

                    // Convert snap threshold from pixels to time
                    Timecode snapThresholdTime = pixelToTime(SNAP_THRESHOLD_PIXELS);

                    // Track best snap candidate
                    Timecode bestSnapTime = 0;
                    Timecode bestSnapDistance = snapThresholdTime + 1;  // Start with invalid distance
                    bool snapToStart = true;  // Whether to snap clip start (true) or end (false)

                    // Check snap to playhead - start edge
                    Timecode distToPlayheadStart = std::abs(desiredStartTime - playheadTime);
                    if (distToPlayheadStart < bestSnapDistance) {
                        bestSnapDistance = distToPlayheadStart;
                        bestSnapTime = playheadTime;
                        snapToStart = true;
                    }

                    // Check snap to playhead - end edge
                    Timecode distToPlayheadEnd = std::abs(desiredEndTime - playheadTime);
                    if (distToPlayheadEnd < bestSnapDistance) {
                        bestSnapDistance = distToPlayheadEnd;
                        bestSnapTime = playheadTime;
                        snapToStart = false;
                    }

                    // Check snap to other clips on the same track
                    if (m_selectedClipTrackIndex >= 0) {
                        const auto& tracks = m_timeline->getTracks();
                        if (m_selectedClipTrackIndex < static_cast<int>(tracks.size())) {
                            const auto* track = registry.try_get<TimelineTrack>(tracks[m_selectedClipTrackIndex]);
                            if (track) {
                                for (entt::entity otherClipEntity : track->clips) {
                                    if (otherClipEntity == m_selectedClip) continue;

                                    const auto* otherClip = registry.try_get<Clip>(otherClipEntity);
                                    if (!otherClip) continue;

                                    float otherStartSec = otherClip->startFrame / static_cast<float>(otherClip->framerate);
                                    float otherDurSec = otherClip->duration / static_cast<float>(otherClip->framerate);
                                    Timecode otherStartTime = static_cast<Timecode>(otherStartSec * 1000000.0f);
                                    Timecode otherEndTime = static_cast<Timecode>((otherStartSec + otherDurSec) * 1000000.0f);

                                    // Our start to other's start
                                    Timecode dist1 = std::abs(desiredStartTime - otherStartTime);
                                    if (dist1 < bestSnapDistance) {
                                        bestSnapDistance = dist1;
                                        bestSnapTime = otherStartTime;
                                        snapToStart = true;
                                    }

                                    // Our start to other's end
                                    Timecode dist2 = std::abs(desiredStartTime - otherEndTime);
                                    if (dist2 < bestSnapDistance) {
                                        bestSnapDistance = dist2;
                                        bestSnapTime = otherEndTime;
                                        snapToStart = true;
                                    }

                                    // Our end to other's start
                                    Timecode dist3 = std::abs(desiredEndTime - otherStartTime);
                                    if (dist3 < bestSnapDistance) {
                                        bestSnapDistance = dist3;
                                        bestSnapTime = otherStartTime;
                                        snapToStart = false;
                                    }

                                    // Our end to other's end
                                    Timecode dist4 = std::abs(desiredEndTime - otherEndTime);
                                    if (dist4 < bestSnapDistance) {
                                        bestSnapDistance = dist4;
                                        bestSnapTime = otherEndTime;
                                        snapToStart = false;
                                    }
                                }
                            }
                        }
                    }

                    // Apply best snap if found
                    if (bestSnapDistance < snapThresholdTime) {
                        if (snapToStart) {
                            desiredStartTime = bestSnapTime;
                        } else {
                            desiredStartTime = bestSnapTime - clipDuration;
                        }
                        if (desiredStartTime < 0) desiredStartTime = 0;
                        m_isSnapping = true;
                        m_snapTargetTime = bestSnapTime;
                    }
                }
            }

            // Check for collisions and snap to valid position
            Timecode newStartTime = checkClipCollision(m_selectedClip, desiredStartTime, m_selectedClipTrackIndex);

            // Update clip's start frame
            if (registry.valid(m_selectedClip)) {
                auto* clip = registry.try_get<Clip>(m_selectedClip);
                if (clip) {
                    float newStartSeconds = newStartTime / 1000000.0f;
                    clip->startFrame = static_cast<FrameNumber>(newStartSeconds * clip->framerate);
                }
            }
        } else {
            // Mouse released - stop dragging
            // Check if we need to move the clip to a different track
            int finalTrackIndex = findTrackAtY(mousePos.y, windowPos.y);
            if (finalTrackIndex >= 0 && finalTrackIndex != m_selectedClipTrackIndex) {
                // Move clip to new track
                m_timeline->moveClipToTrack(m_selectedClip, finalTrackIndex);
                m_selectedClipTrackIndex = finalTrackIndex;
            }

            m_isDraggingClip = false;
            m_selectedClipTrackIndex = -1;
            m_isSnapping = false;  // Clear snap indicator
        }
    } else if (!m_isDraggingRuler && isWindowHovered) {
        // Check for clip edge hover (for trimming cursor)
        entt::entity edgeClip = entt::null;
        int edgeTrackIndex = -1;
        ClipEdge edge = findClipEdgeAtPosition(mousePos, windowPos, edgeClip, edgeTrackIndex);

        if (edge != ClipEdge::None) {
            // Show resize cursor when hovering over clip edge
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            // Start trimming on click
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                m_isTrimmingClip = true;
                m_trimEdge = edge;
                m_trimClip = edgeClip;
                m_selectedClip = edgeClip;
                m_timeline->setSelectedClip(edgeClip);

                // Store original clip state for trim calculations
                auto* clip = registry.try_get<Clip>(edgeClip);
                if (clip) {
                    m_trimOriginalStart = clip->startFrame;
                    m_trimOriginalDuration = clip->duration;
                    m_trimOriginalMediaStart = clip->mediaStartFrame;
                }
                return;
            }
        }

        // Only check for clip selection if not hovering edge
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int trackIndex = -1;
            entt::entity clipUnderMouse = findClipAtPosition(mousePos, windowPos, trackIndex);

            if (clipUnderMouse != entt::null) {
                // Start dragging this clip
                m_selectedClip = clipUnderMouse;
                m_isDraggingClip = true;
                m_selectedClipTrackIndex = trackIndex;

                // Sync selection to Timeline for PropertyWindow
                m_timeline->setSelectedClip(clipUnderMouse);

                // Calculate drag offset (where in the clip the user clicked)
                if (registry.valid(clipUnderMouse)) {
                    auto* clip = registry.try_get<Clip>(clipUnderMouse);
                    if (clip) {
                        float startSeconds = clip->startFrame / clip->framerate;
                        Timecode clipStartTime = static_cast<Timecode>(startSeconds * 1000000.0f);
                        float clipX = windowPos.x + timeToPixel(clipStartTime) - m_syncScrollX;
                        m_dragOffsetX = mousePos.x - clipX;
                        m_clipDragStartTime = clipStartTime;
                    }
                }
            } else {
                // Clicked on empty space within timeline - deselect
                m_selectedClip = entt::null;
                m_selectedClipTrackIndex = -1;

                // Sync deselection to Timeline
                m_timeline->setSelectedClip(entt::null);
            }
        }

        // Handle right-click for context menus (only if window is hovered)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            int trackIndex = -1;
            entt::entity clipUnderMouse = findClipAtPosition(mousePos, windowPos, trackIndex);

            if (clipUnderMouse != entt::null) {
                // Right-clicked on a clip
                m_rightClickedClip = clipUnderMouse;
                m_rightClickedTrackIndex = trackIndex;
                m_showClipContextMenu = true;
                m_showTrackContextMenu = false;
                ImGui::OpenPopup("ClipContextMenu");
            } else {
                // Check if right-clicked on a track (empty area)
                int trackAtY = findTrackAtY(mousePos.y, windowPos.y);
                if (trackAtY >= 0) {
                    m_rightClickedTrackIndex = trackAtY;
                    m_rightClickedClip = entt::null;
                    m_showTrackContextMenu = true;
                    m_showClipContextMenu = false;
                    ImGui::OpenPopup("TrackContextMenu");
                }
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
    Timecode newEndTime = newStartTime + static_cast<Timecode>(durationSeconds * 1000000.0f);

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
        Timecode otherStartTime = static_cast<Timecode>(otherStartSeconds * 1000000.0f);
        Timecode otherEndTime = otherStartTime + static_cast<Timecode>(otherDurationSeconds * 1000000.0f);

        // Check for overlap
        bool overlaps = (newStartTime < otherEndTime && newEndTime > otherStartTime);

        if (overlaps) {
            // Calculate snap positions (before or after the other clip)
            Timecode snapBefore = otherStartTime - static_cast<Timecode>(durationSeconds * 1000000.0f);
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

int TimelineWidget::findTrackAtY(float mouseY, float windowY) const {
    if (!m_timeline) return -1;

    int trackCount = static_cast<int>(m_timeline->getTrackCount());
    for (int i = 0; i < trackCount; ++i) {
        float trackY = windowY + i * (TRACK_HEIGHT + TRACK_PADDING);
        if (mouseY >= trackY && mouseY <= trackY + TRACK_HEIGHT) {
            return i;
        }
    }
    return -1;
}

void TimelineWidget::handleContextMenus() {
    if (!m_timeline) return;

    // Clip context menu
    if (ImGui::BeginPopup("ClipContextMenu")) {
        if (m_rightClickedClip != entt::null) {
            if (ImGui::MenuItem("Delete Clip")) {
                // Clear selection if we're deleting the selected clip
                if (m_selectedClip == m_rightClickedClip) {
                    m_selectedClip = entt::null;
                    m_selectedClipTrackIndex = -1;
                }
                m_timeline->deleteClip(m_rightClickedClip);
                m_rightClickedClip = entt::null;
            }
            ImGui::Separator();

            // Get clip info for display
            auto& registry = m_timeline->getRegistry();
            const auto* clip = registry.try_get<Clip>(m_rightClickedClip);
            if (clip) {
                ImGui::TextDisabled("Duration: %d frames", clip->duration);
                ImGui::TextDisabled("Start: frame %d", clip->startFrame);
            }
        }
        ImGui::EndPopup();
    }

    // Track context menu
    if (ImGui::BeginPopup("TrackContextMenu")) {
        if (m_rightClickedTrackIndex >= 0) {
            std::ostringstream label;
            label << "Delete Track " << (m_rightClickedTrackIndex + 1);
            if (ImGui::MenuItem(label.str().c_str())) {
                const auto& tracks = m_timeline->getTracks();
                if (m_rightClickedTrackIndex < static_cast<int>(tracks.size())) {
                    entt::entity trackToDelete = tracks[m_rightClickedTrackIndex];
                    m_timeline->deleteTrack(trackToDelete);
                }
                m_rightClickedTrackIndex = -1;
            }

            ImGui::Separator();

            // Option to add a new track
            if (ImGui::MenuItem("Add Track Above")) {
                // For now, just add at the end (track ordering would need more work)
                std::ostringstream trackName;
                trackName << "Video Track " << (m_timeline->getTrackCount() + 1);
                m_timeline->createTrack(trackName.str());
            }

            ImGui::Separator();
            ImGui::TextDisabled("Track %d", m_rightClickedTrackIndex + 1);
        }
        ImGui::EndPopup();
    }
}

void TimelineWidget::renderTrackHeaders() {
    // This function is reserved for future use when we implement
    // a separate track header area on the left side of the timeline
}

} // namespace entity
