#pragma once

#include "System.hpp"
#include "entity/core/Types.hpp"
#include <entt/entt.hpp>

namespace entity {

// Forward declarations
class Timeline;
namespace effects { class EffectKindRegistry; }

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

    /**
     * Set the effect-kind registry. Needed only for the effect-param
     * evaluation pass (EffectAnimatedParameters → EffectParameters
     * write-back). The Transform/MediaLayer branches don't use it.
     * Optional — leaving null disables effect-param eval.
     */
    void setEffectKindRegistry(effects::EffectKindRegistry* reg) {
        m_effectKindRegistry = reg;
    }

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
    effects::EffectKindRegistry* m_effectKindRegistry{nullptr};
    bool m_debugLogging{false};
};

} // namespace entity
