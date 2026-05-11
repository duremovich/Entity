#pragma once

#include "entity/core/Types.hpp"
#include "entity/components/Clip.hpp"  // SectionBehavior

#include <entt/entt.hpp>

namespace entity {

/**
 * ObjectAnimationLayer component — marks a Layer entity as a keyframed
 * transform animation targeting a Screen or Prop entity.
 *
 * The layer entity also carries:
 *   Layer             — timeline placement (startFrame, duration, trackIndex)
 *   AnimatedProperties — keyframe tracks (PositionX/Y/Z, RotationX/Y/Z, ScaleX/Y/Z)
 *
 * AnimationSystem writes the evaluated output into ObjectAnimationOutput on
 * the same layer entity each editor tick. buildSceneSnapshot (Phase 3.4)
 * then reads ObjectAnimationOutput and folds it into the target Screen/Prop's
 * ScreenSnapshot entry.
 *
 * ADR-0014 constraint: this component is written only on the editor thread.
 * The show thread reads the baked snapshot — never this component directly.
 */
struct ObjectAnimationLayer {
    entt::entity    target{entt::null};         // Screen or Prop entity being driven
    SectionBehavior sectionBehavior{SectionBehavior::Normal};
};

} // namespace entity
