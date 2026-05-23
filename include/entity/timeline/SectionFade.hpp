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

// fadeSeconds of the section break that aligns exactly with `endFrame`.
// Returns 0.0 if no break aligns (or the break has zero fade configured).
// Pure read of Timeline state -- same threading contract as
// sectionFadeTailFrames.
//
// Used by PlaybackTimeAuthority to gate the Normal-mode break-aligned
// active-window extension on fadeSeconds == 0 (see ADR-0012 amendment
// 2026-05-23 follow-up). When the operator sets fadeSeconds > 0 they
// have signalled "this break is a fade transition" -- the clip should
// fade out per the existing tail-frames mechanism, not extend past the
// break. With fadeSeconds == 0 the extension is the right behavior
// (Fix 8's original use case: trimmed-to-break clip with hard cut,
// playback continues past the trim to source-out).
double sectionFadeSecondsAtBreak(const Timeline& timeline,
                                 FrameNumber endFrame);

} // namespace timeline
} // namespace entity
