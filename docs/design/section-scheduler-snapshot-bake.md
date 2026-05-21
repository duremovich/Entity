# SectionScheduler editor-stall fix — design

**Tracking issue**: `docs/reference/CODE_ISSUES.md` NEW-08.
**Status**: **Implemented 2026-05-20.** Closed NEW-08.
**Sibling design**: `docs/design/animation-snapshot-bake.md` (NEW-07) —
similar shape, less state. Read that first if you haven't.

> **Implemented with two divergences from the design below**, decided
> after a threading review:
>
> 1. **Show-only detection.** The design suggested keeping the editor
>    `tick()` detector as a "non-stalled fast path" alongside the new
>    show detector. That dual-detector arrangement is a race surface
>    (double-seed resetting the wall-clock anchors; `m_lastTickFrame`
>    desync). Instead, crossing detection was **removed from `tick()`
>    entirely** in the same change that added the show detector — one
>    detector, one applier, no dedup races.
> 2. **Sections read live, not baked.** The detector reads the section
>    list via `Timeline::copySectionsAndRate()` (already show-safe —
>    shared lock) instead of a new `SceneSnapshot::sections` field. A
>    detector wants live data, and this adds zero bus surface. The
>    snapshot `atBreak` / `lastBreakHitFrame` gating fields were also
>    not needed — `Timeline::sectionAtBreak()` (an existing atomic) is
>    the gate, and `handleBreakAt` uses it (plus `state == Paused`) as
>    its staleness guard.
>
> The wall-clock continuation anchor went onto `bus::ClipCatalogEntry`
> as `phase_continuationStartTimeNs` / `phase_continuationSeedFrames`
> (mirroring the existing `ClipPlaybackPhase` fields) rather than a
> single `phase_anchorWallTimeNs`. `advanceContinuation` was kept as an
> editor-side per-tick write (belt-and-suspenders for the PropertyWindow)
> and is additionally called once at the top of `go()` so a GO after a
> stall snapshots a fresh phase. The rest of the design below stands.

This doc is more nuanced than the AnimationSystem one because
SectionScheduler is a state machine that carries memory across ticks
(`m_lastTickFrame`, `m_atBreak`), not just a stateless evaluator.

---

## Problem

When the editor thread stalls, `SectionScheduler::tick()` stops running.
Two things break on the projector output:

1. **Section-break crossing detection misses.** If the playhead crosses a
   `Section::breakFrame` *during* the stall (Timeline keeps advancing
   via show-thread fallback), the at-break logic doesn't fire. When the
   stall ends, the editor catches up and fires the break-handling code
   — but by then the playhead is way past the break. Cue fires late;
   user-visible glitch.

2. **Continuation-phase advancement stops.** Once a break has fired and
   the playhead is parked, Normal-mode clips with `ClipPlaybackPhase`
   are supposed to keep their source frame advancing
   (`sourcePhaseFrames += dt * clip.framerate`). If `tick()` doesn't
   run, the source frame stops advancing — Loop / PingPong clips
   freeze on a single source frame instead of cycling during the
   at-break hold.

Both have the same root cause: `tick()` only ticks on the editor thread,
and it writes registry components (`ClipPlaybackPhase`) plus Timeline
state. Same ADR-0014 constraint as NEW-07.

## Why the naive fix doesn't work

Identical to NEW-07: ticking `SectionScheduler::tick()` from
`Engine::showThreadMain()` would violate ADR-0014's editor-only-writes
rule. `seedContinuationAt()` allocates `ClipPlaybackPhase` components.
`advanceContinuation()` mutates them. Both write the registry.

There's also a second problem specific to SectionScheduler that's worse
than AnimationSystem: the crossing-detection logic depends on
`m_lastTickFrame`, a previous-tick snapshot. If two threads run `tick()`
they fight over that state. So even if the writes were safe, the state
machine isn't.

## The shape of the fix

Two distinct things need fallbacks. Address them separately.

### Part 1 — Continuation-phase advancement

This is the simpler half. Once a break has fired and the editor has
seeded `ClipPlaybackPhase` on each Normal-mode clip, the per-tick work
in `advanceContinuation()` is just:

```
phase.sourcePhaseFrames += dt * clip.framerate;
```

That's a wall-clock-anchored accumulator. Per the
`feedback_walltime_anchor_over_dt_accumulator.md` memory, we should not
accumulate `dt` across decoupled threads — derive from a wall-clock
anchor instead.

**Recommendation**: at break time, the editor thread bakes
`anchorWallTimeNs = steady_clock::now()` into `ClipCatalogEntry` (new
field, alongside the existing `phase_sourcePhaseFrames` and
`phase_anchorTimelineFrame`). The show thread (in `buildRenderFrame`
or `mapToMediaFrame`) reads the anchor and computes:

```
effectiveSourcePhaseFrames = anchorSourcePhaseFrames
    + (steady_clock::now() - anchorWallTimeNs) * clip.framerate;
```

That's evaluated per render frame, doesn't accumulate, can't drift,
and reads only snapshot state — show-safe.

Editor-side `advanceContinuation()` becomes a no-op (or stays as a
belt-and-suspenders write into the registry component for editor UI
inspection — same pattern as AnimationSystem).

### Part 2 — Section-break crossing detection during a stall

This is the harder half. The state machine's responsibilities:

- Compare current playhead to last-tick playhead.
- If the interval crossed any `Section::breakFrame`, snap the playhead
  to that break.
- Pause Timeline play state.
- Mark `atBreak = true`.
- Seed continuation phase on each Normal-mode clip active at that
  frame.

Of these, only the *first three* are time-sensitive (they're what
makes a cue fire on time). The continuation seed can land a moment
late — the playhead is parked at the break and the user hasn't pressed
GO, so a few ms of registry-bookkeeping delay is invisible.

**Recommendation**: make the show thread the canonical crossing
**detector**, and keep the editor as the canonical crossing
**applier**.

**Show thread (`Engine::showThreadMain` or `buildRenderFrame`)**:
- Holds its own show-thread-local `SectionDetectionState` (last-seen
  frame, currently-at-break flag). This is a private show-thread
  variable, NOT a registry component — same pattern as
  `CompositorSystem::m_pendingAllocations`.
- Each render tick: read current `Timeline::getCurrentFrame()`. Check
  the snapshot's section list (`SceneSnapshot::sections`, new field)
  for any break frame in `(lastSeen, currentFrame]`.
- On detection: clamp the show-thread-local `displayedFrame` to the
  break frame (so the projector visibly snaps to it on the next
  Present), set the local `atBreak` flag, and post an R2D message
  `SectionBreakDetected{breakFrame}` so the editor catches up.
- Don't write the registry. Don't change Timeline play state from
  the show thread — Timeline already has show-side `m_currentTime`
  but `m_playbackState` is a separate atomic that the show thread
  is allowed to read; editor owns the writes.

**Editor thread**:
- Drains R2D `SectionBreakDetected` messages in
  `drainRendererToDirector`. For each, calls the existing
  `SectionScheduler::handleBreakAt(frame)` (a small refactor: extract
  the snap+seed work from `tick()` into a function callable from a
  message handler).
- Pauses Timeline play state via `Timeline::pause()` (editor-thread
  write, safe).
- Seeds `ClipPlaybackPhase` and snapshots tail-hold frames, all on
  editor thread.
- Marks `m_atBreak = true` so subsequent editor ticks know they're
  parked.

**During a stall**:
- Show thread detects the crossing, snaps its rendering to the break,
  parks. Output stays visually correct.
- R2D message queues up but isn't drained (editor stalled).
- When the editor stall ends, editor drains the R2D message and runs
  the catch-up work. Timeline pause-state and continuation-phase
  components become consistent with what the show thread has been
  rendering.
- User sees: cue fires at the right wall-clock moment. No late fire.

### Edge cases to think through when implementing

- **Multiple breaks in the stall interval.** If the playhead crossed
  two breaks during the stall, only the first should fire (the
  scheduler is supposed to park at the first one). Show-side
  detector must clamp to the first break and not advance past it,
  same as the editor logic today.

- **Show thread detects but editor already at-break**. Race: editor
  tick ran just before the stall, ended at-break. Show thread is
  unaware and might detect *the same break* again. Use the snapshot's
  `atBreak` flag baked from the editor thread to gate show-side
  detection: don't detect if the snapshot already says we're at-break.

- **GO command during a stall**. User presses spacebar while editor
  is stalled. GO already goes through `CommandDispatcher::enqueue`,
  which is processed on the editor thread when it resumes. The
  `Affinity::Show` carve-out for play/pause might apply here —
  inspect `Commands.hpp::SectionGoCommand`'s affinity. If it's
  Editor-affinity today, leave it; the GO just queues until stall
  ends. (User can't see it process anyway — UI is also frozen.)

- **Scrub past a break during a stall**. Scrubbing while stalled is
  ~impossible (UI frozen), but be defensive: the
  `resetAnchorsAcrossScrub` logic in `tick()` must not be skipped on
  catch-up. Editor-side handler must replay the scrub-reset logic
  using the snapshot's per-frame breadcrumb if we keep one, or run
  it conservatively on every R2D-driven catch-up.

## What gets added to the bus

In `include/entity/bus/Message.hpp`, `SceneSnapshot`:

```cpp
struct SectionSnapshot {
    Timecode breakFrame{0};
    std::string name;
    // Add color/fadeSeconds if downstream needs them — keep minimal at first.
};
std::vector<SectionSnapshot> sections;

// At-break authority — written by editor thread, read by show thread
// to gate its detector.
bool      atBreak{false};
Timecode  lastBreakHitFrame{0};
```

In `ClipCatalogEntry`, extend the existing phase fields:

```cpp
// Wall-clock anchor for continuation phase. Set by editor at break
// time. Show thread derives current sourcePhaseFrames from this anchor
// + elapsed wall clock + clip.framerate.
std::int64_t phase_anchorWallTimeNs{0};
```

R2D message:

```cpp
struct SectionBreakDetected {
    Timecode breakFrame{0};
};
```

Per the bus boundary rules in `include/entity/bus/CLAUDE.md`, these
additions are additive — old serialized payloads still deserialize
with defaults.

## Files to touch when implemented

- `include/entity/bus/Message.hpp` — `SectionSnapshot`, `atBreak`/
  `lastBreakHitFrame` on `SceneSnapshot`, `phase_anchorWallTimeNs` on
  `ClipCatalogEntry`, new `SectionBreakDetected` R2D message.
- `src/bus/Serialization.cpp` — encode/decode the new fields.
- `include/entity/director/SectionScheduler.hpp`/`.cpp`:
  - Extract `handleBreakAt(Timecode)` from `tick()` so it can be
    called from a message handler.
  - Remove the per-tick continuation accumulator
    (`advanceContinuation`) — its work is now derived show-side.
- `src/core/Engine.cpp` (`buildSceneSnapshot`) — bake the new section
  list, at-break flag, and per-clip wall-clock anchor.
- `src/core/Engine.cpp` (`showThreadMain` / `buildRenderFrame`) — show-
  side detector: hold `SectionDetectionState` locally, compare frames,
  post R2D on crossing, gate by snapshot's `atBreak`.
- `src/core/Engine.cpp` (`drainRendererToDirector`) — handle
  `SectionBreakDetected`, call `SectionScheduler::handleBreakAt`.
- `include/entity/director/PlaybackTimeAuthority.hpp`/`.cpp` (if
  `mapToMediaFrame` is the right derivation site for the wall-clock
  phase computation — current `mapToMediaFrame` already takes the
  `ClipPlaybackPhase` into account, so this is a small extension).
- `include/entity/systems/CLAUDE.md` — update SectionScheduler entry.
- `docs/reference/SYSTEM_ORDERING.md` — update fallback coverage table
  (SectionScheduler ✓).
- `docs/reference/CODE_ISSUES.md` — close NEW-08.

## Why this is more invasive than the AnimationSystem fix

AnimationSystem is stateless: same input frame → same output every
time. Drop it on a different thread, evaluate, done. The snapshot-bake
fix is essentially "give the show thread the inputs."

SectionScheduler is a state machine: previous-frame state determines
this-frame behavior. The "give the show thread the inputs" recipe
doesn't fully work because there are *write* operations
(`seedContinuationAt`, `snapshotTailHoldFrames`) that have to land on
the editor thread for ADR-0014 to hold. So we end up with a
detector-on-show, applier-on-editor split, with an R2D message
crossing between them.

Worth doing, but worth respecting that it's more code, more places to
audit, and (per the user's "careless changes break threading" warning)
deserves its own focused PR + extensive verification rather than being
bundled into a refactor sweep.

## Verification plan

When implemented:

1. **Smoke**. Load a project with at least one section break. Play
   through. Section break fires at the right frame. No visible
   regression.

2. **Continuation-phase smoke**. Set up a clip with `PlaybackMode::Loop`
   that spans a section break. Play. At the break, observe the clip
   keeps cycling source frames (not frozen). Press GO. Clip resumes
   from a sensible source frame (post-break anchor logic from Phase 4
   of the existing scheduler design).

3. **Editor-stall reproducer for crossing detection**. Set up a
   project where a section break is at timeline frame 100. Set
   Timeline to frame 95 and play. Immediately stall the editor (drag
   window title bar) for 3 seconds. Without this fix, when the stall
   ends, the editor catches up — break fires at whatever frame the
   playhead is at when editor resumes (frame ~280 in this test), the
   playhead is parked at a wrong frame. With this fix, show thread
   detects break at frame 100 mid-stall, snaps display to frame 100,
   and when editor catches up the break-handling state arrives without
   visible glitch.

4. **Editor-stall reproducer for continuation phase**. Set up a Loop
   clip across a section break, at the break, stall the editor for
   3 seconds. Without the wall-clock anchor: source frame freezes for
   3 seconds, clip's display freezes. With this fix: derived phase
   keeps advancing wall-clock, clip keeps cycling during the stall.

5. **Integration test** (per `CLAUDE.md` `WaitSeconds` guidance and
   covering NEW-09's missing regression coverage):
   - Construct timeline with a break at frame 100.
   - Set playhead to 95, set play state to Playing.
   - Simulate stale editor heartbeat
     (`m_lastEditorTickNs = now - 200ms`).
   - `WaitSeconds(0.5)` — show-thread fallback advances Timeline past
     frame 100.
   - Assert `SceneSnapshot::atBreak == true` (or equivalent observable)
     after the show-thread detector fires.
   - Resume editor (un-stale heartbeat), `WaitSeconds(0.5)`.
   - Assert `SectionScheduler::m_atBreak == true` and
     `ClipPlaybackPhase::inContinuation == true` on Normal-mode clips.

6. **Tracy**. Show-thread per-frame plot for the new detector zone.
   Verify the detector cost is bounded — search of N sections per
   tick is N=O(few). Budget < 0.1 ms per show frame.

7. **`ctest -j 1`**. Expect 430+/430+ passing (will add new
   integration test).

## Rollback shape

Like NEW-07, split into commits:

1. Bus message additions (additive — default values).
2. Editor-side `SectionScheduler` extraction of `handleBreakAt`. Still
   editor-driven; no behavior change.
3. Editor-side wall-clock anchor at break time. Show side ignores it
   for now; editor behavior unchanged.
4. Show-side detector + R2D + show-side wall-clock derivation. This
   is the behavior-change commit. Revert this single commit to
   restore prior behavior if a regression emerges.

The phased commit sequence is more important here than for NEW-07
because the change touches more state-machine wiring; bisection should
land at a precise revert point.

## What not to do

- Don't run `SectionScheduler::tick()` from the show thread. Same
  ADR-0014 violation as NEW-07's naive fix, plus a worse race on the
  state machine's private state.
- Don't accumulate `dt` across threads for the continuation phase.
  Wall-clock anchor + recompute, per the
  `feedback_walltime_anchor_over_dt_accumulator.md` memory.
- Don't share state via a side-channel map between editor and show.
  Bus messages (specifically R2D `SectionBreakDetected`) are the
  contract.
- Don't bundle this with NEW-07. They're cleaner reviewed separately.

## See also

- `docs/design/animation-snapshot-bake.md` — sibling design.
- `docs/adr/0014-editor-show-thread-split.md` — threading rules.
- `docs/adr/0012-timeline-sections-and-cues.md` — section semantics
  these mechanics drive.
- `docs/reference/CODE_ISSUES.md` NEW-08, NEW-09.
- `include/entity/bus/CLAUDE.md` — bus boundary rules.
