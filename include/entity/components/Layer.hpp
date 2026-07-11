#pragma once

#include "entity/core/Types.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace entity {

/**
 * Layer component — generic timeline-resident entity.
 *
 * Every entity placed on a TimelineTrack carries a Layer alongside its
 * kind-specific data components. Layer captures the timeline-placement
 * contract: start frame, duration, owning track, display label/color,
 * and a Kind enum for UI badge / serialization disambiguation.
 *
 * Kinds shipped with the abstraction:
 *   - Clip            — video / image source backed by the Clip component.
 *   - ObjectAnimation — keyframed transform on a Screen or Prop target.
 *   - Generative      — procedural texture source (Muncher, Text — ADR-0017).
 *   - Signal          — timeline-driven signal output (OSC — ADR-0027).
 *
 * Per ADR-0016, system selection is by component composition (
 * view<Layer, Clip>, view<Layer, ObjectAnimationLayer>, ...). The Kind
 * field is informational only — never gate behavior on it.
 *
 * Phase 3 migration window: for Clip-backed entities, Clip::startFrame /
 * Clip::duration remain the source of truth. The Layer mirror is kept in
 * sync via syncLayerFromClip() so timeline-iteration code can read
 * Layer::startFrame uniformly. A Phase 4 cleanup PR will promote the
 * placement fields onto Layer outright.
 */
struct Layer {
    enum class Kind : std::uint8_t {
        Clip            = 0,
        ObjectAnimation = 1,
        Generative      = 2,  // procedural texture source (Muncher mini-game, future particles, ...)
        Signal          = 3,  // signal output layer (OSC / plugin event dispatch)
    };

    FrameNumber startFrame{0};
    FrameNumber duration{0};
    std::uint32_t trackIndex{0};
    std::string  name;
    std::array<float, 4> color{0.4f, 0.6f, 0.9f, 1.0f};
    Kind kind{Kind::Clip};
};

// Display label for a Layer::Kind. Used by the Layers panel, timeline
// row headers, and serialization. Keep in sync with serializeLayerKind /
// parseLayerKind once those land (ProjectSerializer v15, commit 3.6).
inline const char* layerKindName(Layer::Kind k) {
    switch (k) {
        case Layer::Kind::Clip:            return "Clip";
        case Layer::Kind::ObjectAnimation: return "Object Animation";
        case Layer::Kind::Generative:      return "Generative";
        case Layer::Kind::Signal:          return "Signal";
    }
    return "Unknown";
}

} // namespace entity
