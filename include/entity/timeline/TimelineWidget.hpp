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

// Forward declaration — TimelineWidget reads ParamSchema from the
// EffectKindRegistry to build effect-param rows for the AE-style
// hierarchy in the expanded-track view.
namespace effects { class EffectKindRegistry; struct ParamSchema; }

// Callback for when a media file is dropped onto a track
// Parameters: filepath, track index, timecode position
using MediaDropCallback = std::function<void(const std::string&, int, Timecode)>;

// Callback for when an OA layer kind is dropped onto a track
// Parameters: track index, start frame (timeline), duration in frames
using OALayerDropCallback = std::function<void(int, FrameNumber, FrameNumber)>;

// Callback for when a Clip layer kind is dropped onto a track (empty clip, no media)
// Parameters: track index, start frame (timeline), duration in frames
using ClipLayerDropCallback = std::function<void(int, FrameNumber, FrameNumber)>;

// Callback for when a Generative layer kind is dropped onto a track (Muncher sub-kind).
// Parameters: track index, start frame (timeline), duration in frames
using GenerativeLayerDropCallback = std::function<void(int, FrameNumber, FrameNumber)>;

// Callback for when a Text generative layer is dropped onto a track.
// Parameters: track index, start frame (timeline), duration in frames
using TextLayerDropCallback = std::function<void(int, FrameNumber, FrameNumber)>;

// Callback for when a Signal Output layer is dropped onto a track.
// Parameters: track index, start frame (timeline), duration in frames
using SignalLayerDropCallback = std::function<void(int, FrameNumber, FrameNumber)>;

// Lookup callback wired by Engine — given a logical media path, returns
// the media's timeline-frame duration (seconds × timeline fps). Used to
// size the drop ghost. Returns 0.0 when the path isn't probed yet; the
// ghost falls back to a default placeholder duration in that case.
using MediaDurationLookup = std::function<double(const std::string&)>;

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
     * what the snap actually was). Discrete-step zoom ladder.
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
    void setTextLayerDropCallback(TextLayerDropCallback callback) { m_textLayerDropCallback = std::move(callback); }
    void setSignalLayerDropCallback(SignalLayerDropCallback callback) { m_signalLayerDropCallback = std::move(callback); }

    /**
     * Optional — Engine-supplied lookup mapping a logical media path to
     * the media's timeline-frame duration. Drives the drop-ghost width
     * during MediaBin → Timeline drags. Without it, drag ghost uses the
     * 10-second placeholder default.
     */
    void setMediaDurationLookup(MediaDurationLookup lookup) { m_mediaDurationLookup = std::move(lookup); }

    /**
     * Optional — wire the CommandDispatcher so ruler-context-menu actions
     * (Phase A cue ops) can enqueue undoable commands. Without it, those
     * menu items are inert (the menu still draws).
     */
    void setCommandDispatcher(CommandDispatcher* dispatcher) { m_commandDispatcher = dispatcher; }
    /** Effect-kind registry pointer. Needed for the expanded-track
     *  view to surface effect-param schemas as rows under
     *  Effects > <kind> in the AE-style hierarchy. Optional —
     *  leaving null hides effect-param rows from the twirl-down. */
    void setEffectKindRegistry(const effects::EffectKindRegistry* reg) {
        m_effectKindRegistry = reg;
    }

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
     * Convert an integer timeline frame to a screen x position (pixels).
     * Used for frame-native markers (section breaks, cues).
     */
    float frameToPixel(FrameNumber frame) const;

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
     * Boolean variant of checkClipCollision used by drop-ghost validity
     * lighting. excludeEntity is skipped (pass m_selectedClip when
     * checking cross-track drag of an existing clip; entt::null for fresh
     * media/layer drops). Returns true if a clip on the given track
     * overlaps the [startTime, startTime+durationTime) window.
     */
    bool wouldOverlapAnyClip(int trackIndex, Timecode startTime, Timecode durationTime,
                             entt::entity excludeEntity) const;

    /**
     * Phase 13 group-drag variant: like wouldOverlapAnyClip but excludes a SET
     * of entities (the whole moving group) instead of one, so a group member
     * is only tested against NON-selected clips on its track. Returns true if
     * any non-excluded clip overlaps the window.
     */
    bool wouldOverlapAnyClipExcluding(int trackIndex, Timecode startTime,
                                      Timecode durationTime,
                                      const std::vector<entt::entity>& exclude) const;

    /** Model track index containing `clip`, or -1 if not on any track. */
    int findTrackIndexForClip(entt::entity clip) const;

    /**
     * Snap a raw drop time to the same candidate set the cue drag uses
     * (playhead, grid, cues, sections) at SNAP_THRESHOLD_PIXELS tolerance.
     * Drops use the same snap pipeline as cues — clip-edge snap is
     * deliberately omitted to keep drop visualizations from chattering at
     * track boundaries. Track index is currently unused but reserved for
     * a future track-local snap variant.
     */
    Timecode snapDropPosition(Timecode rawTime, int trackIndex) const;

    /**
     * Render the track header panel (left side with hierarchy).
     * This panel shows tracks with twirl-downs for clips and properties.
     * Scroll is handled by the slaved TrackHeadersInner child (Phase 3), so
     * this function does not take a scroll argument — it draws at the inner
     * child's already-scrolled cursor origin.
     * @param panelHeight Height of the panel
     */
    void renderTrackHeaderPanel(float panelHeight);

    /**
     * Render a single track row in the header panel.
     * @param trackEntity The track entity
     * @param trackIndex MODEL index (TrackRow::modelIndex), not visual row
     *        position — the "Track N" auto-name and command addressing both
     *        key off it. See TrackRow's comment.
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
     * Resolve a finished rubber-band drag (Phase 12) into a selection. Builds
     * the box from the scroll-stable anchor (m_boxAnchorFrame / ContentY) to
     * the current mouse, then selects every layer whose frame range AND track
     * row both intersect the box. m_boxAnchorCtrl=true unions with the existing
     * selection; otherwise it replaces. windowPos is the tracks child's scrolled
     * cursor origin (m_tracksScreenPos), same as the other hit-tests.
     */
    void commitBoxSelection(ImVec2 mousePos, ImVec2 windowPos);

    /**
     * A keyframe identified by its owning layer entity, animated property,
     * and clip-relative frame. `valid == false` means no keyframe was hit.
     */
    struct KeyframeHit {
        bool               valid{false};
        entt::entity       clip{entt::null};
        AnimatableProperty property{AnimatableProperty::PositionX};
        FrameNumber        frame{0};
    };

    /**
     * Hit-test the expanded property-track rows for a keyframe diamond at
     * mousePos. Walks tracks with the same cumulative-Y layout as
     * findClipAtPosition; the keyframe screen math mirrors
     * renderPropertyTracks so the hit zone matches what is drawn.
     */
    KeyframeHit findKeyframeAtPosition(ImVec2 mousePos, ImVec2 windowPos) const;

    /**
     * True if mousePos is inside any expanded track's property-panel Y band.
     * Used to stop a click in that band from being routed into a clip drag.
     */
    bool isInPropertyRowBand(ImVec2 mousePos, ImVec2 windowPos) const;

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
     * Render the "Insert/Delete Time Here..." modal. Reads m_rippleTimeModal*
     * state and enqueues RippleInsertTimeCommand / RippleDeleteTimeCommand on OK.
     */
    void renderRippleTimeModal();

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
    bool m_pendingScrollX{false};      // True for one render() after a programmatic horizontal
                                       // move (ensurePlayheadVisible / zoom re-anchor) wrote
                                       // m_syncScrollX. Routes the move through the tracks child's
                                       // SetNextWindowScroll (pre-Begin) and suppresses the
                                       // same-frame read-back so the just-applied target isn't
                                       // stomped by the pre-target GetScrollX.
    bool m_pendingScrollY{false};      // Symmetric to m_pendingScrollX for programmatic vertical
                                       // moves — currently set by the wheel-over-header forward
                                       // in render(). Same SetNextWindowScroll + read-back-suppress
                                       // path on the Y axis.

    // Recompute m_pixelsPerSecond from m_zoomIndex + Timeline frame rate.
    // Called at the top of render() so a frame-rate change picks up immediately.
    void applyZoomIndex();

    // Cached positions for rendering playhead across child windows
    ImVec2 m_rulerScreenPos{0, 0};
    ImVec2 m_tracksScreenPos{0, 0};
    float m_tracksHeight{0.0f};

    // ------------------------------------------------------------------------
    // Per-frame track-row layout cache (Phase 9 foundation).
    //
    // Every renderer and hit-test that walks tracks used to recompute the same
    // cumulative-Y layout inline (5+ copies). They now iterate m_trackRows,
    // rebuilt once at the top of render() by buildTrackRows(). This is the one
    // place that decides which tracks are VISIBLE: when the global "hide shy"
    // toggle is on, shy tracks (TrackUiState::shy) are omitted from the cache,
    // so they vanish from both the header panel and the lanes — and every
    // hit-test — without any per-consumer shy logic. Commands still address
    // tracks by `modelIndex` (the index into Timeline::getTracks()), which the
    // row carries; only presentation skips rows.
    // ------------------------------------------------------------------------
    struct TrackRow {
        entt::entity track{entt::null};
        int          modelIndex{0};        // index into Timeline::getTracks() — the MODEL
                                           // position, NOT the visual row position. Because
                                           // shy tracks are skipped, visual row N and model
                                           // index N can differ. Commands address tracks by
                                           // modelIndex, and the "Track N" auto-name is
                                           // derived from it (model position, per Phase 8) so
                                           // a track's default name doesn't shift when a shy
                                           // track above it is hidden.
        float        y{0.0f};              // content-space Y offset (top of the base row,
                                           // before adding a child's windowPos.y)
        float        height{0.0f};         // full row height incl. expanded property panel
                                           // (does NOT include the trailing TRACK_PADDING)
        bool         expanded{false};
        entt::entity clipAtPlayhead{entt::null};  // entt::null when no clip overlaps the playhead
        float        propPanelHeight{0.0f};       // 0 when not expanded / no overlapping clip
    };
    std::vector<TrackRow> m_trackRows;

    // Rebuild m_trackRows from the current track list, expansion state, and the
    // global hide-shy toggle. Called once at the top of render().
    void buildTrackRows();

    // Convenience: the row for a given model index, or nullptr if that track is
    // currently hidden (shy + global hide on) / out of range. Hit-tests that
    // need a specific track by model index use this.
    const TrackRow* rowForModelIndex(int modelIndex) const;

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

    // Multi-select (Phase 12). Pending-collapse: a plain (non-ctrl) click on a
    // clip that is ALREADY in the multi-selection keeps the whole set (so a
    // group drag can start) but, if the gesture turns out to be a click rather
    // than a drag, collapses the selection to just that clip on mouse-up.
    // m_pendingCollapseClip holds the clip to collapse to; entt::null = no
    // pending collapse.
    entt::entity m_pendingCollapseClip{entt::null};

    // Group drag (Phase 13, HORIZONTAL ONLY). When a drag starts with the
    // multi-selection > 1, every member moves rigidly by the grabbed clip's
    // delta. Vertical / cross-track is disabled for group drags (v1 scope cut),
    // and snap applies only to the grabbed clip (others stay rigid). Original
    // starts are captured at drag-start so the per-frame live-write is always
    // origin + delta (not incremental), and so a collision on release can
    // revert the whole group exactly.
    bool m_isGroupDragging{false};
    FrameNumber m_groupGrabbedOriginalStart{0};
    std::vector<std::pair<entt::entity, FrameNumber>> m_groupDragOriginalStarts;

    // Rubber-band box selection. Anchored in SCROLL-STABLE timeline space: the
    // X anchor is a frame, the Y anchor is the content-space pixel offset (row
    // y), so the box stays put under the content if the view scrolls mid-drag.
    bool        m_isBoxSelecting{false};
    bool        m_boxSelectArmed{false};   // mouse-down on empty lane; upgrades to
                                           // m_isBoxSelecting once drag crosses threshold
    FrameNumber m_boxAnchorFrame{0};
    float       m_boxAnchorContentY{0.0f};
    bool        m_boxAnchorCtrl{false};    // ctrl held at anchor -> union on release

    // Trimming state
    bool m_isTrimmingClip{false};
    ClipEdge m_trimEdge{ClipEdge::None};
    entt::entity m_trimClip{entt::null};
    FrameNumber m_trimOriginalStart{0};
    FrameNumber m_trimOriginalDuration{0};
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

    // Follow-playhead. When on, render() scrolls the view to keep the playhead
    // visible while the timeline is playing. m_followSuspended is set when the
    // user scrolls by hand mid-playback — otherwise the view snaps back the
    // instant they try to look ahead of the playhead — and is cleared on the next
    // play, seek, or toggle.
    bool m_followPlayhead{true};
    bool m_followSuspended{false};
    bool m_wasPlaying{false};       // play-edge detect, re-arms following on each play

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
    std::optional<FrameNumber> m_rightClickedSectionBreakFrame;
    bool m_rangeContextMenuRequested{false};

    // Pending modal state for "Add Section Break Here..." / "Edit Break..."
    enum class SectionBreakModalMode { None, Add, Edit };
    SectionBreakModalMode m_sectionBreakModalMode{SectionBreakModalMode::None};
    bool        m_sectionBreakModalOpenRequested{false};
    FrameNumber m_sectionBreakModalOldFrame{0};
    FrameNumber m_sectionBreakModalFrame{0};
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
    bool   m_cueModalAlsoAddSection{false};  // true when opened via "Add Section + Cue Here..."
    double m_cueModalOldNumber{0.0};
    double m_cueModalNumber{1.0};
    FrameNumber m_cueModalFrame{0};
    char   m_cueModalLabelBuf[256]{0};

    // Pending modal state for "Insert Time Here..." / "Delete Time Here..."
    // launched from TrackContextMenu or RulerContextMenu (no active range).
    enum class RippleTimeModalMode { None, Insert, Delete };
    RippleTimeModalMode m_rippleTimeModalMode{RippleTimeModalMode::None};
    bool        m_rippleTimeModalOpenRequested{false};
    FrameNumber m_rippleTimeModalFrame{0};     // anchor frame (right-click position)
    FrameNumber m_rippleTimeModalDuration{1};  // duration for Insert; delete end = frame + duration

    // Time captured when the user opens the right-click ruler menu (used
    // as the timestamp for "Add Cue Here..." when no cue/section/range
    // is under the cursor).
    Timecode m_rulerRightClickTime{0};

    // Cue gesture state (left-click on a cue label/triangle in the lane).
    // Two-stage to disambiguate single-click vs drag (issue 2026-05-25):
    //   - On mouse-down inside a cue's label rect we record a click
    //     candidate (m_cueClickCandidate = cue.number). m_isDraggingCue
    //     stays false.
    //   - When the gesture crosses io.MouseDragThreshold (default 6 px)
    //     we upgrade: clear m_cueClickCandidate, set m_isDraggingCue +
    //     m_draggedCueNumber, run the existing snap+commit path.
    //   - On release without ever crossing the threshold we seek the
    //     playhead to the cue's frame and emit no command. The cue
    //     itself does NOT move; drag-from-the-label rect is preserved.
    // EditCueCommand still only fires on release after a real drag if
    // the frame moved by >1.
    std::optional<double> m_cueClickCandidate;   // cue.number, set on mouse-down
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
    const effects::EffectKindRegistry* m_effectKindRegistry{nullptr};

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

    // Pre-drag capture for the master gain slider. Populated on
    // IsItemActivated so undo restores the value before the drag started,
    // not the penultimate-frame value (which drifts during live preview).
    float m_preEditMasterGain{1.0f};
    char  m_changeMediaFilterBuf[256]{0};

    // Pre-edit capture for twirldown property-panel DragFloat widgets
    // (renderClipPropertyPanel). One slot — only one DragFloat can be
    // active at a time. Keyed by entity + row identity so the wrong
    // slot isn't consumed if the panel scrolls between Activated and
    // DeactivatedAfterEdit.
    struct TwirldownPreEdit {
        entt::entity entity{entt::null};
        // For Transform rows: AnimatableProperty (cast to int for storage).
        // For EffectParam rows: paramHash. Together with entity they
        // uniquely identify which row owns this capture.
        int          propKey{0};      // AnimatableProperty (Transform) or paramHash (Effect)
        bool         isEffect{false};

        // Pre-drag scalar value — restored by undo when wasKeyframed=false.
        float        scalarValue{0.0f};

        // Keyframe state captured on IsItemActivated.
        bool                  wasKeyframed{false};
        FrameNumber           keyframeFrame{0};
        std::optional<float>  keyframeValue;  // nullopt = no kf existed at frame

        // Scale axes dragged along by the uniform-scale lock (ScaleLock). A
        // locked Scale X drag also writes Scale Y and Z, so their pre-edit
        // keyframe state is captured too and each animated axis commits its own
        // UpsertKeyframeCommand. Empty when the row isn't a scale axis or the
        // lock is off.
        struct LinkedAxis {
            AnimatableProperty   prop{AnimatableProperty::ScaleX};
            bool                 wasKeyframed{false};
            std::optional<float> keyframeValue;  // nullopt = no kf existed at frame
        };
        std::vector<LinkedAxis> linkedScaleAxes;
    };
    TwirldownPreEdit m_twirldownPreEdit;

    // Inline track-rename state. -1 = no rename in progress.
    // m_renamingTrackIndex is set on double-click of the name region or
    // "Rename Track..." from the context menu. The InputText overlay is
    // drawn in renderTrackHeaderRow; Enter/deactivate commits via
    // RenameTrackCommand; Esc cancels (clears to -1 without command).
    //
    // Focus protocol (mirrors MathInput.cpp justEntered/wasActive shape):
    //   m_renameFocusPending — set true on entry; renderTrackHeaderRow
    //     calls SetKeyboardFocusHere() before the InputText and clears
    //     it on the SAME frame, then skips the commit check that frame
    //     so the never-yet-active state doesn't trigger an instant commit.
    //   m_renameWasActive — tracks whether IsItemActive() was ever true
    //     for this InputText session; focus-loss commit only fires after
    //     the widget was at least once active.
    //   m_renameTrackCount — track count captured on entry; if it changes
    //     while a rename is open the rename is silently cancelled (stale
    //     positional index guard).
    int    m_renamingTrackIndex{-1};
    char   m_renameTrackBuf[256]{0};
    bool   m_renameFocusPending{false};
    bool   m_renameWasActive{false};
    size_t m_renameTrackCount{0};

    // Callbacks
    MediaDropCallback m_mediaDropCallback;
    OALayerDropCallback m_oaLayerDropCallback;
    ClipLayerDropCallback m_clipLayerDropCallback;
    GenerativeLayerDropCallback m_generativeLayerDropCallback;
    TextLayerDropCallback m_textLayerDropCallback;
    SignalLayerDropCallback m_signalLayerDropCallback;
    MediaDurationLookup m_mediaDurationLookup;

    /**
     * Live state for the drop ghost overlay. Set during clip drag (when
     * cursor crosses to a different track) and during ImGui drag-drop
     * peek on the timeline target. Read by renderDropGhost().
     *
     * `valid` controls border color — false → red (overlap, no gap on
     * target track); true → blue (clean drop). The clip is still allowed
     * to commit when valid=false; checkClipCollision snaps it into the
     * nearest gap. Red is a warning, not a hard veto.
     */
    struct GhostPreview {
        bool        active{false};
        int         trackIndex{-1};
        Timecode    startTime{0};
        Timecode    duration{0};
        bool        valid{true};
        std::string label;
    };
    GhostPreview m_ghost;

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

    /** One row in the expanded-track view. AE-style two-level hierarchy:
     *  group headers ("Transform" / "Effects" / "<effect kind>") are
     *  interleaved with property rows. The visible-only list is what
     *  propertyListForEntity returns — collapsed-group children are
     *  filtered out, so all consumers (sidebar header, right-pane
     *  diamonds, keyframe hit-test) iterate the same shape. */
    struct TimelinePropertyDef {
        enum class Source { Transform, EffectParam, GroupHeader };
        Source             source{Source::Transform};

        /** Indent depth (0 = top-level group, 1 = property inside one,
         *  2 = property inside a nested group like Effects > <kind>). */
        int                depth{0};

        /** Source::GroupHeader: "Transform" or "Effects/Brightness & Contrast"
         *  (slash-joined canonical path used as the collapse-state key).
         *  Empty for property rows. */
        std::string        groupPath;
        /** Source::GroupHeader: display label without parents (e.g.
         *  "Brightness & Contrast"). Empty for property rows. */
        std::string        groupLabel;

        /** Source::Transform / Source::EffectParam: the row's display
         *  name ("Pos X", "Brightness"). Empty for group headers. */
        std::string        shortName;
        /** Source::Transform / Source::EffectParam: fallback value when
         *  no keyframe track exists. */
        float              defaultValue{0.0f};

        /** Source::Transform: the channel this row represents. */
        AnimatableProperty prop{AnimatableProperty::PositionX};

        /** Source::EffectParam: which effect entity owns the track and
         *  what FNV-1a hash to look up in EffectAnimatedParameters.
         *  paramName is the wire-stable string form (for command dispatch). */
        entt::entity       effectEntity{entt::null};
        std::uint32_t      paramHash{0};
        std::string        paramName;

        bool isGroupHeader() const { return source == Source::GroupHeader; }
        bool isPropertyRow() const { return !isGroupHeader(); }
    };

    /** Visible rows shown under an expanded layer. Walks Transform / OA
     *  channels for the layer's archetype, then EffectChain → per-effect
     *  param schemas. Group headers ("Transform" / "Effects" / "<kind>")
     *  are interleaved; rows inside collapsed groups are filtered out so
     *  all consumers iterate the same flat visible list. Shared by
     *  renderPropertyTracks (body), renderClipPropertyPanel (header) and
     *  findKeyframeAtPosition. */
    std::vector<TimelinePropertyDef> propertyListForEntity(entt::entity e) const;

    /** True iff the group at this canonical path is currently collapsed
     *  for this layer. Default = expanded. */
    bool isGroupCollapsed(entt::entity layerEntity,
                          const std::string& groupPath) const;
    /** Toggle the collapse state of a group on a layer. */
    void toggleGroupCollapsed(entt::entity layerEntity,
                              const std::string& groupPath);

    // Keyframe editing state
    entt::entity m_keyframeEditClip{entt::null};
    AnimatableProperty m_keyframeEditProperty{AnimatableProperty::PositionX};
    FrameNumber m_keyframeEditFrame{0};
    bool m_showKeyframeContextMenu{false};

    // Keyframe selection (single-select). entt::null clip = nothing selected.
    // The identity of a keyframe is (clip, property, clip-relative frame).
    entt::entity       m_selectedKeyframeClip{entt::null};
    AnimatableProperty m_selectedKeyframeProperty{AnimatableProperty::PositionX};
    FrameNumber        m_selectedKeyframeFrame{0};

    // Keyframe drag-to-move state. The keyframe data is NOT mutated during the
    // drag — m_dragKeyframeCurrentFrame drives a live preview, and a single
    // undoable MoveKeyframeCommand is committed on release.
    bool               m_isDraggingKeyframe{false};
    entt::entity       m_dragKeyframeClip{entt::null};
    AnimatableProperty m_dragKeyframeProperty{AnimatableProperty::PositionX};
    FrameNumber        m_dragKeyframeOriginalFrame{0};  // clip-relative, fixed
    FrameNumber        m_dragKeyframeCurrentFrame{0};   // clip-relative, live

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
