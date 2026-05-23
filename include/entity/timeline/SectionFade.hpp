#pragma once

#include "entity/core/Types.hpp"

namespace entity {

class Timeline;

namespace timeline {

// Number of timeline frames the section-fade tail extends past `endFrame`
// when a section break sits exactly on `endFrame`. The tail covers two
// distinct concerns:
//   - At least 1 frame so a trailing-edge clip stays visible while the
//     playhead is parked at the break, independent of fadeSeconds.
//   - `ceil(fadeSeconds * timelineFrameRate)` additional frames when the
//     section has a non-zero fade so the clip ramps to zero opacity
//     after GO.
// Returns 0 when no section break aligns with `endFrame` (or the
// timeline has no sections / no rate). Pure read of Timeline state —
// safe to call from any thread that may already hold the section
// shared-lock.
//
// Used by PlaybackTimeAuthority (held-frame math + active-set
// membership), DecodeSystem (tail-window worker steering) and
// AudioSystem (parallel A/V tail steering) — extracted to a free
// function so all three call sites share one implementation.
FrameNumber sectionFadeTailFrames(const Timeline& timeline,
                                  FrameNumber endFrame);

} // namespace timeline
} // namespace entity
