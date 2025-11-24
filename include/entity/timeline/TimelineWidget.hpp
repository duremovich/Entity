#pragma once

#include "entity/timeline/Timeline.hpp"
#include <imgui.h>

namespace entity {

/**
 * TimelineWidget - ImGui widget for rendering and interacting with the timeline
 *
 * Renders:
 * - Time ruler with timecode markings
 * - Track lanes (horizontal rows)
 * - Clips within tracks (colored rectangles)
 * - Playhead position indicator
 *
 * Interaction:
 * - Click on time ruler to seek
 * - Drag clips to reposition
 * - Select clips
 * - Add/remove tracks
 */
class TimelineWidget {
public:
    TimelineWidget(Timeline* timeline);
    ~TimelineWidget() = default;

    /**
     * Render the timeline widget.
     * Call this from your ImGui rendering code.
     */
    void render();

    /**
     * Get/set the timeline this widget displays.
     */
    void setTimeline(Timeline* timeline) { m_timeline = timeline; }
    Timeline* getTimeline() const { return m_timeline; }

    /**
     * Get/set the zoom level (pixels per second).
     * Higher values = more zoomed in.
     */
    void setZoom(float pixelsPerSecond) { m_pixelsPerSecond = pixelsPerSecond; }
    float getZoom() const { return m_pixelsPerSecond; }

    /**
     * Get/set the scroll position (in pixels from the left).
     */
    void setScrollX(float scrollX) { m_scrollX = scrollX; }
    float getScrollX() const { return m_scrollX; }

private:
    /**
     * Render the time ruler (top bar with timecode markers).
     */
    void renderTimeRuler();

    /**
     * Render all track lanes.
     */
    void renderTracks();

    /**
     * Render a single track lane.
     */
    void renderTrack(entt::entity trackEntity, int trackIndex);

    /**
     * Render a clip within a track.
     */
    void renderClip(entt::entity clipEntity, int trackIndex);

    /**
     * Render the playhead indicator.
     */
    void renderPlayhead();

    /**
     * Convert timeline time (milliseconds) to screen x position (pixels).
     */
    float timeToPixel(Timecode time) const;

    /**
     * Convert screen x position (pixels) to timeline time (milliseconds).
     */
    Timecode pixelToTime(float pixel) const;

    /**
     * Handle timeline interactions (seeking, dragging clips, etc.).
     */
    void handleInteraction();

    /**
     * Find the clip entity at the given screen position.
     * Returns entt::null if no clip is found.
     */
    entt::entity findClipAtPosition(ImVec2 mousePos, ImVec2 windowPos, int& outTrackIndex);

private:
    Timeline* m_timeline{nullptr};

    // View settings
    float m_pixelsPerSecond{100.0f};  // Zoom level
    float m_scrollX{0.0f};             // Horizontal scroll position

    // Layout constants
    static constexpr float RULER_HEIGHT = 30.0f;
    static constexpr float TRACK_HEIGHT = 60.0f;
    static constexpr float TRACK_PADDING = 4.0f;
    static constexpr float CLIP_PADDING = 2.0f;

    // Interaction state
    entt::entity m_selectedClip{entt::null};
    bool m_isDraggingClip{false};
    bool m_isDraggingRuler{false};
    Timecode m_clipDragStartTime{0};
    float m_dragOffsetX{0.0f};
};

} // namespace entity
