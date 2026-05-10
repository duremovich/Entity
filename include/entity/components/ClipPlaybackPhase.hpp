#pragma once

#include "../core/Types.hpp"

namespace entity {

// Per-clip continuation-phase state. Runtime-only (NOT persisted by
// ProjectSerializer). Allocated lazily by SectionScheduler when the
// playhead pauses at a section break and the clip is sectionBehavior=Normal.
//
// While `inContinuation` is true, the Director advances `sourcePhaseFrames`
// each tick (in SOURCE-clip frames, not timeline frames) and
// PlaybackTimeAuthority::mapToMediaFrame consults this phase instead of
// computing the offset from the (frozen) timeline frame. Cleared by
// SectionScheduler::go() so the next mapToMediaFrame call falls back to
// the timeline-derived path.
//
// `tailHoldMediaFrame` records the source-media frame to display during the
// post-break hold + fade-out tail (Phase 6). Snapshotted by
// SectionScheduler::go() at the moment continuation is cleared so the
// held image matches whatever the clip was showing when GO fired (matters
// for Loop / PingPong clips that continued cycling at the break).
// Sentinel -1 means "no snapshot; fall back to last-decoded frame".
//
// Round-2 fixup, Phase 4 — `postBreakMediaAnchor` / `anchorTimelineFrame`
// implement the post-break anchor model. At GO for spanning Normal
// continuation clips (clip ends past the break, not at it), capture the
// source-media frame the user was watching at that exact moment. Subsequent
// timeline-frame ticks compute mediaFrame relative to this anchor instead
// of clipStart, so playback continues without the rewind that the natural
// (clipStart-derived) mapping would otherwise produce. Sentinel -1 means
// no anchor (use natural mapping). The pair is preserved across
// clearAllContinuation (anchors outlive a single break crossing) and is
// reset on backward scrub past `anchorTimelineFrame`.
//
// Phase C (sections-and-cues epic): pure data. POD per components/CLAUDE.md.
struct ClipPlaybackPhase {
    double      sourcePhaseFrames{0.0};  // Source-frame phase, fractional. See note below.
    FrameNumber tailHoldMediaFrame{-1};  // Source-media frame to hold during tail; -1 = unset.
    bool        inContinuation{false};

    // Post-break anchor (round-2 fixup, Phase 4): at GO for spanning
    // Normal-continuation clips, capture the source frame the user was
    // watching at that exact moment. Subsequent timeline-frame ticks compute
    // mediaFrame relative to this anchor instead of clipStart, so playback
    // continues without a rewind. Sentinel -1 = no anchor (use natural mapping).
    // Reset on backward scrub past `anchorTimelineFrame`.
    FrameNumber postBreakMediaAnchor{-1};
    FrameNumber anchorTimelineFrame{0};

    // Wall-clock anchor for continuation (round-3 fixup): the at-break park
    // recomputes sourcePhaseFrames each tick as
    //   continuationSeedFrames + (now - continuationStartTimeNs) * fps
    // instead of accumulating dt-per-tick. Immune to editor tick-rate
    // fluctuations and dt mis-measurement — phase tracks wall time exactly.
    // continuationStartTimeNs == 0 means "no wall-clock anchor"; any code path
    // (legacy / synthetic-test / future) that wants the historical
    // dt-accumulator can leave it at 0. seedContinuationAt sets both fields
    // when raising inContinuation; clearAllContinuation resets them.
    int64_t     continuationStartTimeNs{0};   // steady_clock ns at continuation entry.
    double      continuationSeedFrames{0.0};  // sourcePhaseFrames at continuation entry.
};

} // namespace entity
