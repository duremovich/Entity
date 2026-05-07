#pragma once

#include "entity/core/Types.hpp"

namespace entity {

class Timeline;

// SectionScheduler — Director-side state machine that watches Timeline
// playback for section-break crossings and parks the playhead at the
// first break it crosses each tick.
//
// Phase B treats every clip as Locked at a break (the playhead simply
// stops, and `mapToMediaFrame` freezes the source frame because the
// timeline frame stops growing). Phase C will introduce a per-clip
// continuation phase so Normal-mode clips keep cycling past the break
// while the playhead is parked; this scheduler is the seam where that
// per-clip seeding will live.
//
// Owned by Director (next to PlaybackTimeAuthority). Engine ticks it
// each main-loop update AFTER `Timeline::update(dt)` (so the snapped
// frame is the one AnimationSystem evaluates against).
class SectionScheduler {
public:
    explicit SectionScheduler(Timeline* timeline);
    ~SectionScheduler() = default;

    SectionScheduler(const SectionScheduler&) = delete;
    SectionScheduler& operator=(const SectionScheduler&) = delete;

    /** Per-tick hook. If the playhead just crossed any Section::breakFrame
     *  this tick, snap to it, pause the timeline, and raise the at-break
     *  flag on Timeline. Scrub jumps are not crossings — only continuous
     *  playback advances trigger break detection. */
    void tick();

    /** Spacebar GO when at a break: clear the at-break flag, advance one
     *  frame past the break, resume play. No-op if not at a break. */
    void go();

    bool atBreak() const { return m_atBreak; }
    Timecode lastBreakHitFrame() const { return m_lastBreakHitFrame; }

private:
    Timeline* m_timeline{nullptr};
    bool      m_atBreak{false};
    Timecode  m_lastBreakHitFrame{0};
    Timecode  m_lastTickFrame{0};
    bool      m_haveLastTickFrame{false};  // First-tick guard: skip crossing detection.
};

} // namespace entity
