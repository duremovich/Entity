#pragma once

#include "entity/core/Types.hpp"

#include <entt/entt.hpp>

namespace entity {

class PlaybackTimeAuthority;
class Timeline;

// SectionScheduler — Director-side (editor-thread) state machine for
// section-break parking.
//
// Break-crossing DETECTION runs on the show thread (Engine::showThreadMain,
// NEW-08) so a break is caught on time even when the editor thread stalls;
// the show thread snaps + pauses the playhead and posts a
// bus::SectionBreakDetected. This class APPLIES a detected crossing via
// handleBreakAt() and owns the at-break continuation state machine.
//
// When a break is applied, every clip flagged `sectionBehavior == Normal`
// gets a `ClipPlaybackPhase` component seeded with the source-frame phase
// it had at the break (`seedContinuationAt`). `mapToMediaFrame` consults
// the phase so Loop/PingPong clips keep cycling while the playhead is
// parked; the show-side `mapToMediaFrameFromCatalog` re-derives that phase
// from a wall-clock anchor so cycling survives an editor stall. Locked
// clips skip continuation and freeze (`inContinuation` stays false).
//
// Owned by Director (next to PlaybackTimeAuthority). Engine ticks it each
// editor-thread update; handleBreakAt() is additionally called from
// Engine::drainRendererToDirector when a show-detected crossing arrives.
class SectionScheduler {
public:
    SectionScheduler(entt::registry& registry, Timeline* timeline);
    ~SectionScheduler() = default;

    SectionScheduler(const SectionScheduler&) = delete;
    SectionScheduler& operator=(const SectionScheduler&) = delete;

    /** Per-tick hook on the editor thread. Section-break *crossing
     *  detection* lives on the show thread now (NEW-08) — see
     *  `Engine::showThreadMain` — so this no longer detects crossings.
     *  It still: clears continuation on Stop, advances continuation phase
     *  while parked at a break (Paused), drops a stale at-break latch on
     *  manual resume, and resets post-break anchors when the user scrubs
     *  during playback. */
    void tick(double deltaTimeSeconds);

    /** Apply a section-break crossing detected by the show thread (NEW-08).
     *  Called only from `Engine::drainRendererToDirector` when a
     *  `bus::SectionBreakDetected` message arrives. The show thread has
     *  already snapped + paused the playhead and raised
     *  `Timeline::sectionAtBreak()`; this runs the registry-mutating
     *  catch-up on the editor thread: raise the scheduler's at-break
     *  latch and seed `ClipPlaybackPhase` on every Normal-mode active
     *  clip. Idempotent; drops the call if the message is stale (a seek
     *  or Play landed before the editor drained it). */
    void handleBreakAt(Timecode breakFrameTime);

    /** Spacebar GO when at a break: clear the at-break flag, advance one
     *  frame past the break, resume play. Also clears `inContinuation`
     *  on every clip so the next mapToMediaFrame call falls back to the
     *  timeline-derived path. No-op if not at a break. */
    void go();

    bool atBreak() const { return m_atBreak; }
    Timecode lastBreakHitFrame() const { return m_lastBreakHitFrame; }

    /** Wire the active PlaybackTimeAuthority so the continuation-phase
     *  wall-clock anchor uses the same RateSource as the main timeline
     *  (audio crystal when audio is on). Called once from Director constructor
     *  after both objects are constructed. Phase G. */
    void setTimeAuthority(PlaybackTimeAuthority* a) { m_timeAuthority = a; }

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

    // Phase 6 — capture the source-media frame each Normal-mode continuing
    // clip is currently displaying, so the post-break held-frame fade
    // shows what the user actually saw at the moment of GO. Must be
    // called BEFORE `clearAllContinuation` flips `inContinuation` to
    // false — otherwise the math falls back to the timeline-frozen path
    // and we snapshot the wrong frame. No-op for clips that aren't
    // ending at this break (their tail hold is irrelevant).
    void snapshotTailHoldFrames(Timecode breakFrameTime);

    // Round-2 fixup, Phase 4 — capture a post-break source-frame anchor
    // for every Normal-mode continuing clip that SPANS this break (clip
    // does NOT end at it; ends-at-break is owned by tail-hold). Called
    // from go() between `snapshotTailHoldFrames` and `clearAllContinuation`
    // so the same observed source frame fed into the tail snapshot is
    // also frozen as the anchor for clips that continue past the break.
    // The 3-arg `mapToMediaFrame` later derives subsequent media frames
    // as `anchor + (timelineFrame - anchorTimelineFrame) * frameRateRatio`
    // wrapped per playbackMode, so post-GO playback resumes without the
    // rewind that natural-mapping (clipStart-derived) would produce.
    void snapshotPostBreakAnchors(Timecode breakFrameTime);

    // Round-2 fixup, Phase 4 — invalidate post-break anchors when the
    // user scrubs backward past the timeline frame that set them, or
    // forward-scrubs past a break without experiencing the at-break pause.
    // Called from the discontinuity branch in `tick()` (and from the
    // Stopped path) using the current playhead frame; any anchor whose
    // `anchorTimelineFrame > currentTimelineFrame` is reset to sentinels.
    void resetAnchorsAcrossScrub(Timecode currentTime);

    entt::registry&        m_registry;
    Timeline*              m_timeline{nullptr};
    PlaybackTimeAuthority* m_timeAuthority{nullptr};
    bool                   m_atBreak{false};
    Timecode        m_lastBreakHitFrame{0};
    Timecode        m_lastTickFrame{0};
    bool            m_haveLastTickFrame{false};  // First-tick guard: skip crossing detection.

    // Round-2 fixup, Phase 4 — unit tests for the post-break anchor
    // model exercise the private snapshot / re-seed / scrub-reset paths
    // directly. The state-machine wrapper (tick / go) is gated on
    // Timeline play-state transitions that are awkward to drive
    // synthetically; calling the helpers directly keeps the assertions
    // tight to the behavior under test.
    friend class PostBreakAnchorTest;
};

} // namespace entity
