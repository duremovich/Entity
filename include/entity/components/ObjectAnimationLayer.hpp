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
    // What happens to the target Screen/Prop when the playhead is past the
    // end of the OA layer's active window. See ADR-0020 / docs/adr/0020-*.
    //   Hold  — keep last-evaluated ObjectAnimationOutput populated; the
    //           target stays parked where the animation left it until
    //           another OA layer or operator action moves it. Show-friendly
    //           default.
    //   Reset — clear ObjectAnimationOutput; buildSceneSnapshot falls
    //           through to the Screen/Prop's Stage-configured base
    //           position. Today's pre-ADR-0020 behavior, kept as an opt-in.
    enum class EndBehavior : std::uint8_t {
        Hold  = 0,
        Reset = 1,
    };

    entt::entity    target{entt::null};         // Screen or Prop entity being driven
    SectionBehavior sectionBehavior{SectionBehavior::Normal};
    EndBehavior     endBehavior{EndBehavior::Hold};
    // Set by SectionScheduler::seedContinuationAt when a Locked OA layer is
    // active at a section break. AnimationSystem skips re-evaluation while true,
    // preserving the last-evaluated ObjectAnimationOutput. Cleared by
    // SectionScheduler::clearAllContinuation (on GO or Stop).
    bool            frozen{false};
};

} // namespace entity
