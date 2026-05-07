#pragma once

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
// Phase C (sections-and-cues epic): pure data. POD per components/CLAUDE.md.
struct ClipPlaybackPhase {
    double sourcePhaseFrames{0.0};  // Accumulated source-frame phase, fractional.
    bool   inContinuation{false};
};

} // namespace entity
