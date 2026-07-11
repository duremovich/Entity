#pragma once

// Pure instance-level outer→inner frame mapping for precomps (ADR-0029
// Decision 3). Stateless floor-of-product — no accumulator, so no drift —
// matching the existing clip math's style. Consumers evaluate a precomp's
// member clips by running the UNCHANGED clip math (isClipActiveAtFrame,
// mapToMediaFrameFromCatalogEx) with timelineFrame = innerFrame and
// timelineFrameRate = definitionFrameRate; double-wrap (instance Loop ×
// member PingPong) is well-defined because both wraps are stateless
// functions of absolute frame.

#include "entity/components/Clip.hpp"   // PlaybackMode
#include "entity/core/Types.hpp"

namespace entity {

struct PrecompInstanceParams {
    FrameNumber instanceStartFrame{0};   // outer timeline frames
    FrameNumber instanceDuration{0};     // outer timeline frames
    FrameNumber innerStartFrame{0};      // trim into the definition (definition frames)
    FrameNumber definitionDuration{0};   // definition frames
    double      definitionFrameRate{30.0};
    double      speed{1.0};              // clamped to [0.01, 100]; negative banned v1
    PlaybackMode playbackMode{PlaybackMode::Freeze};  // instance-level end behavior
};

struct PrecompFrameResult {
    bool        active{false};
    FrameNumber innerFrame{0};
    bool        pingPongReverse{false};
};

// active = outerFrame ∈ [start, start + duration); playLen =
// definitionDuration - innerStartFrame (<= 0 ⇒ inactive, as is
// instanceDuration <= 0); sourceLocal = floor(local · (defFps/outerFps) ·
// speed); wrap via wrapLocalFrame; innerFrame = innerStartFrame + wrapped.
// Precondition (authoring invariant enforced by later phases, not here):
// innerStartFrame ∈ [0, definitionDuration).
PrecompFrameResult mapOuterToInnerFrame(const PrecompInstanceParams& params,
                                        FrameNumber outerFrame,
                                        double outerTimelineFps);

} // namespace entity
