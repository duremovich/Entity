/**
 * TimelineWidget core: constructor, top-level render() dispatch, and the small
 * coordinate/hit-test helpers shared by both the render and input halves.
 *
 * Split layout (Phase B #16):
 *   - TimelineWidget.cpp         (this file) — ctor, render(), timeToPixel,
 *                                               pixelToTime, findTrackAtY,
 *                                               checkClipCollision
 *   - TimelineWidgetRender.cpp   — all render*() draw code
 *   - TimelineWidgetInput.cpp    — all handle*() / findClip*() interaction code
 */

#include "entity/timeline/TimelineWidget.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/media/FrameRingBuffer.hpp"
#include <sstream>
#include <cmath>
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

float TimelineWidget::timeToPixel(Timecode time) const {
    float seconds = time / 1000000.0f;  // Timecode is in microseconds
    return seconds * m_pixelsPerSecond;
}

Timecode TimelineWidget::pixelToTime(float pixel) const {
    float seconds = pixel / m_pixelsPerSecond;
    return static_cast<Timecode>(seconds * 1000000.0f);  // Timecode is in microseconds
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

} // namespace entity
