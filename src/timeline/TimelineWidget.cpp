/**
 * TimelineWidget Implementation
 *
 * ImGui-based widget for rendering and interacting with the timeline.
 */

#include "entity/timeline/TimelineWidget.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/media/FrameRingBuffer.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <set>
#include <limits>

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

    // Clean up stale entity references in expansion state
    {
        auto& reg = m_timeline->getRegistry();
        for (auto it = m_expandedClips.begin(); it != m_expandedClips.end(); ) {
            entt::entity entity = static_cast<entt::entity>(*it);
            if (!reg.valid(entity)) {
                it = m_expandedClips.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = m_expandedTracks.begin(); it != m_expandedTracks.end(); ) {
            entt::entity entity = static_cast<entt::entity>(*it);
            if (!reg.valid(entity)) {
                it = m_expandedTracks.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Note: Window Begin/End is now handled by TimelineWindow wrapper
    // This render() method only renders the timeline content

    ImVec2 contentRegion = ImGui::GetContentRegionAvail();

    // Calculate timeline dimensions
    int trackCount = static_cast<int>(m_timeline->getTrackCount());
    float durationSeconds = m_timeline->getDuration() / 1000000.0f;
    float timelineWidth = durationSeconds * m_pixelsPerSecond;

    // Calculate total tracks content height (accounting for expanded tracks/clips)
    float tracksContentHeight = 0.0f;
    const auto& tracks = m_timeline->getTracks();
    auto& registry = m_timeline->getRegistry();
    for (size_t i = 0; i < tracks.size(); ++i) {
        uint32_t trackId = static_cast<uint32_t>(tracks[i]);
        bool trackExpanded = m_expandedTracks.count(trackId) > 0 || m_timeline->isTrackExpanded(tracks[i]);
        tracksContentHeight += TRACK_HEIGHT + TRACK_PADDING;

        if (trackExpanded) {
            const auto* track = registry.try_get<TimelineTrack>(tracks[i]);
            if (track) {
                for (entt::entity clipEntity : track->clips) {
                    tracksContentHeight += HEADER_ROW_HEIGHT;  // Clip row

                    uint32_t clipId = static_cast<uint32_t>(clipEntity);
                    bool clipExpanded = m_expandedClips.count(clipId) > 0 || m_timeline->isClipExpanded(clipEntity);
                    if (clipExpanded) {
                        tracksContentHeight += 6 * PROPERTY_ROW_HEIGHT;  // 6 property rows
                    }
                }
            }
        }
    }

    // Reserve space for controls at bottom (approximately 40 pixels)
    float controlsHeight = 40.0f;
    float availableHeight = contentRegion.y - controlsHeight;
    float tracksWindowHeight = availableHeight - RULER_HEIGHT;
    float timelineContentWidth = contentRegion.x - TRACK_HEADER_WIDTH - 4.0f;  // 4px spacing

    // === TOP ROW: Header corner + Ruler ===
    // Header corner (empty space above track headers, aligned with ruler)
    ImGui::BeginChild("HeaderCorner", ImVec2(TRACK_HEADER_WIDTH, RULER_HEIGHT), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* cornerDrawList = ImGui::GetWindowDrawList();
    ImVec2 cornerMin = ImGui::GetWindowPos();
    ImVec2 cornerMax(cornerMin.x + TRACK_HEADER_WIDTH, cornerMin.y + RULER_HEIGHT);
    cornerDrawList->AddRectFilled(cornerMin, cornerMax, IM_COL32(35, 35, 40, 255));
    cornerDrawList->AddText(ImVec2(cornerMin.x + 8, cornerMin.y + 8), IM_COL32(150, 150, 150, 255), "Tracks");
    ImGui::EndChild();

    ImGui::SameLine(0, 2.0f);

    // === STICKY RULER (Fixed at top, right side) ===
    ImGui::BeginChild("TimelineRuler", ImVec2(timelineContentWidth, RULER_HEIGHT), false,
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

    // === BOTTOM ROW: Track headers + Tracks content ===
    // Track header panel (left side, syncs vertical scroll only)
    ImGui::BeginChild("TrackHeaders", ImVec2(TRACK_HEADER_WIDTH, tracksWindowHeight), false,
                      ImGuiWindowFlags_NoScrollbar);

    // Create inner scrollable region for header content
    ImGui::BeginChild("TrackHeadersInner", ImVec2(TRACK_HEADER_WIDTH - 2, tracksContentHeight + 100), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Apply synced vertical scroll
    ImGui::SetScrollY(m_syncScrollY);

    renderTrackHeaderPanel(tracksContentHeight, m_syncScrollY);

    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine(0, 2.0f);

    // === SCROLLABLE TRACKS (right side, scrolls both ways) ===
    ImGui::BeginChild("TimelineTracks", ImVec2(timelineContentWidth, tracksWindowHeight), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    // Set scroll to synced position
    ImGui::SetScrollX(m_syncScrollX);
    ImGui::SetScrollY(m_syncScrollY);

    // Save tracks position for playhead rendering
    m_tracksScreenPos = ImGui::GetCursorScreenPos();

    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##tracksArea", ImVec2(timelineWidth, tracksContentHeight + 100));
    ImGui::SetCursorPos(ImVec2(0, 0));

    renderTracks();
    handleTracksInteraction();

    // Save scroll position for next frame (from whichever window was scrolled)
    m_syncScrollX = ImGui::GetScrollX();
    m_syncScrollY = ImGui::GetScrollY();
    m_tracksHeight = tracksContentHeight;

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

    // Display current time in SMPTE timecode format (MM:SS:FF)
    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    double frameRate = m_timeline->getFrameRate();
    int totalSeconds = static_cast<int>(currentFrame / frameRate);
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    int frames = static_cast<int>(currentFrame % static_cast<FrameNumber>(frameRate));
    ImGui::Text("Time: %02d:%02d:%02d", minutes, seconds, frames);
    ImGui::SameLine();
    ImGui::TextDisabled("(Frame %d)", static_cast<int>(currentFrame));

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

    // Get timeline framerate
    double frameRate = m_timeline->getFrameRate();
    float durationSeconds = m_timeline->getDuration() / 1000000.0f;
    FrameNumber totalFrames = static_cast<FrameNumber>(durationSeconds * frameRate);

    // Determine marker interval in frames based on zoom
    // pixelsPerSecond = pixelsPerFrame * frameRate
    float pixelsPerFrame = m_pixelsPerSecond / static_cast<float>(frameRate);

    FrameNumber frameInterval = 30;  // Default: every second at 30fps
    if (pixelsPerFrame < 2.0f) {
        frameInterval = 150;  // Every 5 seconds
    } else if (pixelsPerFrame < 5.0f) {
        frameInterval = 30;   // Every second
    } else if (pixelsPerFrame < 15.0f) {
        frameInterval = 10;   // Every 10 frames
    } else {
        frameInterval = 1;    // Every frame
    }

    // Draw frame markers
    for (FrameNumber frame = 0; frame <= totalFrames; frame += frameInterval) {
        float timeSeconds = static_cast<float>(frame) / static_cast<float>(frameRate);
        float x = windowPos.x + timeToPixel(static_cast<Timecode>(timeSeconds * 1000000.0f));

        if (x < windowPos.x || x > rulerMax.x) continue;

        // Draw tick mark
        bool isMajorTick = (frame % static_cast<FrameNumber>(frameRate) == 0);  // Major tick every second
        float tickHeight = isMajorTick ? 12.0f : 6.0f;
        drawList->AddLine(
            ImVec2(x, rulerMax.y - tickHeight),
            ImVec2(x, rulerMax.y),
            IM_COL32(200, 200, 200, 255),
            1.0f
        );

        // Draw time label (SMPTE timecode: HH:MM:SS:FF)
        if (isMajorTick || frameInterval <= 10) {
            int totalSeconds = static_cast<int>(frame / frameRate);
            int hours = totalSeconds / 3600;
            int minutes = (totalSeconds % 3600) / 60;
            int seconds = totalSeconds % 60;
            int frames = static_cast<int>(frame % static_cast<FrameNumber>(frameRate));

            std::ostringstream oss;
            if (hours > 0) {
                oss << std::setfill('0') << std::setw(2) << hours << ":";
            }
            oss << std::setfill('0') << std::setw(2) << minutes << ":"
                << std::setfill('0') << std::setw(2) << seconds << ":"
                << std::setfill('0') << std::setw(2) << frames;

            drawList->AddText(
                ImVec2(x + 3.0f, rulerMin.y + 5.0f),
                IM_COL32(200, 200, 200, 255),
                oss.str().c_str()
            );
        }
    }
}

void TimelineWidget::renderTracks() {
    if (!m_timeline) return;

    // Get base window position ONCE at the start
    // This prevents cursor drift from InvisibleButtons affecting subsequent tracks
    ImVec2 baseWindowPos = ImGui::GetCursorScreenPos();

    // Calculate cumulative Y offset for tracks to account for expanded tracks/clips
    float cumulativeY = 0.0f;
    const auto& tracks = m_timeline->getTracks();
    auto& registry = m_timeline->getRegistry();

    for (size_t i = 0; i < tracks.size(); ++i) {
        // Calculate track Y position including offset from previous expanded tracks
        float trackY = baseWindowPos.y + cumulativeY;
        ImVec2 trackBasePos(baseWindowPos.x, trackY);

        // Calculate track height (including expanded clips from header hierarchy)
        float trackHeight = TRACK_HEIGHT;
        uint32_t trackId = static_cast<uint32_t>(tracks[i]);
        bool trackExpanded = m_expandedTracks.count(trackId) > 0 || m_timeline->isTrackExpanded(tracks[i]);

        const auto* track = registry.try_get<TimelineTrack>(tracks[i]);

        renderTrack(tracks[i], static_cast<int>(i), trackBasePos, trackHeight);

        // Render expanded clip content (property tracks with keyframe diamonds)
        // This must match the Y positions used in the header panel
        if (trackExpanded && track) {
            // Clip rows start right after track (no padding between track and first clip)
            float clipContentY = cumulativeY + trackHeight;

            for (entt::entity clipEntity : track->clips) {
                // Space for clip header row (no visual on timeline, just spacing)
                clipContentY += HEADER_ROW_HEIGHT;

                uint32_t clipId = static_cast<uint32_t>(clipEntity);
                bool clipExpanded = m_expandedClips.count(clipId) > 0 || m_timeline->isClipExpanded(clipEntity);
                if (clipExpanded) {
                    // Render property tracks (keyframe diamonds) at this Y position
                    float propY = baseWindowPos.y + clipContentY;
                    renderPropertyTracks(clipEntity, static_cast<int>(i), baseWindowPos, propY);
                    clipContentY += 6 * PROPERTY_ROW_HEIGHT;
                }
            }

            // Update cumulative Y to account for all expanded content + padding
            cumulativeY = clipContentY + TRACK_PADDING;
        } else {
            cumulativeY += trackHeight + TRACK_PADDING;
        }
    }
}

void TimelineWidget::renderTrack(entt::entity trackEntity, int trackIndex, ImVec2 baseWindowPos, float trackHeight) {
    if (!m_timeline) return;

    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto& registry = m_timeline->getRegistry();
    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return;

    // Use the provided base position (already accounts for cumulative offset)
    float trackY = baseWindowPos.y;
    ImVec2 trackMin = ImVec2(baseWindowPos.x, trackY);
    ImVec2 trackMax = ImVec2(baseWindowPos.x + windowSize.x, trackY + trackHeight);

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
    ImGui::InvisibleButton(dropTargetId.str().c_str(), ImVec2(trackMax.x - trackMin.x, trackHeight));

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

float TimelineWidget::renderClip(entt::entity clipEntity, int trackIndex, ImVec2 baseWindowPos) {
    (void)trackIndex;  // trackIndex no longer used for Y calculation
    if (!m_timeline) return TRACK_HEIGHT;

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto& registry = m_timeline->getRegistry();
    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) return TRACK_HEIGHT;

    // Check if this clip is expanded (check both local and Timeline state)
    uint32_t entityId = static_cast<uint32_t>(clipEntity);
    bool isExpanded = m_expandedClips.count(entityId) > 0 || m_timeline->isClipExpanded(clipEntity);

    // Use baseWindowPos.y directly - it already includes cumulative offset from renderTracks
    float trackY = baseWindowPos.y;

    // Convert frame timing to microseconds (Timecode units)
    float startSeconds = static_cast<float>(clip->startFrame) / clip->framerate;
    float durationSeconds = static_cast<float>(clip->duration) / clip->framerate;
    Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
    Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

    float clipX = baseWindowPos.x + timeToPixel(startTime);
    float clipWidth = timeToPixel(endTime - startTime);

    ImVec2 clipMin = ImVec2(clipX, trackY + CLIP_PADDING);
    ImVec2 clipMax = ImVec2(clipX + clipWidth, trackY + TRACK_HEIGHT - CLIP_PADDING);

    // Choose clip color based on selection (check both local and Timeline state)
    bool isSelected = (clipEntity == m_selectedClip) || (clipEntity == m_timeline->getSelectedClip());
    ImU32 clipColor = isSelected
        ? IM_COL32(100, 150, 255, 255)
        : IM_COL32(80, 120, 180, 255);

    // Draw clip rectangle
    drawList->AddRectFilled(clipMin, clipMax, clipColor, 3.0f);
    drawList->AddRect(clipMin, clipMax, IM_COL32(120, 160, 220, 255), 3.0f, 0, 2.0f);

    // Draw twirl-down triangle for expansion (only when selected)
    if (isSelected && clipWidth > 30) {
        float triSize = 6.0f;
        float triX = clipMin.x + 8.0f;
        float triY = clipMin.y + 8.0f;

        ImVec2 triPoints[3];
        if (isExpanded) {
            // Down-pointing triangle (expanded)
            triPoints[0] = ImVec2(triX - triSize, triY - triSize/2);
            triPoints[1] = ImVec2(triX + triSize, triY - triSize/2);
            triPoints[2] = ImVec2(triX, triY + triSize/2);
        } else {
            // Right-pointing triangle (collapsed)
            triPoints[0] = ImVec2(triX - triSize/2, triY - triSize);
            triPoints[1] = ImVec2(triX + triSize/2, triY);
            triPoints[2] = ImVec2(triX - triSize/2, triY + triSize);
        }
        drawList->AddTriangleFilled(triPoints[0], triPoints[1], triPoints[2], IM_COL32(255, 255, 255, 200));

        // Handle click on twirl-down
        ImVec2 triHitMin(triX - triSize - 4, triY - triSize - 4);
        ImVec2 triHitMax(triX + triSize + 4, triY + triSize + 4);
        if (ImGui::IsMouseClicked(0)) {
            ImVec2 mousePos = ImGui::GetMousePos();
            if (mousePos.x >= triHitMin.x && mousePos.x <= triHitMax.x &&
                mousePos.y >= triHitMin.y && mousePos.y <= triHitMax.y) {
                if (isExpanded) {
                    m_expandedClips.erase(entityId);
                } else {
                    m_expandedClips.insert(entityId);
                }
            }
        }
    }

    // Draw clip label (filename without path)
    size_t lastSlash = clip->filepath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos)
        ? clip->filepath.substr(lastSlash + 1)
        : clip->filepath;

    // Truncate filename if too long
    if (filename.length() > 20) {
        filename = filename.substr(0, 17) + "...";
    }

    // Offset label if twirl-down is visible
    float labelOffsetX = isSelected ? 20.0f : 5.0f;
    drawList->AddText(
        ImVec2(clipMin.x + labelOffsetX, clipMin.y + 4.0f),
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

    // Draw keyframe markers if clip has animated properties
    const auto* animProps = registry.try_get<AnimatedProperties>(clipEntity);
    if (animProps && animProps->hasAnyKeyframes()) {
        // Keyframe diamond size
        float diamondSize = 4.0f;
        float keyframeY = clipMin.y + 8.0f;  // Position near top of clip

        // Collect all unique keyframe frames
        std::set<FrameNumber> keyframeFrames;
        for (const auto& track : animProps->tracks) {
            for (const auto& kf : track.keyframes) {
                keyframeFrames.insert(kf.frame);
            }
        }

        // Draw diamond marker for each keyframe
        for (FrameNumber kfFrame : keyframeFrames) {
            // Convert keyframe frame (relative to clip) to pixel position
            float kfSeconds = kfFrame / clip->framerate;
            float kfX = clipMin.x + (kfSeconds * m_pixelsPerSecond);

            // Only draw if within clip bounds
            if (kfX >= clipMin.x && kfX <= clipMax.x) {
                // Draw diamond shape
                ImVec2 points[4] = {
                    ImVec2(kfX, keyframeY - diamondSize),           // Top
                    ImVec2(kfX + diamondSize, keyframeY),           // Right
                    ImVec2(kfX, keyframeY + diamondSize),           // Bottom
                    ImVec2(kfX - diamondSize, keyframeY)            // Left
                };

                // Gold color for keyframes
                drawList->AddConvexPolyFilled(points, 4, IM_COL32(255, 200, 50, 255));
                drawList->AddPolyline(points, 4, IM_COL32(255, 255, 255, 200), ImDrawFlags_Closed, 1.0f);
            }
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

    // Property tracks are now rendered from renderTracks() to align with header panel
    return TRACK_HEIGHT;
}

float TimelineWidget::renderPropertyTracks(entt::entity clipEntity, int trackIndex, ImVec2 baseWindowPos, float startY) {
    (void)trackIndex;  // Unused parameter
    if (!m_timeline) return 0.0f;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    auto& registry = m_timeline->getRegistry();

    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) return 0.0f;

    // Get AnimatedProperties for keyframe data
    auto* animProps = registry.try_get<AnimatedProperties>(clipEntity);

    // Calculate clip timing for positioning keyframes
    float startSeconds = static_cast<float>(clip->startFrame) / clip->framerate;
    float durationSeconds = static_cast<float>(clip->duration) / clip->framerate;
    Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
    float clipX = baseWindowPos.x + timeToPixel(startTime);
    float clipWidth = timeToPixel(static_cast<Timecode>(durationSeconds * 1000000.0f));

    // Define the properties
    AnimatableProperty properties[] = {
        AnimatableProperty::PositionX,
        AnimatableProperty::PositionY,
        AnimatableProperty::ScaleX,
        AnimatableProperty::ScaleY,
        AnimatableProperty::Rotation,
        AnimatableProperty::Opacity
    };

    int numProperties = 6;

    for (int i = 0; i < numProperties; i++) {
        float rowY = startY + i * PROPERTY_ROW_HEIGHT;

        // Draw property row background (scrollable track area)
        ImVec2 rowMin(baseWindowPos.x, rowY);
        ImVec2 rowMax(baseWindowPos.x + 4000.0f, rowY + PROPERTY_ROW_HEIGHT);
        drawList->AddRectFilled(rowMin, rowMax, IM_COL32(35, 40, 50, 255));

        // Draw property track area background under clip (slightly lighter)
        ImVec2 trackMin(clipX, rowY);
        ImVec2 trackMax(clipX + clipWidth, rowY + PROPERTY_ROW_HEIGHT);
        drawList->AddRectFilled(trackMin, trackMax, IM_COL32(50, 55, 65, 255));

        // Draw keyframe shapes on the timeline
        const KeyframeTrack* track = animProps ? animProps->getTrack(properties[i]) : nullptr;
        if (track && track->hasKeyframes()) {
            float size = 5.0f;
            float keyframeY = rowY + PROPERTY_ROW_HEIGHT / 2.0f;
            ImU32 kfColor = IM_COL32(255, 200, 50, 255);  // Gold color for all keyframes
            ImU32 outlineColor = IM_COL32(255, 255, 255, 200);

            for (const auto& kf : track->keyframes) {
                // Convert keyframe frame to pixel position
                float kfSeconds = static_cast<float>(kf.frame) / static_cast<float>(clip->framerate);
                float kfX = clipX + (kfSeconds * m_pixelsPerSecond);

                // Only draw if within clip bounds
                if (kfX >= clipX && kfX <= clipX + clipWidth) {
                    switch (kf.interpolation) {
                        case InterpolationType::Step: {
                            // Square shape for Hold
                            ImVec2 sqMin(kfX - size, keyframeY - size);
                            ImVec2 sqMax(kfX + size, keyframeY + size);
                            drawList->AddRectFilled(sqMin, sqMax, kfColor);
                            drawList->AddRect(sqMin, sqMax, outlineColor, 0.0f, 0, 1.0f);
                            break;
                        }

                        case InterpolationType::EaseIn: {
                            // Left hourglass (curved), right diamond (pointed)
                            // Draw left side: hourglass curves
                            drawList->AddBezierQuadratic(
                                ImVec2(kfX - size, keyframeY - size),
                                ImVec2(kfX, keyframeY),
                                ImVec2(kfX - size, keyframeY + size),
                                kfColor, 3.0f);
                            // Draw right side: diamond point
                            ImVec2 rightDiamond[3] = {
                                ImVec2(kfX, keyframeY - size),
                                ImVec2(kfX + size, keyframeY),
                                ImVec2(kfX, keyframeY + size)
                            };
                            drawList->AddTriangleFilled(rightDiamond[0], rightDiamond[1], rightDiamond[2], kfColor);
                            // Outline
                            drawList->AddLine(ImVec2(kfX, keyframeY - size), ImVec2(kfX + size, keyframeY), outlineColor, 1.0f);
                            drawList->AddLine(ImVec2(kfX + size, keyframeY), ImVec2(kfX, keyframeY + size), outlineColor, 1.0f);
                            break;
                        }

                        case InterpolationType::EaseOut: {
                            // Left diamond (pointed), right hourglass (curved)
                            // Draw left side: diamond point
                            ImVec2 leftDiamond[3] = {
                                ImVec2(kfX, keyframeY - size),
                                ImVec2(kfX - size, keyframeY),
                                ImVec2(kfX, keyframeY + size)
                            };
                            drawList->AddTriangleFilled(leftDiamond[0], leftDiamond[1], leftDiamond[2], kfColor);
                            // Draw right side: hourglass curves
                            drawList->AddBezierQuadratic(
                                ImVec2(kfX + size, keyframeY - size),
                                ImVec2(kfX, keyframeY),
                                ImVec2(kfX + size, keyframeY + size),
                                kfColor, 3.0f);
                            // Outline
                            drawList->AddLine(ImVec2(kfX, keyframeY - size), ImVec2(kfX - size, keyframeY), outlineColor, 1.0f);
                            drawList->AddLine(ImVec2(kfX - size, keyframeY), ImVec2(kfX, keyframeY + size), outlineColor, 1.0f);
                            break;
                        }

                        case InterpolationType::EaseInOut: {
                            // Full hourglass shape (pinched in middle)
                            // Top triangle
                            drawList->AddTriangleFilled(
                                ImVec2(kfX - size, keyframeY - size),
                                ImVec2(kfX + size, keyframeY - size),
                                ImVec2(kfX, keyframeY),
                                kfColor);
                            // Bottom triangle
                            drawList->AddTriangleFilled(
                                ImVec2(kfX - size, keyframeY + size),
                                ImVec2(kfX + size, keyframeY + size),
                                ImVec2(kfX, keyframeY),
                                kfColor);
                            // Outline
                            ImVec2 hourglass[6] = {
                                ImVec2(kfX - size, keyframeY - size),
                                ImVec2(kfX + size, keyframeY - size),
                                ImVec2(kfX, keyframeY),
                                ImVec2(kfX + size, keyframeY + size),
                                ImVec2(kfX - size, keyframeY + size),
                                ImVec2(kfX, keyframeY)
                            };
                            drawList->AddPolyline(hourglass, 6, outlineColor, ImDrawFlags_Closed, 1.0f);
                            break;
                        }

                        case InterpolationType::Linear:
                        default: {
                            // Diamond shape for Linear
                            ImVec2 diamond[4] = {
                                ImVec2(kfX, keyframeY - size),
                                ImVec2(kfX + size, keyframeY),
                                ImVec2(kfX, keyframeY + size),
                                ImVec2(kfX - size, keyframeY)
                            };
                            drawList->AddConvexPolyFilled(diamond, 4, kfColor);
                            drawList->AddPolyline(diamond, 4, outlineColor, ImDrawFlags_Closed, 1.0f);
                            break;
                        }
                    }

                    // Handle right-click for context menu
                    if (ImGui::IsMouseClicked(1)) {
                        ImVec2 mousePos = ImGui::GetMousePos();
                        if (mousePos.x >= kfX - size - 3 && mousePos.x <= kfX + size + 3 &&
                            mousePos.y >= keyframeY - size - 3 && mousePos.y <= keyframeY + size + 3) {
                            m_keyframeEditClip = clipEntity;
                            m_keyframeEditProperty = properties[i];
                            m_keyframeEditFrame = kf.frame;
                            m_showKeyframeContextMenu = true;
                        }
                    }
                }
            }
        }
    }

    return numProperties * PROPERTY_ROW_HEIGHT;
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

    // Calculate cumulative Y position to account for expanded clips
    float cumulativeY = 0.0f;

    // Check each track
    for (size_t i = 0; i < tracks.size(); ++i) {
        entt::entity trackEntity = tracks[i];
        const auto* track = registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;

        // Calculate track Y position including offset from previous expanded tracks
        float trackY = windowPos.y + cumulativeY;

        // Calculate this track's height (including expanded clips)
        float trackHeight = TRACK_HEIGHT;
        for (entt::entity clipEntity : track->clips) {
            bool isExpanded = m_expandedClips.count(static_cast<uint32_t>(clipEntity)) > 0 ||
                              m_timeline->isClipExpanded(clipEntity);
            bool isSelected = (clipEntity == m_selectedClip) ||
                              (clipEntity == m_timeline->getSelectedClip());
            if (isExpanded && isSelected) {
                trackHeight = TRACK_HEIGHT + 6 * PROPERTY_ROW_HEIGHT;
                break;
            }
        }

        // Check if mouse Y is within this track (including expanded area)
        if (mousePos.y >= trackY && mousePos.y <= trackY + trackHeight) {
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

                // Calculate this clip's height (including expanded property tracks)
                float clipHeight = TRACK_HEIGHT;
                bool isExpanded = m_expandedClips.count(static_cast<uint32_t>(clipEntity)) > 0 ||
                                  m_timeline->isClipExpanded(clipEntity);
                bool isSelected = (clipEntity == m_selectedClip) ||
                                  (clipEntity == m_timeline->getSelectedClip());
                if (isExpanded && isSelected) {
                    clipHeight = TRACK_HEIGHT + 6 * PROPERTY_ROW_HEIGHT;
                }

                // Check if mouse is within clip bounds (including expanded area)
                if (mousePos.x >= clipX && mousePos.x <= clipX + clipWidth &&
                    mousePos.y >= trackY && mousePos.y <= trackY + clipHeight) {
                    outTrackIndex = static_cast<int>(i);
                    return clipEntity;
                }
            }
        }

        cumulativeY += trackHeight + TRACK_PADDING;
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
                m_timeline->setSelectedScreen(entt::null);  // Deselect screen when selecting clip

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
                m_timeline->setSelectedScreen(entt::null);  // Deselect screen when selecting clip

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

    // Keyframe context menu (for easing options)
    if (m_showKeyframeContextMenu) {
        ImGui::OpenPopup("KeyframeContextMenu");
        m_showKeyframeContextMenu = false;
    }

    if (ImGui::BeginPopup("KeyframeContextMenu")) {
        if (m_keyframeEditClip != entt::null) {
            auto& registry = m_timeline->getRegistry();
            auto* animProps = registry.try_get<AnimatedProperties>(m_keyframeEditClip);

            if (animProps) {
                auto* track = animProps->getTrack(m_keyframeEditProperty);
                if (track) {
                    Keyframe* kf = track->getKeyframeAt(m_keyframeEditFrame);
                    if (kf) {
                        ImGui::TextDisabled("Interpolation:");

                        bool isLinear = (kf->interpolation == InterpolationType::Linear);
                        bool isStep = (kf->interpolation == InterpolationType::Step);
                        bool isEaseIn = (kf->interpolation == InterpolationType::EaseIn);
                        bool isEaseOut = (kf->interpolation == InterpolationType::EaseOut);
                        bool isEaseInOut = (kf->interpolation == InterpolationType::EaseInOut);

                        if (ImGui::MenuItem("Linear (Diamond)", nullptr, isLinear)) {
                            kf->interpolation = InterpolationType::Linear;
                        }
                        if (ImGui::MenuItem("Hold (Square)", nullptr, isStep)) {
                            kf->interpolation = InterpolationType::Step;
                        }
                        if (ImGui::MenuItem("Ease In", nullptr, isEaseIn)) {
                            kf->interpolation = InterpolationType::EaseIn;
                        }
                        if (ImGui::MenuItem("Ease Out", nullptr, isEaseOut)) {
                            kf->interpolation = InterpolationType::EaseOut;
                        }
                        if (ImGui::MenuItem("Ease In/Out (Hourglass)", nullptr, isEaseInOut)) {
                            kf->interpolation = InterpolationType::EaseInOut;
                        }

                        ImGui::Separator();

                        if (ImGui::MenuItem("Delete Keyframe")) {
                            track->removeKeyframe(m_keyframeEditFrame);
                        }
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
}

void TimelineWidget::renderTrackHeaderPanel(float panelHeight, float verticalScroll) {
    if (!m_timeline) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetCursorScreenPos();

    // Draw panel background
    ImVec2 panelMin = windowPos;
    ImVec2 panelMax(windowPos.x + TRACK_HEADER_WIDTH - 4, windowPos.y + panelHeight + 100);
    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(40, 42, 48, 255));

    const auto& tracks = m_timeline->getTracks();
    auto& registry = m_timeline->getRegistry();

    float currentY = windowPos.y;

    for (size_t i = 0; i < tracks.size(); ++i) {
        float trackHeight = renderTrackHeaderRow(tracks[i], static_cast<int>(i), currentY);
        currentY += trackHeight;
    }
}

float TimelineWidget::renderTrackHeaderRow(entt::entity trackEntity, int trackIndex, float rowY) {
    if (!m_timeline) return TRACK_HEIGHT + TRACK_PADDING;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    auto& registry = m_timeline->getRegistry();

    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return TRACK_HEIGHT + TRACK_PADDING;

    uint32_t trackId = static_cast<uint32_t>(trackEntity);
    bool isExpanded = m_expandedTracks.count(trackId) > 0 || m_timeline->isTrackExpanded(trackEntity);

    // Track header background (matches track height)
    float headerX = windowPos.x + 2;
    ImVec2 headerMin(headerX, rowY);
    ImVec2 headerMax(headerX + TRACK_HEADER_WIDTH - 8, rowY + TRACK_HEIGHT);

    // Alternating background colors
    ImU32 bgColor = (trackIndex % 2 == 0)
        ? IM_COL32(50, 52, 58, 255)
        : IM_COL32(45, 47, 53, 255);
    drawList->AddRectFilled(headerMin, headerMax, bgColor);

    // Twirl-down triangle
    float triX = headerX + 12.0f;
    float triY = rowY + TRACK_HEIGHT / 2.0f;
    float triSize = 5.0f;

    ImVec2 triPoints[3];
    if (isExpanded) {
        // Down-pointing triangle (expanded)
        triPoints[0] = ImVec2(triX - triSize, triY - triSize / 2);
        triPoints[1] = ImVec2(triX + triSize, triY - triSize / 2);
        triPoints[2] = ImVec2(triX, triY + triSize / 2);
    } else {
        // Right-pointing triangle (collapsed)
        triPoints[0] = ImVec2(triX - triSize / 2, triY - triSize);
        triPoints[1] = ImVec2(triX + triSize / 2, triY);
        triPoints[2] = ImVec2(triX - triSize / 2, triY + triSize);
    }

    // Only show triangle if track has clips
    bool hasClips = !track->clips.empty();
    if (hasClips) {
        drawList->AddTriangleFilled(triPoints[0], triPoints[1], triPoints[2], IM_COL32(180, 180, 180, 255));
    }

    // Track name
    std::ostringstream trackName;
    trackName << "Track " << (trackIndex + 1);
    drawList->AddText(ImVec2(headerX + 26, rowY + TRACK_HEIGHT / 2 - 7),
                      IM_COL32(220, 220, 220, 255), trackName.str().c_str());

    // Clip count badge
    if (hasClips) {
        std::ostringstream clipCount;
        clipCount << "(" << track->clips.size() << ")";
        drawList->AddText(ImVec2(headerX + TRACK_HEADER_WIDTH - 50, rowY + TRACK_HEIGHT / 2 - 7),
                          IM_COL32(120, 120, 120, 255), clipCount.str().c_str());
    }

    // Handle click on twirl-down triangle
    if (hasClips && ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 triHitMin(triX - triSize - 6, triY - triSize - 6);
        ImVec2 triHitMax(triX + triSize + 6, triY + triSize + 6);

        if (mousePos.x >= triHitMin.x && mousePos.x <= triHitMax.x &&
            mousePos.y >= triHitMin.y && mousePos.y <= triHitMax.y) {
            if (isExpanded) {
                m_expandedTracks.erase(trackId);
            } else {
                m_expandedTracks.insert(trackId);
            }
        }
    }

    float totalHeight = TRACK_HEIGHT + TRACK_PADDING;

    // Render expanded clip rows
    if (isExpanded) {
        float clipY = rowY + TRACK_HEIGHT;
        for (entt::entity clipEntity : track->clips) {
            float clipHeight = renderClipHeaderRow(clipEntity, clipY);
            clipY += clipHeight;
            totalHeight += clipHeight;
        }
    }

    return totalHeight;
}

float TimelineWidget::renderClipHeaderRow(entt::entity clipEntity, float rowY) {
    if (!m_timeline) return HEADER_ROW_HEIGHT;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    auto& registry = m_timeline->getRegistry();

    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) return HEADER_ROW_HEIGHT;

    uint32_t clipId = static_cast<uint32_t>(clipEntity);
    bool isExpanded = m_expandedClips.count(clipId) > 0 || m_timeline->isClipExpanded(clipEntity);
    bool isSelected = (clipEntity == m_selectedClip) || (clipEntity == m_timeline->getSelectedClip());

    // Clip row background (indented)
    float headerX = windowPos.x + 2 + INDENT_WIDTH;
    ImVec2 headerMin(headerX, rowY);
    ImVec2 headerMax(headerX + TRACK_HEADER_WIDTH - 8 - INDENT_WIDTH, rowY + HEADER_ROW_HEIGHT);

    ImU32 bgColor = isSelected
        ? IM_COL32(60, 80, 120, 255)
        : IM_COL32(48, 50, 56, 255);
    drawList->AddRectFilled(headerMin, headerMax, bgColor);

    // Twirl-down triangle
    float triX = headerX + 10.0f;
    float triY = rowY + HEADER_ROW_HEIGHT / 2.0f;
    float triSize = 4.0f;

    ImVec2 triPoints[3];
    if (isExpanded) {
        // Down-pointing triangle (expanded)
        triPoints[0] = ImVec2(triX - triSize, triY - triSize / 2);
        triPoints[1] = ImVec2(triX + triSize, triY - triSize / 2);
        triPoints[2] = ImVec2(triX, triY + triSize / 2);
    } else {
        // Right-pointing triangle (collapsed)
        triPoints[0] = ImVec2(triX - triSize / 2, triY - triSize);
        triPoints[1] = ImVec2(triX + triSize / 2, triY);
        triPoints[2] = ImVec2(triX - triSize / 2, triY + triSize);
    }
    drawList->AddTriangleFilled(triPoints[0], triPoints[1], triPoints[2], IM_COL32(150, 150, 150, 255));

    // Clip name (filename without path)
    size_t lastSlash = clip->filepath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos)
        ? clip->filepath.substr(lastSlash + 1)
        : clip->filepath;

    // Truncate if too long
    if (filename.length() > 16) {
        filename = filename.substr(0, 13) + "...";
    }

    drawList->AddText(ImVec2(headerX + 22, rowY + 4),
                      IM_COL32(200, 200, 200, 255), filename.c_str());

    // Handle click on twirl-down triangle
    if (ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 triHitMin(triX - triSize - 6, triY - triSize - 6);
        ImVec2 triHitMax(triX + triSize + 6, triY + triSize + 6);

        if (mousePos.x >= triHitMin.x && mousePos.x <= triHitMax.x &&
            mousePos.y >= triHitMin.y && mousePos.y <= triHitMax.y) {
            if (isExpanded) {
                m_expandedClips.erase(clipId);
            } else {
                m_expandedClips.insert(clipId);
            }
        }
    }

    // Handle click on clip row to select
    if (ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        if (mousePos.x >= headerMin.x && mousePos.x <= headerMax.x &&
            mousePos.y >= headerMin.y && mousePos.y <= headerMax.y) {
            m_selectedClip = clipEntity;
            m_timeline->setSelectedClip(clipEntity);
            m_timeline->setSelectedScreen(entt::null);  // Deselect screen when selecting clip
        }
    }

    float totalHeight = HEADER_ROW_HEIGHT;

    // Render expanded property rows
    if (isExpanded) {
        float propY = rowY + HEADER_ROW_HEIGHT;
        float propX = headerX + INDENT_WIDTH;
        float propWidth = TRACK_HEADER_WIDTH - 8 - INDENT_WIDTH * 2;

        // Get animation data
        auto* animProps = registry.try_get<AnimatedProperties>(clipEntity);

        // Get current frame relative to clip
        FrameNumber currentFrame = m_timeline->getCurrentFrame();
        FrameNumber localFrame = currentFrame - clip->startFrame;

        // Define properties
        struct PropertyDef {
            AnimatableProperty prop;
            const char* name;
            float defaultValue;
        };

        PropertyDef properties[] = {
            {AnimatableProperty::PositionX, "Pos X", 0.0f},
            {AnimatableProperty::PositionY, "Pos Y", 0.0f},
            {AnimatableProperty::ScaleX, "Scale X", 1.0f},
            {AnimatableProperty::ScaleY, "Scale Y", 1.0f},
            {AnimatableProperty::Rotation, "Rotation", 0.0f},
            {AnimatableProperty::Opacity, "Opacity", 1.0f}
        };

        for (int i = 0; i < 6; ++i) {
            // Property row background
            ImVec2 propMin(propX, propY);
            ImVec2 propMax(propX + propWidth, propY + PROPERTY_ROW_HEIGHT);
            drawList->AddRectFilled(propMin, propMax, IM_COL32(42, 44, 50, 255));

            // Get keyframe track info
            const KeyframeTrack* track = animProps ? animProps->getTrack(properties[i].prop) : nullptr;
            bool hasKeyframes = track && track->hasKeyframes();
            bool atKeyframe = track && track->getKeyframeAt(localFrame) != nullptr;

            // Get current value
            float currentValue = properties[i].defaultValue;
            if (animProps) {
                currentValue = animProps->evaluate(properties[i].prop, localFrame);
            }

            float centerY = propY + PROPERTY_ROW_HEIGHT / 2.0f;

            // === PROPERTY NAME ===
            drawList->AddText(ImVec2(propX + 4, propY + 2),
                              IM_COL32(140, 140, 140, 255), properties[i].name);

            // === KEYFRAME CONTROLS (right side of row) ===
            float controlsX = propX + propWidth - 70;  // Position for controls
            float arrowSize = 3.0f;
            float diamondSize = 4.0f;

            // Left arrow (prev keyframe)
            float leftArrowX = controlsX;
            ImVec2 leftArrow[3] = {
                ImVec2(leftArrowX - arrowSize, centerY),
                ImVec2(leftArrowX + arrowSize, centerY - arrowSize),
                ImVec2(leftArrowX + arrowSize, centerY + arrowSize)
            };
            ImU32 arrowColor = hasKeyframes ? IM_COL32(180, 180, 180, 255) : IM_COL32(80, 80, 80, 255);
            drawList->AddTriangleFilled(leftArrow[0], leftArrow[1], leftArrow[2], arrowColor);

            // Diamond (keyframe indicator)
            float diamondX = controlsX + 14;
            ImVec2 diamond[4] = {
                ImVec2(diamondX, centerY - diamondSize),
                ImVec2(diamondX + diamondSize, centerY),
                ImVec2(diamondX, centerY + diamondSize),
                ImVec2(diamondX - diamondSize, centerY)
            };
            if (atKeyframe) {
                drawList->AddConvexPolyFilled(diamond, 4, IM_COL32(255, 200, 50, 255));
            } else if (hasKeyframes) {
                drawList->AddPolyline(diamond, 4, IM_COL32(180, 180, 180, 255), ImDrawFlags_Closed, 1.0f);
            } else {
                drawList->AddPolyline(diamond, 4, IM_COL32(80, 80, 80, 255), ImDrawFlags_Closed, 1.0f);
            }

            // Right arrow (next keyframe)
            float rightArrowX = controlsX + 28;
            ImVec2 rightArrow[3] = {
                ImVec2(rightArrowX + arrowSize, centerY),
                ImVec2(rightArrowX - arrowSize, centerY - arrowSize),
                ImVec2(rightArrowX - arrowSize, centerY + arrowSize)
            };
            drawList->AddTriangleFilled(rightArrow[0], rightArrow[1], rightArrow[2], arrowColor);

            // === CURRENT VALUE ===
            char valueStr[32];
            if (properties[i].prop == AnimatableProperty::Opacity) {
                snprintf(valueStr, sizeof(valueStr), "%.0f%%", currentValue * 100.0f);
            } else if (properties[i].prop == AnimatableProperty::Rotation) {
                snprintf(valueStr, sizeof(valueStr), "%.1f", currentValue);
            } else {
                snprintf(valueStr, sizeof(valueStr), "%.2f", currentValue);
            }
            drawList->AddText(ImVec2(controlsX + 38, propY + 2),
                              IM_COL32(200, 200, 200, 255), valueStr);

            // === HANDLE CLICKS ===
            if (ImGui::IsMouseClicked(0)) {
                ImVec2 mousePos = ImGui::GetMousePos();

                // Left arrow click (prev keyframe)
                if (hasKeyframes &&
                    mousePos.x >= leftArrowX - arrowSize - 4 && mousePos.x <= leftArrowX + arrowSize + 4 &&
                    mousePos.y >= centerY - arrowSize - 4 && mousePos.y <= centerY + arrowSize + 4) {
                    // Find previous keyframe
                    FrameNumber prevFrame = -1;
                    for (const auto& kf : track->keyframes) {
                        if (kf.frame < localFrame && kf.frame > prevFrame) {
                            prevFrame = kf.frame;
                        }
                    }
                    if (prevFrame >= 0) {
                        FrameNumber globalFrame = prevFrame + clip->startFrame;
                        m_timeline->seek(static_cast<Timecode>(globalFrame / clip->framerate * 1000000.0f));
                    }
                }

                // Diamond click (add/remove keyframe)
                if (mousePos.x >= diamondX - diamondSize - 4 && mousePos.x <= diamondX + diamondSize + 4 &&
                    mousePos.y >= centerY - diamondSize - 4 && mousePos.y <= centerY + diamondSize + 4) {
                    if (atKeyframe && animProps) {
                        // Remove keyframe
                        auto* mutableTrack = animProps->getTrack(properties[i].prop);
                        if (mutableTrack) {
                            mutableTrack->removeKeyframe(localFrame);
                        }
                    } else {
                        // Add keyframe at current position
                        auto& props = registry.get_or_emplace<AnimatedProperties>(clipEntity);
                        props.addKeyframe(properties[i].prop, localFrame, currentValue, InterpolationType::Linear);
                    }
                }

                // Right arrow click (next keyframe)
                if (hasKeyframes &&
                    mousePos.x >= rightArrowX - arrowSize - 4 && mousePos.x <= rightArrowX + arrowSize + 4 &&
                    mousePos.y >= centerY - arrowSize - 4 && mousePos.y <= centerY + arrowSize + 4) {
                    // Find next keyframe
                    FrameNumber nextFrame = std::numeric_limits<FrameNumber>::max();
                    for (const auto& kf : track->keyframes) {
                        if (kf.frame > localFrame && kf.frame < nextFrame) {
                            nextFrame = kf.frame;
                        }
                    }
                    if (nextFrame != std::numeric_limits<FrameNumber>::max()) {
                        FrameNumber globalFrame = nextFrame + clip->startFrame;
                        m_timeline->seek(static_cast<Timecode>(globalFrame / clip->framerate * 1000000.0f));
                    }
                }
            }

            propY += PROPERTY_ROW_HEIGHT;
            totalHeight += PROPERTY_ROW_HEIGHT;
        }
    }

    return totalHeight;
}

} // namespace entity
