#pragma once

#include "entity/core/Types.hpp"
#include <entt/entt.hpp>
#include <vector>
#include <string>
#include <functional>
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

    /**
     * Get current frame number based on timeline position.
     * Converts current time (in microseconds) to frame number using frame rate.
     * @return Frame number (0-based)
     */
    FrameNumber getCurrentFrame() const {
        // Convert microseconds to seconds, multiply by frame rate
        double seconds = m_currentTime / 1000000.0;
        return static_cast<FrameNumber>(std::round(seconds * m_frameRate));
    }

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

    // Get registry reference
    entt::registry& getRegistry() { return m_registry; }
    const entt::registry& getRegistry() const { return m_registry; }

    // Selection management
    void setSelectedClip(entt::entity clip) { m_selectedClip = clip; }
    entt::entity getSelectedClip() const { return m_selectedClip; }

    // Expansion state (for twirl-down property tracks)
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
    double m_frameRate{30.0};

    // Track entities (stored in ECS)
    std::vector<entt::entity> m_tracks;

    // Selection state
    entt::entity m_selectedClip{entt::null};
    std::unordered_set<uint32_t> m_expandedClips;

    // Callback for clip creation
    ClipCreatedCallback m_clipCreatedCallback;
};

} // namespace entity
