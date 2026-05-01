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
#include <sstream>
#include <cmath>
#include <iostream>
#include <limits>

namespace entity {

TimelineWidget::TimelineWidget(Timeline* timeline)
    : m_timeline(timeline)
{
}

void TimelineWidget::applyZoomIndex() {
    if (!m_timeline) return;
    // Tick spacing is fixed at TICK_PX. Zoom changes how much TIME a tick
    // represents (framesPerTick), which scales pxPerSec accordingly.
    const float pxPerFrame = TICK_PX / static_cast<float>(framesPerTick());
    m_pixelsPerSecond = pxPerFrame * static_cast<float>(m_timeline->getFrameRate());
}

void TimelineWidget::setZoomIndex(int idx) {
    const int newIdx = std::clamp(idx, 0, ZOOM_LEVEL_COUNT - 1);
    if (newIdx == m_zoomIndex) return;

    // Re-anchor scroll so a reference time stays at the same screen position
    // across the zoom change — otherwise scroll stays at the old absolute pixel
    // value and the viewport jumps to a completely different time when zoom
    // (and therefore pxPerSec) changes.
    const bool canReanchor = (m_lastVisibleWidth > 0.0f && m_timeline != nullptr);
    float anchorScreenOffset = 0.0f;
    Timecode anchorTime = 0;
    if (canReanchor) {
        const float playheadPx = timeToPixel(m_timeline->getCurrentTime());
        const float viewLeft  = m_syncScrollX;
        const float viewRight = m_syncScrollX + m_lastVisibleWidth;
        const bool playheadVisible = (playheadPx >= viewLeft && playheadPx <= viewRight);
        if (playheadVisible) {
            anchorTime = m_timeline->getCurrentTime();
            anchorScreenOffset = playheadPx - viewLeft;
        } else {
            anchorScreenOffset = m_lastVisibleWidth * 0.5f;
            anchorTime = pixelToTime(viewLeft + anchorScreenOffset);
        }
    }

    m_zoomIndex = newIdx;
    applyZoomIndex();

    if (canReanchor) {
        const float anchorPxNew = timeToPixel(anchorTime);
        const float durationPx = timeToPixel(m_timeline->getDuration());
        const float maxScrollX = std::max(0.0f, durationPx - m_lastVisibleWidth);
        float newScroll = anchorPxNew - anchorScreenOffset;
        newScroll = std::clamp(newScroll, 0.0f, maxScrollX);
        m_syncScrollX = newScroll;
        m_pendingScrollX = true;
    }
}

void TimelineWidget::render() {
    if (!m_timeline) {
        ImGui::Text("No timeline set");
        return;
    }

    // Clean up stale entity references in expansion state
    {
        auto& reg = m_timeline->getRegistry();
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

    // Calculate timeline dimensions. Tick spacing is fixed at TICK_PX; zoom
    // changes the time each tick represents, so content width grows/shrinks
    // with zoom (and horizontal scroll is used to pan within long timelines).
    int trackCount = static_cast<int>(m_timeline->getTrackCount());
    float durationSeconds = m_timeline->getDuration() / 1000000.0f;
    m_lastVisibleWidth = contentRegion.x - TRACK_HEADER_WIDTH - 4.0f;
    applyZoomIndex();
    float timelineWidth = durationSeconds * m_pixelsPerSecond;

    // Calculate total tracks content height (accounting for expanded tracks/clips)
    float tracksContentHeight = 0.0f;
    const auto& tracks = m_timeline->getTracks();
    auto& registry = m_timeline->getRegistry();
    for (size_t i = 0; i < tracks.size(); ++i) {
        uint32_t trackId = static_cast<uint32_t>(tracks[i]);
        bool trackExpanded = m_expandedTracks.count(trackId) > 0 || m_timeline->isTrackExpanded(tracks[i]);
        tracksContentHeight += TRACK_HEIGHT + TRACK_PADDING;

        // Expanded track adds room for the playhead clip's property panel
        // (6 rows × PROPERTY_ROW_HEIGHT). When no clip overlaps the playhead
        // on this track, no extra height — expansion is empty.
        if (trackExpanded && findClipAtPlayhead(tracks[i]) != entt::null) {
            tracksContentHeight += 6 * PROPERTY_ROW_HEIGHT;
        }
    }
    (void)registry;

    // Reserve space for controls at bottom (approximately 40 pixels)
    float controlsHeight = 40.0f;
    float availableHeight = contentRegion.y - controlsHeight;
    float tracksWindowHeight = availableHeight - RULER_HEIGHT;
    float timelineContentWidth = m_lastVisibleWidth;

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
    // NoNav: arrow keys are owned by Engine::handleKey(). NoScrollWithMouse so
    // the wheel scrolls the tracks child (below), not the ruler independently.
    // NoScrollbar hides the user-facing scrollbar widget — internal SetScrollX
    // below still mirrors from the tracks child via m_syncScrollX. Without
    // NoScrollbar, dragging the ruler's scrollbar desyncs from the tracks
    // scroll and the playhead renders as a diagonal line (top end uses ruler
    // scroll, bottom end uses tracks scroll).
    ImGui::BeginChild("TimelineRuler", ImVec2(timelineContentWidth, RULER_HEIGHT), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
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
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);

    // Create inner scrollable region for header content
    ImGui::BeginChild("TrackHeadersInner", ImVec2(TRACK_HEADER_WIDTH - 2, tracksContentHeight + 100), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);

    // Apply synced vertical scroll
    ImGui::SetScrollY(m_syncScrollY);

    renderTrackHeaderPanel(tracksContentHeight, m_syncScrollY);

    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine(0, 2.0f);

    // === SCROLLABLE TRACKS (right side — horizontal + vertical scroll) ===
    ImGui::BeginChild("TimelineTracks", ImVec2(timelineContentWidth, tracksWindowHeight), false,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);

    ImGui::SetScrollX(m_syncScrollX);
    ImGui::SetScrollY(m_syncScrollY);

    // Save tracks position for playhead rendering
    m_tracksScreenPos = ImGui::GetCursorScreenPos();

    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##tracksArea", ImVec2(timelineWidth, tracksContentHeight + 100));
    ImGui::SetCursorPos(ImVec2(0, 0));

    renderTracks();
    handleTracksInteraction();

    // Save scroll position for next frame. Skip X read-back for one frame after
    // ensurePlayheadVisible() moved m_syncScrollX programmatically — SetScrollX
    // sets a target that isn't reflected in GetScrollX until the NEXT Begin(),
    // so reading it now returns the pre-target value and stomps our move
    // (produces a 2-frame ping-pong).
    if (!m_pendingScrollX) {
        m_syncScrollX = ImGui::GetScrollX();
    }
    m_pendingScrollX = false;
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
    ImGui::Text("Frame %lld", static_cast<long long>(currentFrame));

    ImGui::SameLine();

    // Discrete zoom ladder: dropdown + minus/plus + Alt+scroll all step the
    // same m_zoomIndex. Disguise/Resolve-style fixed division sizes instead
    // of a continuous px/sec slider.
    static const char* kZoomLabels[ZOOM_LEVEL_COUNT] = {
        "1f", "2f", "5f", "10f", "20f", "50f", "100f", "200f", "500f"
    };
    ImGui::Text("Zoom:");
    ImGui::SameLine();
    if (ImGui::SmallButton("-##zoomOut")) setZoomIndex(m_zoomIndex + 1);  // larger frames/div = zoomed out
    ImGui::SameLine(0, 2);
    ImGui::SetNextItemWidth(70.0f);
    {
        // Route through setZoomIndex() so the re-anchor happens. Writing directly
        // to m_zoomIndex via Combo bypasses the scroll adjustment.
        int pickedIdx = m_zoomIndex;
        if (ImGui::Combo("##zoom", &pickedIdx, kZoomLabels, ZOOM_LEVEL_COUNT)) {
            setZoomIndex(pickedIdx);
        }
    }
    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton("+##zoomIn")) setZoomIndex(m_zoomIndex - 1);   // smaller frames/div = zoomed in

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Add Track button
    if (ImGui::Button("+ Add Track")) {
        std::ostringstream trackName;
        trackName << "Video Track " << (m_timeline->getTrackCount() + 1);
        m_timeline->createTrack(trackName.str());
    }

    // Per-clip buffer-fill indicator removed in Phase C.10 — frames now
    // live in an engine-global FrameCache. A cache-wide bytesUsed/maxBytes
    // overlay can come back here once the widget gets an Engine* injected.

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

entt::entity TimelineWidget::findClipAtPlayhead(entt::entity trackEntity) const {
    if (!m_timeline) return entt::null;
    auto& registry = m_timeline->getRegistry();
    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return entt::null;

    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    for (entt::entity clipEntity : track->clips) {
        const auto* clip = registry.try_get<Clip>(clipEntity);
        if (!clip) continue;
        if (currentFrame >= clip->startFrame &&
            currentFrame <  clip->startFrame + clip->duration) {
            return clipEntity;
        }
    }
    return entt::null;
}

void TimelineWidget::ensurePlayheadVisible() {
    if (!m_timeline) return;
    if (m_lastVisibleWidth <= 0.0f) return;
    applyZoomIndex();

    const float playheadPx = timeToPixel(m_timeline->getCurrentTime());
    const float durationPx = timeToPixel(m_timeline->getDuration());
    const float margin = 20.0f;
    const float viewLeft  = m_syncScrollX;
    const float viewRight = m_syncScrollX + m_lastVisibleWidth;
    const float maxScrollX = std::max(0.0f, durationPx - m_lastVisibleWidth);

    float newScrollX = m_syncScrollX;
    if (playheadPx < viewLeft + margin) {
        newScrollX = playheadPx - margin;
    } else if (playheadPx > viewRight - margin) {
        newScrollX = playheadPx - m_lastVisibleWidth + margin;
    }
    newScrollX = std::clamp(newScrollX, 0.0f, maxScrollX);

    if (newScrollX != m_syncScrollX) {
        m_syncScrollX = newScrollX;
        m_pendingScrollX = true;
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

Timecode TimelineWidget::snapTimeToTickGrid(Timecode t) const {
    if (!m_timeline) return t;
    const FrameNumber tickEvery = static_cast<FrameNumber>(framesPerTick());
    FrameNumber f = m_timeline->timeToFrame(t);
    f = ((f + tickEvery / 2) / tickEvery) * tickEvery;  // round to nearest tick
    return m_timeline->frameToTime(f);
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
