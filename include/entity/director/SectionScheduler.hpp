#pragma once

#include "entity/core/Types.hpp"

#include <entt/entt.hpp>

namespace entity {

class Timeline;

// SectionScheduler — Director-side state machine that watches Timeline
// playback for section-break crossings and parks the playhead at the
// first break it crosses each tick.
//
// Phase B parked the playhead and let `mapToMediaFrame` freeze every
// clip's source frame as a side effect of the timeline frame freezing.
// Phase C activates the per-clip `sectionBehavior` flag: when a break
// fires, every clip flagged Normal gets a `ClipPlaybackPhase` component
// seeded with the source-frame phase it had at the break, and the
// scheduler advances that phase each at-break tick. `mapToMediaFrame`
// consults the phase so Loop/PingPong clips keep cycling while the
// playhead is parked. Locked clips simply skip continuation and freeze
// (their `inContinuation` is never set true).
//
// Owned by Director (next to PlaybackTimeAuthority). Engine ticks it
// each main-loop update AFTER `Timeline::update(dt)` (so the snapped
// frame is the one AnimationSystem evaluates against).
class SectionScheduler {
public:
    SectionScheduler(entt::registry& registry, Timeline* timeline);
    ~SectionScheduler() = default;

    SectionScheduler(const SectionScheduler&) = delete;
    SectionScheduler& operator=(const SectionScheduler&) = delete;

    /** Per-tick hook. If the playhead just crossed any Section::breakFrame
     *  this tick, snap to it, pause the timeline, raise the at-break flag
     *  on Timeline, and seed `ClipPlaybackPhase` on every Normal-mode
     *  active clip. While at-break, advances accumulated source-frame
     *  phase by `deltaTimeSeconds * clip.framerate` for each Normal clip
     *  in continuation. Scrub jumps are not crossings — only continuous
     *  playback advances trigger break detection. */
    void tick(double deltaTimeSeconds);

    /** Spacebar GO when at a break: clear the at-break flag, advance one
     *  frame past the break, resume play. Also clears `inContinuation`
     *  on every clip so the next mapToMediaFrame call falls back to the
     *  timeline-derived path. No-op if not at a break. */
    void go();

    bool atBreak() const { return m_atBreak; }
    Timecode lastBreakHitFrame() const { return m_lastBreakHitFrame; }

private:
    // Runs once when a break-crossing is detected. Walks Clip + Section
    // behavior, allocates ClipPlaybackPhase on Normal clips, seeds
    // `sourcePhaseFrames` from the break-time delta.
    void seedContinuationAt(Timecode breakFrameTime);

    // Runs every at-break tick. Adds `dt * clip.framerate` (in source
    // frames) to each clip currently in continuation.
    void advanceContinuation(double deltaTimeSeconds);

    // Runs from go(). Clears `inContinuation` and resets the accumulated
    // phase across every clip carrying the component. The component is
    // left allocated so re-seeding on the next break crossing is alloc-free.
    void clearAllContinuation();

    entt::registry& m_registry;
    Timeline*       m_timeline{nullptr};
    bool            m_atBreak{false};
    Timecode        m_lastBreakHitFrame{0};
    Timecode        m_lastTickFrame{0};
    bool            m_haveLastTickFrame{false};  // First-tick guard: skip crossing detection.
};

} // namespace entity
