#pragma once

#include "entity/core/Types.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/timeline/CueTag.hpp"
#include <entt/entt.hpp>
#include <vector>
#include <string>
#include <functional>
#include <optional>
#include <unordered_set>
#include <cmath>
#include <atomic>

namespace entity {

// Callback for when a new clip is created (split, duplicate)
// Parameters: new clip entity, source clip filepath
using ClipCreatedCallback = std::function<void(entt::entity, const std::string&)>;

/**
 * PlaybackState - Current state of timeline playback
 */
enum class PlaybackState {
    Stopped,
    Playing,
    Paused
};

/**
 * Timeline - Manages timeline state, tracks, and playback
 *
 * The Timeline class manages:
 * - Current playback position and state
 * - List of track entities (stored in ECS with TimelineTrack component)
 * - Timeline duration and frame rate
 * - Playback control (play, pause, stop, seek)
 *
 * This is a non-ECS class that works alongside the ECS registry.
 * Tracks are represented as entities with TimelineTrack components.
 */
class Timeline {
public:
    Timeline(entt::registry& registry);
    ~Timeline() = default;

    // Non-copyable
    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    /**
     * Update timeline (should be called every frame).
     * Advances current time if playing.
     * @param deltaTime Time elapsed since last frame (seconds)
     */
    void update(double deltaTime);

    // Playback control
    void play();
    void pause();
    void stop();
    void seek(Timecode time);

    // Time getters
    Timecode getCurrentTime() const { return m_currentTime; }
    Timecode getDuration() const { return m_duration; }
    PlaybackState getPlaybackState() const { return m_playbackState.load(); }
    double getFrameRate() const { return m_frameRate; }

    // Scrubbing mode - when true, DecodeSystem won't trigger seeks (prevents buffer thrashing)
    void setScrubbing(bool scrubbing) { m_isScrubbing.store(scrubbing); }
    bool isScrubbing() const { return m_isScrubbing.load(); }

    /**
     * Convert a frame number to a Timecode (microseconds).
     * Uses std::round so seekToFrame(N) -> getCurrentFrame() round-trips to N.
     */
    Timecode frameToTime(FrameNumber frame) const {
        return static_cast<Timecode>(std::round((frame / m_frameRate) * 1000000.0));
    }

    /**
     * Convert a Timecode (microseconds) to a frame number.
     * Rounds to nearest. Single source of truth for time->frame across the app.
     */
    FrameNumber timeToFrame(Timecode time) const {
        return static_cast<FrameNumber>(std::round((time / 1000000.0) * m_frameRate));
    }

    /**
     * Get current frame number based on timeline position.
     */
    FrameNumber getCurrentFrame() const { return timeToFrame(m_currentTime); }

    /**
     * Seek to a specific integer frame. Round-trip safe with getCurrentFrame().
     */
    void seekToFrame(FrameNumber frame) { seek(frameToTime(frame)); }

    // Time setters
    void setDuration(Timecode duration) { m_duration = duration; }
    void setFrameRate(double frameRate) { m_frameRate = frameRate; }

    // Track management
    entt::entity createTrack(const std::string& name);
    void deleteTrack(entt::entity track);
    const std::vector<entt::entity>& getTracks() const { return m_tracks; }
    size_t getTrackCount() const { return m_tracks.size(); }

    /**
     * Clear all tracks and clips from the timeline.
     * Used when loading a new project.
     */
    void clear();

    /**
     * Delete a clip from its track.
     * Removes clip from track's clip list and destroys the entity.
     */
    void deleteClip(entt::entity clipEntity);

    /**
     * Split a clip at the given frame.
     * Creates a new clip for the portion after the split point.
     * @param clipEntity The clip to split
     * @param splitFrame The frame number to split at (relative to timeline)
     * @return The new clip entity (right side), or entt::null if split failed
     */
    entt::entity splitClip(entt::entity clipEntity, FrameNumber splitFrame);

    /**
     * Duplicate a clip.
     * Creates a copy positioned immediately after the original.
     * @param clipEntity The clip to duplicate
     * @return The new clip entity, or entt::null if duplication failed
     */
    entt::entity duplicateClip(entt::entity clipEntity);

    /**
     * Find which track contains a clip.
     * @return Track entity containing the clip, or entt::null if not found
     */
    entt::entity findTrackForClip(entt::entity clipEntity) const;

    /**
     * Move a clip from one track to another.
     * @param clipEntity The clip to move
     * @param newTrackIndex The target track index
     * @return True if move was successful
     */
    bool moveClipToTrack(entt::entity clipEntity, int newTrackIndex);

    // ============================================================================
    // Ripple time edits (Phase C #4) — undoable via the captured records below.
    // Helpers operate directly on the registry so they can be unit-tested
    // without spinning up an Engine; the matching Commands wrap these and
    // own the captured-state vectors for undo.
    // ============================================================================

    struct ClipShiftRecord {
        entt::entity entity{entt::null};
        FrameNumber oldStartFrame{0};
    };
    struct ClipSplitRecord {
        entt::entity originalEntity{entt::null};
        FrameNumber oldDuration{0};
        bool hadAnimProps{false};
        AnimatedProperties oldAnimProps{};
        entt::entity newRightEntity{entt::null};
    };
    struct RippleInsertResult {
        bool success{false};
        std::vector<ClipShiftRecord> shifted;
        std::vector<ClipSplitRecord> splits;
    };
    struct RippleDeleteResult {
        bool success{false};
        std::vector<ClipShiftRecord> shifted;
    };

    /**
     * Insert `durationFrames` of empty time at `insertFrame`. Splits any clip
     * that spans `insertFrame` and shifts every clip starting at or after
     * `insertFrame` by `+durationFrames`. Capture the returned record to undo.
     */
    RippleInsertResult rippleInsertTime(FrameNumber insertFrame, FrameNumber durationFrames);
    void undoRippleInsertTime(RippleInsertResult& record);

    /**
     * Delete the time span `[rangeStart, rangeEnd)` from every track. v1
     * limitation: refuses if any clip overlaps the range — caller must split
     * or move overlapping clips first. Clips entirely after the range shift
     * left by `(rangeEnd - rangeStart)`. Capture the returned record to undo.
     */
    RippleDeleteResult rippleDeleteTime(FrameNumber rangeStart, FrameNumber rangeEnd);
    void undoRippleDeleteTime(RippleDeleteResult& record);

    // Get registry reference
    entt::registry& getRegistry() { return m_registry; }
    const entt::registry& getRegistry() const { return m_registry; }

    // ============================================================================
    // Named time-range sections (Phase C #5). User-named labelled spans on the
    // timeline used for show structure ("Verse 1", "Chorus", "Drop"). Persists
    // through ProjectSerializer at PROJECT_VERSION 3.
    // ============================================================================
    struct Section {
        std::string name;
        Timecode start{0};
        Timecode end{0};
        uint32_t color{0xFF6090C8};  // ImU32 (ABGR), default cool blue
    };
    const std::vector<Section>& getSections() const { return m_sections; }
    std::vector<Section>& getMutableSections() { return m_sections; }
    void addSection(Section s) { m_sections.push_back(std::move(s)); }
    void removeSection(size_t index) {
        if (index < m_sections.size()) m_sections.erase(m_sections.begin() + index);
    }
    void clearSections() { m_sections.clear(); }

    // ============================================================================
    // Cue tags — numbered timeline markers fired by command to seek-and-play.
    // Sorted ascending by `number`; uniqueness enforced on insert/edit.
    // ============================================================================
    const std::vector<CueTag>& getCueTags() const { return m_cueTags; }

    /** Insert a cue. Returns false if a cue with the same `number` already
     *  exists (no-op in that case). Sorted insert via lower_bound. */
    bool addCueTag(CueTag tag);

    /** Remove the cue with the given number. Returns true if a cue was
     *  removed. */
    bool removeCueTag(double number);

    /** Look up a cue by number. Returns nullptr if absent. */
    const CueTag* findCueTag(double number) const;

    /** Edit an existing cue. If `newNumber != oldNumber`, fails when
     *  another cue already uses `newNumber`. Re-sorts the vector if the
     *  number changed. Returns true on success. */
    bool editCueTag(double oldNumber, double newNumber,
                    Timecode newTimestamp, std::string newLabel);

    /** Drop every cue. Used by Timeline::clear() and the project loader. */
    void clearCueTags() { m_cueTags.clear(); }

    // Selection management
    void setSelectedClip(entt::entity clip) {
        m_selectedClip = clip;
        if (clip != entt::null) m_selectedCueNumber.reset();
    }
    entt::entity getSelectedClip() const { return m_selectedClip; }

    void setSelectedScreen(entt::entity screen) {
        m_selectedScreen = screen;
        m_selectedProjector = entt::null;
        if (screen != entt::null) m_selectedCueNumber.reset();
    }
    entt::entity getSelectedScreen() const { return m_selectedScreen; }

    void setSelectedProjector(entt::entity projector) {
        m_selectedProjector = projector;
        m_selectedScreen = entt::null;
        if (projector != entt::null) m_selectedCueNumber.reset();
    }
    entt::entity getSelectedProjector() const { return m_selectedProjector; }

    // Cue selection — mutually exclusive with clip/screen/projector. Setting
    // a cue clears the others; setting any of the others clears this.
    void setSelectedCueNumber(std::optional<double> number) {
        m_selectedCueNumber = number;
        if (number.has_value()) {
            m_selectedClip = entt::null;
            m_selectedScreen = entt::null;
            m_selectedProjector = entt::null;
        }
    }
    std::optional<double> getSelectedCueNumber() const { return m_selectedCueNumber; }

    // Clear all selection (deselect clip, screen, projector, and cue)
    void clearSelection() {
        m_selectedClip = entt::null;
        m_selectedScreen = entt::null;
        m_selectedProjector = entt::null;
        m_selectedCueNumber.reset();
    }

    // Expansion state (for twirl-down property tracks)
    void setTrackExpanded(entt::entity track, bool expanded) {
        if (expanded) {
            m_expandedTracks.insert(static_cast<uint32_t>(track));
        } else {
            m_expandedTracks.erase(static_cast<uint32_t>(track));
        }
    }
    bool isTrackExpanded(entt::entity track) const {
        return m_expandedTracks.count(static_cast<uint32_t>(track)) > 0;
    }
    void setClipExpanded(entt::entity clip, bool expanded) {
        if (expanded) {
            m_expandedClips.insert(static_cast<uint32_t>(clip));
        } else {
            m_expandedClips.erase(static_cast<uint32_t>(clip));
        }
    }
    bool isClipExpanded(entt::entity clip) const {
        return m_expandedClips.count(static_cast<uint32_t>(clip)) > 0;
    }

    /**
     * Set callback for when new clips are created (split, duplicate).
     * Engine uses this to create decoders and GPU resources.
     */
    void setClipCreatedCallback(ClipCreatedCallback callback) { m_clipCreatedCallback = callback; }

private:
    entt::registry& m_registry;

    // Timeline state
    Timecode m_currentTime{0};
    Timecode m_duration{600000000};  // Default: 10 minutes = 600 seconds = 600,000,000 microseconds
    std::atomic<PlaybackState> m_playbackState{PlaybackState::Stopped};
    std::atomic<bool> m_isScrubbing{false};  // True during drag-scrubbing (prevents decoder seeks)
    double m_frameRate{30.0};

    // Track entities (stored in ECS)
    std::vector<entt::entity> m_tracks;

    // Named time-range sections, persisted in project files.
    std::vector<Section> m_sections;

    // Numbered cue markers, sorted ascending by `number`. Persisted at v9.
    std::vector<CueTag> m_cueTags;

    // Selection state
    entt::entity m_selectedClip{entt::null};
    entt::entity m_selectedScreen{entt::null};
    entt::entity m_selectedProjector{entt::null};
    std::optional<double> m_selectedCueNumber;
    std::unordered_set<uint32_t> m_expandedTracks;
    std::unordered_set<uint32_t> m_expandedClips;

    // Callback for clip creation
    ClipCreatedCallback m_clipCreatedCallback;
};

} // namespace entity
