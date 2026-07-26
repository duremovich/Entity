#pragma once

#include "../components/AnimatedProperties.hpp"
#include "../core/Types.hpp"

#include <entt/entt.hpp>
#include <string>

namespace entity {

/**
 * Address of one keyframe, for the plural (multi-select) keyframe commands.
 *
 * Covers both track kinds behind one descriptor so the group operations don't
 * need a duplicate command family per kind:
 *   - transform tracks: trackIndex + clipIndex + property (enum-keyed
 *     AnimatedProperties)
 *   - effect params:    effectEntity + paramName (hash-keyed
 *     EffectAnimatedParameters)
 * `frame` is layer-local, matching every other keyframe command.
 *
 * Lives in its own header so TimelineWidget can build these without pulling in
 * all of Commands.hpp.
 */
struct KeyframeAddr {
    bool isEffect{false};

    // Transform rows
    int trackIndex{0};
    int clipIndex{0};
    AnimatableProperty property{AnimatableProperty::PositionX};

    // Effect-param rows
    entt::entity effectEntity{entt::null};
    std::string  paramName;

    FrameNumber frame{0};
};

}  // namespace entity
