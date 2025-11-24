#pragma once

#include "entity/core/Types.hpp"
#include <entt/entt.hpp>
#include <vector>
#include <string>

namespace entity {

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
    PlaybackState getPlaybackState() const { return m_playbackState; }
    double getFrameRate() const { return m_frameRate; }

    /**
     * Get current frame number based on timeline position.
     * Converts current time (in microseconds) to frame number using frame rate.
     * @return Frame number (0-based)
     */
    FrameNumber getCurrentFrame() const {
        // Convert microseconds to seconds, multiply by frame rate
        double seconds = m_currentTime / 1000000.0;
        return static_cast<FrameNumber>(seconds * m_frameRate);
    }

    // Time setters
    void setDuration(Timecode duration) { m_duration = duration; }
    void setFrameRate(double frameRate) { m_frameRate = frameRate; }

    // Track management
    entt::entity createTrack(const std::string& name);
    void deleteTrack(entt::entity track);
    const std::vector<entt::entity>& getTracks() const { return m_tracks; }
    size_t getTrackCount() const { return m_tracks.size(); }

    // Get registry reference
    entt::registry& getRegistry() { return m_registry; }
    const entt::registry& getRegistry() const { return m_registry; }

private:
    entt::registry& m_registry;

    // Timeline state
    Timecode m_currentTime{0};
    Timecode m_duration{90000};  // Default: 90 seconds at 1000 ticks/sec = 90000
    PlaybackState m_playbackState{PlaybackState::Stopped};
    double m_frameRate{30.0};

    // Track entities (stored in ECS)
    std::vector<entt::entity> m_tracks;
};

} // namespace entity
