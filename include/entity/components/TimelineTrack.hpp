#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>
#include <entt/entt.hpp>
#include "Clip.hpp"

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
     * Note: Call sortClips() after adding clips to maintain sorted order.
     */
    void addClip(entt::entity clipEntity) {
        clips.push_back(clipEntity);
    }

    /**
     * Sort clips by their start frame.
     * Should be called after adding clips to ensure proper ordering.
     */
    void sortClips(entt::registry& registry) {
        std::sort(clips.begin(), clips.end(), [&registry](entt::entity a, entt::entity b) {
            auto* clipA = registry.try_get<Clip>(a);
            auto* clipB = registry.try_get<Clip>(b);
            if (!clipA || !clipB) return false;
            return clipA->startFrame < clipB->startFrame;
        });
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
