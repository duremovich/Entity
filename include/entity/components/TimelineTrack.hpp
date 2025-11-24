#pragma once

#include <vector>
#include <cstdint>
#include <entt/entt.hpp>

namespace entity {

/**
 * TimelineTrack component for organizing clips into tracks.
 *
 * A track is a horizontal row on the timeline that can contain
 * multiple clips. Clips within a track cannot overlap.
 */
struct TimelineTrack {
    uint32_t trackIndex{0};             // Track number (0-based)
    std::vector<entt::entity> clips;    // Clip entities in this track (sorted by start time)

    /**
     * Add a clip entity to this track.
     * Clips are kept sorted by start time.
     */
    void addClip(entt::entity clipEntity) {
        clips.push_back(clipEntity);
        // Note: Sorting should be done by the TimelineSystem
    }

    /**
     * Remove a clip entity from this track.
     */
    void removeClip(entt::entity clipEntity) {
        clips.erase(
            std::remove(clips.begin(), clips.end(), clipEntity),
            clips.end()
        );
    }

    /**
     * Get number of clips in this track.
     */
    size_t getClipCount() const {
        return clips.size();
    }

    /**
     * Check if track is empty.
     */
    bool isEmpty() const {
        return clips.empty();
    }
};

} // namespace entity
