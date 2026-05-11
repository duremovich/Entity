#pragma once

#include <vector>
#include <cstdint>
#include <algorithm>
#include <entt/entt.hpp>
#include "Clip.hpp"
#include "Layer.hpp"

namespace entity {

/**
 * TimelineTrack component for organizing layers (clips, object animations, etc.) into tracks.
 *
 * A track is a horizontal row on the timeline that can contain
 * multiple layer entities. Layers within a track cannot overlap.
 */
struct TimelineTrack {
    uint32_t trackIndex{0};              // Track number (0-based)
    std::vector<entt::entity> layers;    // Layer entities in this track (sorted by start frame)

    void addLayer(entt::entity layerEntity) {
        layers.push_back(layerEntity);
    }

    // Sort layers by start frame. All entities carry Layer post-3.2.
    void sortLayers(entt::registry& registry) {
        std::sort(layers.begin(), layers.end(), [&registry](entt::entity a, entt::entity b) {
            const auto* layA = registry.try_get<Layer>(a);
            const auto* layB = registry.try_get<Layer>(b);
            if (!layA || !layB) return false;  // degenerate: preserve order
            return layA->startFrame < layB->startFrame;
        });
    }

    void removeLayer(entt::entity layerEntity) {
        layers.erase(
            std::remove(layers.begin(), layers.end(), layerEntity),
            layers.end()
        );
    }

    size_t getLayerCount() const {
        return layers.size();
    }

    bool isEmpty() const {
        return layers.empty();
    }
};

} // namespace entity
