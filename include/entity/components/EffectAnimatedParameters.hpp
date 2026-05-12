#pragma once

#include "AnimatedProperties.hpp"

#include <cstdint>
#include <vector>

namespace entity {

// Per-effect animated parameter tracks. Parallel to AnimatedProperties
// but keyed by parameter NAME hash (FNV-1a of the ParamSchema.name)
// instead of the closed AnimatableProperty enum.
//
// Why a separate component instead of generalising AnimatedProperties:
// the existing enum has only 10 values and the AnimationSystem switch
// is hot. Effects have unbounded parameter sets (Blur radius/direction/
// iterations; ColorCorrect shadows/mids/highlights × rgb; …). Forcing
// effect parameter identity into the enum would either bloat the
// switch or destroy the property-name → effect-parameter mapping.
// Keeping them in a sibling component preserves the fast-path
// Transform animation while giving effects an extensible store.
//
// The keyframe data type and interpolation math are shared with
// AnimatedProperties — reuse entity::Keyframe + entity::InterpolationType
// + KeyframeTrack::interpolate via a small wrapper.
struct EffectAnimatedParameters {
    struct NamedTrack {
        // FNV-1a hash of ParamSchema.name (e.g. FNV-1a("radius") for
        // a Blur effect). Wire serialization uses the string name and
        // the show side rehashes once on receipt.
        std::uint32_t          paramKeyHash{0};
        bool                   enabled{true};
        std::vector<Keyframe>  keyframes;

        bool hasKeyframes() const { return !keyframes.empty(); }
    };

    std::vector<NamedTrack> tracks;

    bool hasAnyKeyframes() const {
        for (const auto& t : tracks) {
            if (t.hasKeyframes()) return true;
        }
        return false;
    }
};

} // namespace entity
