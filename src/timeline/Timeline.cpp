/**
 * Timeline Implementation
 *
 * Manages timeline state, playback, and track organization.
 */

#include "entity/timeline/Timeline.hpp"
#include "entity/components/TimelineTrack.hpp"
#include <iostream>

namespace entity {

Timeline::Timeline(entt::registry& registry)
    : m_registry(registry)
{
    std::cout << "[Timeline] Created" << std::endl;
}

void Timeline::update(double deltaTime) {
    if (m_playbackState == PlaybackState::Playing) {
        // Advance current time based on deltaTime
        Timecode deltaTimecode = static_cast<Timecode>(deltaTime * 1000.0); // Convert seconds to milliseconds
        m_currentTime += deltaTimecode;

        // Clamp to duration
        if (m_currentTime > m_duration) {
            m_currentTime = m_duration;
            m_playbackState = PlaybackState::Stopped; // Stop at end
            std::cout << "[Timeline] Reached end, stopping playback" << std::endl;
        }
    }
}

void Timeline::play() {
    if (m_playbackState != PlaybackState::Playing) {
        m_playbackState = PlaybackState::Playing;
        std::cout << "[Timeline] Playing from " << m_currentTime << std::endl;
    }
}

void Timeline::pause() {
    if (m_playbackState == PlaybackState::Playing) {
        m_playbackState = PlaybackState::Paused;
        std::cout << "[Timeline] Paused at " << m_currentTime << std::endl;
    }
}

void Timeline::stop() {
    m_playbackState = PlaybackState::Stopped;
    m_currentTime = 0;
    std::cout << "[Timeline] Stopped, reset to 0" << std::endl;
}

void Timeline::seek(Timecode time) {
    m_currentTime = time;

    // Clamp to valid range
    if (m_currentTime < 0) {
        m_currentTime = 0;
    }
    if (m_currentTime > m_duration) {
        m_currentTime = m_duration;
    }

    std::cout << "[Timeline] Seek to " << m_currentTime << std::endl;
}

entt::entity Timeline::createTrack(const std::string& name) {
    // Create track entity
    entt::entity trackEntity = m_registry.create();

    // Add TimelineTrack component
    auto& track = m_registry.emplace<TimelineTrack>(trackEntity);
    track.trackIndex = static_cast<uint32_t>(m_tracks.size());

    // Add to tracks list
    m_tracks.push_back(trackEntity);

    std::cout << "[Timeline] Created track " << track.trackIndex
              << " (" << name << "), entity=" << static_cast<uint32_t>(trackEntity) << std::endl;

    return trackEntity;
}

void Timeline::deleteTrack(entt::entity track) {
    // Remove from tracks list
    auto it = std::find(m_tracks.begin(), m_tracks.end(), track);
    if (it != m_tracks.end()) {
        m_tracks.erase(it);

        // Get track component to access clips
        if (m_registry.valid(track)) {
            auto* trackComponent = m_registry.try_get<TimelineTrack>(track);
            if (trackComponent) {
                // Delete all clips in this track
                for (entt::entity clipEntity : trackComponent->clips) {
                    if (m_registry.valid(clipEntity)) {
                        m_registry.destroy(clipEntity);
                    }
                }
            }

            // Delete track entity
            m_registry.destroy(track);

            std::cout << "[Timeline] Deleted track entity=" << static_cast<uint32_t>(track) << std::endl;
        }

        // Reindex remaining tracks
        for (size_t i = 0; i < m_tracks.size(); ++i) {
            if (m_registry.valid(m_tracks[i])) {
                auto* trackComponent = m_registry.try_get<TimelineTrack>(m_tracks[i]);
                if (trackComponent) {
                    trackComponent->trackIndex = static_cast<uint32_t>(i);
                }
            }
        }
    }
}

} // namespace entity
