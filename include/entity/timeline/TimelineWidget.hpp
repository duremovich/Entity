#pragma once

#include "entity/timeline/Timeline.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <imgui.h>
#include <algorithm>
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
     * Discrete zoom ladder, in frames per tick. Single tier — the ladder
     * value IS the tick spacing AND the snap increment AND the dropdown
     * label. "10f" means: tick lines every 10 frames, scrub snaps to every
     * 10 frames. No separate minor/major hierarchy (it confused users about
     * what the snap actually was). Disguise / Resolve-style stepping.
     */
    static constexpr int FRAMES_PER_TICK[] = {1, 2, 5, 10, 20, 50, 100, 200, 500};
    static constexpr int ZOOM_LEVEL_COUNT = 9;
    static constexpr float TICK_PX = 20.0f;  // visual width of one tick division (fixed — zoom changes the time each tick represents, not the spacing)

    int getZoomIndex() const { return m_zoomIndex; }
    /**
     * Set zoom index. Re-anchors scroll so the playhead (if visible) or the
     * viewport center stays at the same screen position across the zoom change
     * — otherwise the view jumps unpredictably because scroll is in absolute
     * pixels and zoom changes the px/sec ratio.
     */
    void setZoomIndex(int idx);
    int framesPerTick() const { return FRAMES_PER_TICK[m_zoomIndex]; }

    /**
     * Derived pixels-per-second based on current zoom level + timeline framerate.
     * Cached in m_pixelsPerSecond at render() top so the rest of the widget can
     * stay unchanged.
     */
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

    /**
     * Adjust horizontal scroll so the playhead is within the visible range.
     * Called after keyboard seeks (arrow keys, Home/End, J/L) so the playhead
     * never walks off-screen. No-op if the widget hasn't rendered yet.
     * Vertical scroll is untouched.
     */
    void ensurePlayheadVisible();

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
    int m_zoomIndex{3};                // index into FRAMES_PER_TICK; default 10f tick
    float m_pixelsPerSecond{100.0f};  // derived from zoom index + frame rate, refreshed each render()
    float m_scrollX{0.0f};             // Horizontal scroll position
    float m_syncScrollX{0.0f};         // Sync scroll between ruler and tracks
    float m_lastVisibleWidth{0.0f};    // Horizontal viewport width captured each render(),
                                       // used by ensurePlayheadVisible() to follow the playhead.
    bool m_pendingScrollX{false};      // True for one render() after ensurePlayheadVisible()
                                       // moved m_syncScrollX programmatically — suppresses the
                                       // read-back at the bottom of render() that would otherwise
                                       // stomp the target (ImGui's SetScrollX sets a target that
                                       // doesn't reach GetScrollX until the NEXT Begin()).

    // Recompute m_pixelsPerSecond from m_zoomIndex + Timeline frame rate.
    // Called at the top of render() so a frame-rate change picks up immediately.
    void applyZoomIndex();

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

    // Time-range selection on the ruler (shift+drag). Used as the input to
    // upcoming ripple-insert / ripple-delete ops and for naming sections.
    struct TimeRange {
        Timecode start{0};
        Timecode end{0};
        bool active{false};
    };
    TimeRange m_range;
    bool m_isCreatingRange{false};
    Timecode m_rangeAnchorTime{0};

public:
    /** Public range accessors so command code (and tests) can read the
     *  selection without poking widget internals. */
    bool hasRangeSelection() const { return m_range.active && m_range.end > m_range.start; }
    Timecode getRangeStart() const { return m_range.start; }
    Timecode getRangeEnd() const { return m_range.end; }
    void clearRangeSelection() { m_range = {}; }

private:
    /** Snap a Timecode to the nearest minor-tick frame boundary at the
     *  current zoom level. Used to keep range endpoints frame-aligned. */
    Timecode snapTimeToTickGrid(Timecode t) const;

    // Section context-menu state. -1 means "no section right-clicked";
    // otherwise it's an index into Timeline::getSections().
    int m_rightClickedSection{-1};
    bool m_rangeContextMenuRequested{false};

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
