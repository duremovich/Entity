#pragma once

#include <array>

namespace entity {

/**
 * ObjectAnimationOutput component — editor-side evaluated output for an OA layer.
 *
 * Written by AnimationSystem each editor tick on the OA layer entity.
 * Read by buildSceneSnapshot (Phase 3.4) to fold the override values into the
 * target Screen/Prop's ScreenSnapshot entry.
 *
 * The three `has*` flags indicate which axes have active keyframe tracks.
 * A false flag means the animated value should not override the target's
 * static position/rotation/scale for that axis group.
 *
 * ADR-0014: written only from the editor thread. The show thread reads
 * the baked ScreenSnapshot, never this component.
 */
struct ObjectAnimationOutput {
    std::array<float, 3> positionOverride{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotationOverride{0.0f, 0.0f, 0.0f};
    std::array<float, 3> sizeOverride{1.0f, 1.0f, 1.0f};

    bool hasPosition{false};
    bool hasRotation{false};
    bool hasSize{false};
};

} // namespace entity
