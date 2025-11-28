#pragma once

#include "entity/timeline/Timeline.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <imgui.h>
#include <functional>
#include <string>
#include <unordered_set>
#include <unordered_map>

namespace entity {

// Callback for when a media file is dropped onto a track
// Parameters: filepath, track index, timecode position
using MediaDropCallback = std::function<void(const std::string&, int, Timecode)>;

/**
 * Clip edge for trimming operations
 */
enum class ClipEdge {
    None,
    Left,   // Trim in-point (start)
    Right   // Trim out-point (end)
};

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
 * - Drag clip edges to trim in/out points
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

    /**
     * Set callback for when media is dropped onto a track.
     */
    void setMediaDropCallback(MediaDropCallback callback) { m_mediaDropCallback = std::move(callback); }

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
     * @param baseWindowPos Base window position to use for this track
     * @param trackHeight Height of this track (may be taller if clips are expanded)
     */
    void renderTrack(entt::entity trackEntity, int trackIndex, ImVec2 baseWindowPos, float trackHeight);

    /**
     * Render a clip within a track.
     * @param baseWindowPos Base window position to use for positioning
     * @return Height of rendered content (clip + expanded properties)
     */
    float renderClip(entt::entity clipEntity, int trackIndex, ImVec2 baseWindowPos);

    /**
     * Render property tracks for an expanded clip (keyframe diamonds only).
     * @return Height of rendered property tracks
     */
    float renderPropertyTracks(entt::entity clipEntity, int trackIndex, ImVec2 baseWindowPos, float clipY);

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
     * Handle ruler-specific interactions (scrubbing, zooming).
     */
    void handleRulerInteraction();

    /**
     * Handle tracks-specific interactions (clip dragging, selection).
     */
    void handleTracksInteraction();

    /**
     * Find the clip entity at the given screen position.
     * Returns entt::null if no clip is found.
     */
    entt::entity findClipAtPosition(ImVec2 mousePos, ImVec2 windowPos, int& outTrackIndex);

    /**
     * Check if mouse is near a clip edge for trimming.
     * Returns the edge type (None, Left, Right).
     */
    ClipEdge findClipEdgeAtPosition(ImVec2 mousePos, ImVec2 windowPos, entt::entity& outClip, int& outTrackIndex);

    /**
     * Check if a clip would collide with other clips on the same track.
     * Returns the nearest valid position (snapped to avoid collision).
     */
    Timecode checkClipCollision(entt::entity clipEntity, Timecode newStartTime, int trackIndex);

    /**
     * Render the track header panel (left side with hierarchy).
     * This panel shows tracks with twirl-downs for clips and properties.
     * @param panelHeight Height of the panel
     * @param verticalScroll Current vertical scroll position to sync
     */
    void renderTrackHeaderPanel(float panelHeight, float verticalScroll);

    /**
     * Render a single track row in the header panel.
     * @param trackEntity The track entity
     * @param trackIndex Track index
     * @param rowY Y position for this row
     * @return Height consumed by this track (including expanded clips)
     */
    float renderTrackHeaderRow(entt::entity trackEntity, int trackIndex, float rowY);

    /**
     * Render a clip row in the header panel (indented under track).
     * @param clipEntity The clip entity
     * @param rowY Y position for this row
     * @return Height consumed by this clip (including expanded properties)
     */
    float renderClipHeaderRow(entt::entity clipEntity, float rowY);

    /**
     * Handle right-click context menus.
     */
    void handleContextMenus();

    /**
     * Find which track is at the given Y position.
     * @return Track index or -1 if not over a track
     */
    int findTrackAtY(float mouseY, float windowY) const;

    /**
     * Get the selected clip entity.
     */
    entt::entity getSelectedClip() const { return m_selectedClip; }

private:
    Timeline* m_timeline{nullptr};

    // View settings
    float m_pixelsPerSecond{100.0f};  // Zoom level
    float m_scrollX{0.0f};             // Horizontal scroll position
    float m_syncScrollX{0.0f};         // Sync scroll between ruler and tracks

    // Cached positions for rendering playhead across child windows
    ImVec2 m_rulerScreenPos{0, 0};
    ImVec2 m_tracksScreenPos{0, 0};
    float m_tracksHeight{0.0f};

    // Layout constants
    static constexpr float RULER_HEIGHT = 30.0f;
    static constexpr float TRACK_HEIGHT = 60.0f;
    static constexpr float TRACK_PADDING = 4.0f;
    static constexpr float CLIP_PADDING = 2.0f;

    // Interaction state
    entt::entity m_selectedClip{entt::null};
    bool m_isDraggingClip{false};
    bool m_isDraggingRuler{false};
    Timecode m_lastSeekTime{-1};  // Debounce: last time we sought to
    int m_selectedClipTrackIndex{-1};
    Timecode m_clipDragStartTime{0};
    float m_dragOffsetX{0.0f};

    // Trimming state
    bool m_isTrimmingClip{false};
    ClipEdge m_trimEdge{ClipEdge::None};
    entt::entity m_trimClip{entt::null};
    FrameNumber m_trimOriginalStart{0};
    FrameNumber m_trimOriginalDuration{0};
    FrameNumber m_trimOriginalMediaStart{0};
    static constexpr float TRIM_EDGE_WIDTH = 8.0f;  // Pixels from edge to trigger trim

    // Context menu state
    int m_rightClickedTrackIndex{-1};
    entt::entity m_rightClickedClip{entt::null};
    bool m_showTrackContextMenu{false};
    bool m_showClipContextMenu{false};

    // Snapping
    static constexpr float SNAP_THRESHOLD_PIXELS = 10.0f;  // Pixels threshold for snapping
    bool m_snappingEnabled{true};
    bool m_isSnapping{false};          // True when actively snapping
    Timecode m_snapTargetTime{0};      // Time position being snapped to

    // Callbacks
    MediaDropCallback m_mediaDropCallback;

    // Track and clip expansion state (for showing hierarchy)
    std::unordered_set<uint32_t> m_expandedTracks;  // Set of expanded track entity IDs
    std::unordered_set<uint32_t> m_expandedClips;  // Set of expanded clip entity IDs
    static constexpr float PROPERTY_ROW_HEIGHT = 20.0f;  // Height of each property track row
    static constexpr float TRACK_HEADER_WIDTH = 200.0f;  // Width of track header panel
    static constexpr float HEADER_ROW_HEIGHT = 24.0f;  // Height of track/clip header rows
    static constexpr float INDENT_WIDTH = 16.0f;  // Indentation for hierarchy levels
    float m_syncScrollY{0.0f};  // Synchronized vertical scroll position

    // Keyframe editing state
    entt::entity m_keyframeEditClip{entt::null};
    AnimatableProperty m_keyframeEditProperty{AnimatableProperty::PositionX};
    FrameNumber m_keyframeEditFrame{0};
    bool m_showKeyframeContextMenu{false};
};

} // namespace entity
