#pragma once

#include "entity/systems/System.hpp"
#include "entity/components/Clip.hpp"
#include "entity/core/Types.hpp"

namespace entity {

/**
 * TimelineSystem - Manages timeline logic and frame mapping.
 *
 * Extracted from Clip component to follow ECS/DOD principles.
 * Handles frame number conversions and timeline queries.
 *
 * This system provides pure logic operations on Clip component data
 * without modifying component state. Other systems (DecodeSystem,
 * etc.) use these methods to query timeline-clip relationships.
 *
 * Design Notes:
 * - Components are pure data (no methods)
 * - Systems contain logic (operate on component data)
 * - Timeline queries are stateless transformations
 * - No per-frame update needed (on-demand queries only)
 */
class TimelineSystem : public System {
public:
    /**
     * TimelineSystem doesn't need per-frame updates.
     * Logic is called on-demand by other systems (DecodeSystem, etc.)
     */
    void update(entt::registry& registry, float deltaTime) override {
        // No per-frame work - this system provides utility methods
    }

    /**
     * Get system name for debugging and logging.
     */
    const char* getName() const override {
        return "TimelineSystem";
    }

    /**
     * Check if a timeline frame falls within this clip's range.
     * Extracted from Clip::containsFrame()
     *
     * @param clip The clip component to query
     * @param timelineFrame The timeline frame number to check
     * @return true if the frame is within [startFrame, startFrame + duration)
     */
    bool containsFrame(const Clip& clip, FrameNumber timelineFrame) const {
        return timelineFrame >= clip.startFrame &&
               timelineFrame < (clip.startFrame + clip.duration);
    }

    /**
     * Map timeline frame to media frame number.
     * Extracted from Clip::getMediaFrame()
     *
     * @param clip The clip component to query
     * @param timelineFrame The timeline frame number to map
     * @return Media frame number, or INVALID_FRAME if out of range
     *
     * Example: If clip starts at timeline frame 100 with mediaStartFrame 0,
     *          timeline frame 105 maps to media frame 5.
     */
    FrameNumber mapToMediaFrame(const Clip& clip, FrameNumber timelineFrame) const {
        if (!containsFrame(clip, timelineFrame)) {
            return INVALID_FRAME;
        }
        return clip.mediaStartFrame + (timelineFrame - clip.startFrame);
    }
};

} // namespace entity
