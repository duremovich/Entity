#pragma once

#include "System.hpp"
#include "entity/core/Types.hpp"
#include <entt/entt.hpp>

namespace entity {

// Forward declarations
class Timeline;

/**
 * AnimationSystem - Evaluates keyframes and updates animated properties
 *
 * Queries entities with AnimatedProperties component,
 * evaluates keyframe tracks at the current timeline frame,
 * and updates Transform and MediaLayer components accordingly.
 *
 * Animation frames are relative to clip start, so a keyframe at frame 10
 * occurs 10 frames after the clip begins on the timeline.
 */
class AnimationSystem : public System {
public:
    AnimationSystem() = default;

    /**
     * Set the timeline for frame-accurate animation evaluation.
     */
    void setTimeline(Timeline* timeline) { m_timeline = timeline; }

    void initialize(entt::registry& registry) override;
    void update(entt::registry& registry, float deltaTime) override;
    void shutdown(entt::registry& registry) override;
    const char* getName() const override { return "AnimationSystem"; }

    /**
     * Enable/disable verbose debug logging.
     */
    void setDebugLogging(bool enabled) { m_debugLogging = enabled; }

private:
    Timeline* m_timeline{nullptr};
    bool m_debugLogging{false};
};

} // namespace entity
