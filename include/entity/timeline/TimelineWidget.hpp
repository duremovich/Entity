#pragma once

#include "entity/timeline/Timeline.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <imgui.h>
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace entity {

// Forward declaration — TimelineWidget may enqueue ruler-context-menu
// commands (Phase A cue ops) when wired by Engine::initializeWindowsAndCallbacks.
class CommandDispatcher;

// Forward declaration — TimelineWidget queries Engine's clipboard
// snapshot to enable/disable Paste in the clip context menu, and walks
// Engine::getProjectManager()'s media library for the Change Media modal.
class Engine;

// Callback for when a media file is dropped onto a track
// Parameters: filepath, track index, timecode position
using MediaDropCallback = std::function<void(const std::string&, int, Timecode)>;

// Callback for when an OA layer kind is dropped onto a track
// Parameters: track index, start frame (timeline), duration in frames
using OALayerDropCallback = std::function<void(int, FrameNumber, FrameNumber)>;

// Callback for when a Clip layer kind is dropped onto a track (empty clip, no media)
// Parameters: track index, start frame (timeline), duration in frames
using ClipLayerDropCallback = std::function<void(int, FrameNumber, FrameNumber)>;

// Callback for when a Generative layer kind is dropped onto a track.
// V1 only ships the Muncher kind; future kinds either reuse this callback
// with a sub-kind argument or add their own typedef.
// Parameters: track index, start frame (timeline), duration in frames
using GenerativeLayerDropCallback = std::function<void(int, FrameNumber, FrameNumber)>;

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

    void setOALayerDropCallback(OALayerDropCallback callback) { m_oaLayerDropCallback = std::move(callback); }
    void setClipLayerDropCallback(ClipLayerDropCallback callback) { m_clipLayerDropCallback = std::move(callback); }
    void setGenerativeLayerDropCallback(GenerativeLayerDropCallback callback) { m_generativeLayerDropCallback = std::move(callback); }

    /**
     * Optional — wire the CommandDispatcher so ruler-context-menu actions
     * (Phase A cue ops) can enqueue undoable commands. Without it, those
     * menu items are inert (the menu still draws).
     */
    void setCommandDispatcher(CommandDispatcher* dispatcher) { m_commandDispatcher = dispatcher; }

    /**
     * Optional — wire Engine so the clip context menu can query the
     * clipboard state (enables/disables Paste) and the Change Media
     * modal can walk the project's media library. Without it, those
     * specific menu items are inert; the rest of the widget works.
     */
    void setEngine(Engine* engine) { m_engine = engine; }

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
     * Render the property panel (per-property name + nav arrows + keyframe
     * diamond + value) for the given clip directly under an expanded track.
     * Called once per expanded track with the clip currently at the playhead.
     * @return Height consumed (rowCount × PROPERTY_ROW_HEIGHT). 6 rows for
     *         Clip-backed layers (2D set + Opacity), 9 rows for OA layers
     *         (full 3D Pos/Rot/Scale).
     */
    float renderClipPropertyPanel(entt::entity clipEntity, float rowY);

    /**
     * Handle right-click context menus.
     */
    void handleContextMenus();

    /**
     * Modal popup for "Change Media..." opened from the clip context menu.
     * Lets the user pick a different file from the project's media library
     * with a filter box. On selection enqueues a SetClipMediaCommand
     * against m_pendingChangeMediaClip.
     */
    void renderChangeMediaPopup();

    /**
     * Find which track is at the given Y position.
     * @return Track index or -1 if not over a track
     */
    int findTrackAtY(float mouseY, float windowY) const;

    /**
     * Render the dedicated cue lane band that sits *above* the time ruler.
     * Stacks overlapping cue labels into rows (greedy left-to-right,
     * lowest available row), draws each cue as a numbered flag with a
     * triangle pointer dropping down to the ruler tick.
     * @param laneOriginPos Top-left of the cue-lane band in screen space.
     * @param laneHeight    Pixel height of the cue lane (== getCueLaneHeight()).
     * @param rulerTopY     Screen-space Y of the ruler top, where pointer
     *                      triangles terminate.
     */
    void renderCueLane(ImVec2 laneOriginPos, float laneHeight, float rulerTopY);

    /**
     * Per-cue stacking decision used by both the renderer and the input
     * hit-test so they agree on which row a cue ends up in. Cheap to
     * recompute (cue counts are O(tens)).
     */
    struct CueLaneSlot {
        size_t   cueIndex{0};   // index into Timeline::getCueTags()
        int      row{0};        // 0..kMaxCueRows-1
        float    labelW{0.0f};  // measured pixel width of the rendered label
        bool     longLabel{true}; // false = fell back to "1.50" (over-stacked row cap)
    };

    /**
     * Run the stacking pass over the current cue list. Returns slot rows
     * and the total row count used (caller multiplies by kCueRowH for
     * lane height). Empty cue list -> empty result, rowsUsed=0.
     */
    void computeCueLaneLayout(std::vector<CueLaneSlot>& outSlots, int& outRowsUsed) const;

    /**
     * Cue lane layout constants. Public-private so renderer + input + the
     * top-level layout shift can all read them. Cap at 4 rows; beyond
     * that, we shrink labels to the smallest form (number-only).
     */
    static constexpr float kCueRowH    = 16.0f;
    static constexpr float kCueLanePad = 2.0f;
    static constexpr int   kMaxCueRows = 4;

    /**
     * Effective lane height for the current frame's cue layout.
     * Recomputed each render() before laying out the ruler.
     */
    float getCueLaneHeight() const { return m_cueLaneHeight; }

    /**
     * Modal popup for "Add Cue Here..." / "Edit Cue..." opened from the
     * ruler context menus. Reads m_cueModal* state, enqueues commands on OK.
     */
    void renderCueModal();

    /**
     * Modal popup for "Add Section Break Here..." / "Edit Break..." opened
     * from the ruler context menus. Reads m_sectionBreakModal* state and
     * enqueues AddSectionBreakCommand / EditSectionBreakCommand on OK.
     */
    void renderSectionBreakModal();

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
    static constexpr float TRACK_HEIGHT = 28.0f;
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
    // Frame on the timeline where the user right-clicked an empty track
    // area. Captured + floor-snapped to the active tick grid. Read by
    // the TrackContextMenu's Paste item.
    FrameNumber m_rightClickedTrackFrame{0};

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

    /** Snap a Timecode to the closest cue timestamp within `tol`. Skips the
     *  cue currently being dragged so a cue can't snap to itself. Returns
     *  `t` unchanged if no cue is within tolerance. */
    Timecode snapTimeToCues(Timecode t, Timecode tol) const;

    /** Snap a Timecode to the closest section break within `tol`. Returns
     *  `t` unchanged if no break is within tolerance. */
    Timecode snapTimeToSections(Timecode t, Timecode tol) const;

    /** Combined snap: grid (always-on) + nearest cue + nearest section
     *  break, returning whichever candidate is closest to `t`. Tolerance
     *  derived from kSnapPx pixels at current zoom. */
    Timecode snapTimeToBest(Timecode t) const;

    /** Snap tolerance in pixels for cue/section near-snap. Grid snap is
     *  always-on and ignores this. */
    static constexpr float kSnapPx = 6.0f;

    // Section break context-menu state. nullopt = nothing right-clicked;
    // otherwise the breakFrame of the right-clicked break (the vector
    // index isn't stable across edits, but the breakFrame is).
    std::optional<Timecode> m_rightClickedSectionBreakFrame;
    bool m_rangeContextMenuRequested{false};

    // Pending modal state for "Add Section Break Here..." / "Edit Break..."
    enum class SectionBreakModalMode { None, Add, Edit };
    SectionBreakModalMode m_sectionBreakModalMode{SectionBreakModalMode::None};
    bool      m_sectionBreakModalOpenRequested{false};
    Timecode  m_sectionBreakModalOldFrame{0};
    Timecode  m_sectionBreakModalFrame{0};
    uint32_t  m_sectionBreakModalColor{0xFF6090C8u};
    double    m_sectionBreakModalFadeSeconds{0.0};

    // Cue context-menu state. -1 means "no cue right-clicked"; otherwise
    // it's an index into Timeline::getCueTags(). Hit testing for cue flags
    // uses a small pixel tolerance around the rendered marker.
    int m_rightClickedCueIndex{-1};
    static constexpr float m_pixelToleranceCue = 8.0f;

    // Pending modal state for "Add Cue Here..." / "Edit Cue..." flows.
    // Mode + form fields all live here so the popup can survive across frames.
    enum class CueModalMode { None, Add, Edit };
    CueModalMode m_cueModalMode{CueModalMode::None};
    bool   m_cueModalOpenRequested{false};
    double m_cueModalOldNumber{0.0};
    double m_cueModalNumber{1.0};
    Timecode m_cueModalTimestamp{0};
    char   m_cueModalLabelBuf[256]{0};

    // Time captured when the user opens the right-click ruler menu (used
    // as the timestamp for "Add Cue Here..." when no cue/section/range
    // is under the cursor).
    Timecode m_rulerRightClickTime{0};

    // Cue-drag state (left-click on a cue flag in the lane drags it to a
    // new timestamp). Edit emitted on release if the frame moved by >1.
    bool       m_isDraggingCue{false};
    double     m_draggedCueNumber{0.0};
    Timecode   m_dragOriginalCueTime{0};
    Timecode   m_dragCurrentCueTime{0};

    // Cue lane height computed each render() (function of stacked rows).
    // 0 when there are no cues. Read by ruler/tracks layout to shift down.
    float m_cueLaneHeight{0.0f};

    // Index of the tick-division currently under the mouse on the ruler,
    // or -1 when the mouse is elsewhere. Computed in handleRulerInteraction()
    // and consumed by renderTimeRuler() to draw the hover highlight.
    int m_hoverDivisionIndex{-1};

    // Optional dispatcher for ruler-menu cue commands.
    CommandDispatcher* m_commandDispatcher{nullptr};

    // Optional Engine pointer. Used by the clip context menu for
    // clipboard state (Paste enable/disable) and by the Change Media
    // modal to walk the project's media library.
    Engine* m_engine{nullptr};

    // Change Media modal state. m_openChangeMediaPopup is set true by
    // the context menu's "Change Media..." item; handleContextMenus
    // calls ImGui::OpenPopup once and resets the flag on the same
    // frame so we don't keep reopening every tick.
    entt::entity m_pendingChangeMediaClip{entt::null};
    bool         m_openChangeMediaPopup{false};
    char         m_changeMediaFilterBuf[256]{0};

    // Callbacks
    MediaDropCallback m_mediaDropCallback;
    OALayerDropCallback m_oaLayerDropCallback;
    ClipLayerDropCallback m_clipLayerDropCallback;
    GenerativeLayerDropCallback m_generativeLayerDropCallback;

    // Track expansion state. Track twirl now drives property-panel display
    // for the clip currently under the playhead on that track — no separate
    // per-clip twirl/expansion. findClipAtPlayhead() resolves the clip.
    std::unordered_set<uint32_t> m_expandedTracks;
    static constexpr float PROPERTY_ROW_HEIGHT = 20.0f;  // Height of each property track row
    static constexpr float TRACK_HEADER_WIDTH = 200.0f;  // Width of track header panel
    static constexpr float HEADER_ROW_HEIGHT = 24.0f;    // Height of track/clip header rows (legacy; unused after track-only expansion)
    static constexpr float INDENT_WIDTH = 16.0f;         // Indentation for hierarchy levels
    float m_syncScrollY{0.0f};  // Synchronized vertical scroll position

    /** Find the clip on the given track whose timeline range contains the
     *  current playhead frame. Returns entt::null if no clip overlaps. Used
     *  to populate the property panel under an expanded track. */
    entt::entity findClipAtPlayhead(entt::entity trackEntity) const;

    /** Number of animatable channels shown when the layer at the playhead is
     *  expanded. 6 for Clip-backed layers (Pos X/Y, Scale X/Y, Rotation,
     *  Opacity), 9 for OA layers (Pos/Rot/Scale × X/Y/Z). Drives the
     *  expanded-track height computation in handleInteraction +
     *  findClipAtPosition. */
    std::size_t expandedPropertyRowCount(entt::entity layerEntity) const;

    // Keyframe editing state
    entt::entity m_keyframeEditClip{entt::null};
    AnimatableProperty m_keyframeEditProperty{AnimatableProperty::PositionX};
    FrameNumber m_keyframeEditFrame{0};
    bool m_showKeyframeContextMenu{false};

    // Pre-edit snapshot for the bezier-tangent DragFloat sliders in the
    // keyframe context menu. Captured on ImGui::IsItemActivated so we can
    // emit one undoable SetKeyframeInterpolationCommand on drag-end
    // (IsItemDeactivatedAfterEdit). m_kfEasingPreEdit.valid means a drag
    // is in progress; the easeIn/easeOut sliders share the same record
    // because they're co-located in the same popup pass.
    struct KeyframeEasingPreEdit {
        bool valid{false};
        InterpolationType interp{InterpolationType::Linear};
        float easeIn{0.42f};
        float easeOut{0.58f};
    };
    KeyframeEasingPreEdit m_kfEasingPreEdit;

    /** Draw one keyframe glyph at (cx, cy) with shape determined by the
     *  keyframe's interpolation type. Linear=diamond, Step=square,
     *  EaseIn=half-hourglass-left, EaseOut=half-hourglass-right,
     *  EaseInOut=full-hourglass. Used by both the top-of-clip cluster
     *  and the expanded property-track row so shapes stay consistent. */
    void drawKeyframeShape(ImDrawList* drawList,
                           const Keyframe& kf,
                           float cx, float cy,
                           float size) const;
};

} // namespace entity
