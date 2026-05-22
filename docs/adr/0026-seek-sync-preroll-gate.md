# ADR-0026: Seek-sync preroll gate

- **Status:** Accepted
- **Date:** 2026-05-22
- **Implemented by:** Audio playback seek-sync plan (`~/.claude/plans/tidy-nibbling-kurzweil.md`),
  Part 2 Phases A-D. Commits: seek-sync implementation on `master`.
- **Extends:** ADR-0025 (rate vs. position authority — a position-axis concern).
  The gate is a sub-state of the Playing position state.
- **Relates to:** ADR-0014 (editor/show thread split — Engine placement rationale),
  ADR-0003 (Director/Renderer split — boundary constraint).

## Context

When playback starts after a seek, decoders need genuine catch-up time before
they can present the target frame:

- **Video** must decode forward from the previous GOP keyframe to the target —
  hundreds of milliseconds to seconds on 4K H.264.
- **Audio** must re-seek and refill its decoded-PCM ring buffer (~tens of
  milliseconds).

`Timeline::play()` otherwise starts the transport advancing immediately, so the
playhead (and audio) move while the video decoder is still catching up — the
first moments of playback show stale frames and start visibly misaligned.

This ADR records the **seek-sync preroll gate**: hold the playhead parked at the
target frame, audio silent, until every active decoder genuinely reaches that
frame, then release — playback begins cleanly, both streams from the same frame.

> **Scope note — what this gate does *not* do.** The seek-sync plan was
> originally motivated by a *multi-second* mid-clip-seek desync (audio seconds
> ahead of video). Investigation during implementation found that this was a
> separate **decoder correctness bug**: `ProResDecoder` assumed an intra-only
> codec and, on an inter-frame (H.264) seek, returned the GOP keyframe's pixels
> mislabeled as the target frame — so video sat a whole GOP behind while audio
> played correctly. That is fixed independently (commit `12590ae`). The preroll
> gate does **not** fix or mask that bug — it relies on decoders reporting their
> position honestly. The gate's value, recorded here, is the narrower one above:
> a clean, synchronized *start* to playback after a seek instead of a glitchy
> interval while decoders do their legitimate catch-up.

Play-from-frame-0 needs no real preroll — frame 0 is a keyframe and decoders are
already warm — so the gate releases within one editor tick there.

## Decision

When playback starts, hold the timeline playhead **parked at the target frame**
and keep audio output **silent** until every active decoder (video + audio) reports
its target frame ready; then release — both resume from the same frame, in sync.

### Gate mechanics

A new `std::atomic<bool> m_seekSyncGate` on `Timeline` gates the advance loop
in `Timeline::update()`:

```cpp
if (m_playbackState.load() == PlaybackState::Playing
        && !m_seekSyncGate.load(std::memory_order_acquire)) {
    // advance m_currentTime
}
```

`Timeline::play()` sets the gate on the `->Playing` transition.
`pause()`, `stop()`, `seek()`, and `clear()` always clear it so a stale gate
can never deadlock the transport.

### Sibling atomic, not enum sub-state

The gate was implemented as a sibling `std::atomic<bool>` alongside the existing
`m_playbackState` atomic (which carries `Stopped / Playing / Paused`). The
alternative — extending the enum with a `PreRolling` state — was considered and
rejected:

- **Existing code** checks `== Playing` in many places; introducing `PreRolling`
  would require auditing every one of those sites.
- **The gate is transient** — it engages for tens to hundreds of milliseconds and
  clears itself. A PlaybackState is a stable, user-visible mode; pre-roll is an
  internal synchronization phase.
- **Audio silence** is already handled by `mixSource.active` in AudioSystem
  (`active = shouldSteer && !isSeekSyncGated()`), so no new state fan-out is needed.
- **Thread safety** is identical: both `m_playbackState` and `m_seekSyncGate` are
  `std::atomic`; acquire/release ordering is explicit.

### Readiness predicates

`SeekSyncController` (a plain class, not an ECS System) owns the release logic:

```
videoReady: ∀ active Clips at the parked frame:
    DecodeSystem::isClipReadyAt(entity, mediaFrame)
        → initialized && !seekPending && FrameCache::has(entity, mediaFrame)

audioReady: ∀ active Clips+AudioSource at the parked frame:
    AudioSystem::isWorkerSeekReady(entity)
        → initialized && !seekPending && ring.availableFrames() >= 2048
```

Clips that `initFailed` (broken media, no audio stream, image sequences) count
as ready so they never block the gate.

### Engine placement (ADR-0014 constraint)

Per ADR-0014 / ADR-0003, Director-side code must not name Renderer-side code.
`Timeline`, `AudioSystem`, and `PlaybackTimeAuthority` are Director-side;
`DecodeSystem` and `FrameCache` are Renderer-side. A controller that polls both
must therefore live on **`Engine`**, which already holds shortcuts to all of them.
The controller is constructed in `Engine::initialize()` and ticked from
`Engine::update()` right after `AudioSystem::update`.

### Testable seam

The readiness checks are injectable `std::function<bool()>` predicates rather
than hardcoded system pointers. This lets unit tests supply fakes:
`[]{ return false; }` for a stalled decoder, `[]{ return true; }` for an
immediately-ready one — without standing up a full Engine or registry.

### Timeout failsafe

If the gate is held longer than `kPrerollTimeoutMs = 3000` ms, `SeekSyncController`
releases it with a warning log. This ensures a decoder that genuinely cannot reach
the target frame (missing media, irrecoverable seek error) never hangs playback
indefinitely. The degraded outcome is the same as before this ADR: A/V may start
slightly out of sync.

## Consequences

### Positive

- **Clean playback start on every seek.** Video and audio begin from the same
  frame on every Play command — no glitchy interval while decoders catch up.
- **Play-from-frame-0 is unaffected.** Decoders are already warm at keyframe 0;
  the gate releases within one editor tick — imperceptible.
- **No double-logic.** `Timeline::play()` is the single engagement site. All play
  entry points (`PlayCommand`, `TogglePlayPauseCommand`, `SectionScheduler::go`)
  call `timeline->play()`.
- **Degraded but not broken on timeout.** A 3-second ceiling prevents indefinite
  holds while still accommodating slow GOP seeks on compressed 4K media.

### Negative / known limitations

- **Editor-stall extends the hold.** `SeekSyncController` ticks editor-thread-only
  (`Engine::update`). A >50 ms editor stall (Win32 modal, slow project load) freezes
  both the readiness poll and the timeout clock. The playhead stays parked — not a
  hang, but the hold is extended by the stall duration. The timeout still fires once
  the editor resumes. A show-thread timeout force-clear (polling an atomic engage
  timestamp) is a deferred follow-up; acceptable for v1.
- **One additional per-tick view walk.** When the gate is active, `tick()` walks
  `view<Clip>()` and `view<Clip, AudioSource>()` once per editor tick. With the gate
  clear (99.9% of ticks), `tick()` returns immediately on the first branch — no cost.
- **No show-thread preroll meter.** The preroll state is not visible to the show
  thread; there is no "loading..." overlay driven from this gate. A future UI
  affordance would need a separate atomic readable from the show thread.

## Alternatives considered

### Extend `PlaybackState` with a `PreRolling` variant (rejected)

Would make the parked-but-Playing sub-state first-class in the type system.
Rejected because: dozens of `== Playing` checks would need auditing; the gate is
too transient to warrant a user-visible enum value; the sibling atomic achieves
the same isolation without the audit cost.

### Hold the gate in `SectionScheduler::go()` instead of `Timeline::play()` (rejected)

Would restrict the gate to cue-driven resumes and miss direct Play commands on
mid-clip seeks. `Timeline::play()` is the common engagement path for all play
variants.

### Let video wait for audio (audio drives; video catches up) (rejected)

Inverting the direction: pause audio until video reaches the target frame. Rejected
because video decode latency is an order of magnitude larger and less predictable;
"wait for the slow thing" is the correct model.

### No gate; rely on AudioSystem `seekPending` to delay audio output (rejected)

AudioSystem already silences via `mixSource.active = false` while `seekPending`.
That keeps *audio* from emitting stale samples, but it doesn't stop the *timeline*
from advancing while the video decoder is still catching up — so playback still
starts on a frame the video pipeline hasn't reached. The gate must hold the shared
clock, not just mute one output.

## References

- Plan: `~/.claude/plans/tidy-nibbling-kurzweil.md` (Part 2, Phases A-D)
- [ADR-0014: Editor/show thread split](0014-editor-show-thread-split.md)
- [ADR-0025: Timeline clock — rate vs. position authority](0025-clock-rate-vs-position-authority.md)
- [ADR-0003: Director/Renderer split](0003-director-renderer-split.md)
- Implementation: `include/entity/core/SeekSyncController.hpp`,
  `src/core/SeekSyncController.cpp`, `include/entity/timeline/Timeline.hpp`,
  `src/timeline/Timeline.cpp`, `src/core/Engine.cpp`
- Tests: `tests/unit/SeekSyncControllerTest.cpp`,
  `scripts/integration/seek_sync_mid_clip.json`,
  `scripts/integration/seek_sync_frame_zero.json`
