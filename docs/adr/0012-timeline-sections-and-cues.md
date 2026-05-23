# ADR-0012: Timeline structural playback — break-point sections, cue tags, and continuation-phase decoupling

- **Status:** Accepted
- **Date:** 2026-05-06
- **Context source:** Working plan
  `~/.claude/plans/where-are-we-on-sunny-sutherland.md`. Implements
  the four-phase epic "Sections + Cue tags".
- **Implemented by:** Six commits on `master`:
  - `707fcd0` — Phase A: cue tags (data + UI + persistence at v9)
  - `1c890fa` — Phase B: section break-points + Locked playback semantics
  - `483ee21` — Phase C: Normal continuation phase
  - `2409d74` — Phase D: section fade envelopes
  - `ec04926`, `6121995` — review-driven fixups

## Context

Live-show timelines need two structural-playback primitives that the
single-clip / single-track scrubbing model doesn't cover:

1. **Section break points.** Show structure splits the timeline into
   spans the operator advances through deliberately — verse, chorus,
   drop, bridge — with a hard pause at each boundary and a "GO" gesture
   to release. Spacebar GO is the lighting-board convention. Without
   this, a long set is one monolithic playback that drifts off the
   programmed cue points the moment a band stretches a verse by ten
   seconds.
2. **Numbered cue markers.** Operators reference moments by number
   ("trigger cue 1.5") regardless of where they live on the timeline.
   Decimal numbering (1, 1.5, 1.6) lets a programmer insert a cue
   between two existing ones without renumbering the show.

Both are mission-critical for the projection-mapping use case the
project targets. They had been deferred behind the Phase D feature
queue but moved up after the Phase C single-machine MVP closed, since
without them the timeline is essentially a glorified video player.

## Decisions

### D1 — Sections are timeline-owned **break points**, not regions or per-clip metadata

A section is `{breakFrame, name, color, fadeSeconds}` — a single point
on the timeline. Implicit "section spans" for UI display are computed
from adjacent breaks: span N = `[break[N].frame, break[N+1].frame)`.
The data type stores breaks, never spans.

**Why not regions** (`{name, start, end, color}`, the schema we had at
v8): regions force the data model to express constraints it can't
actually enforce — overlap, gaps, ordering. Two adjacent regions
sharing a boundary frame create two coincident points where the user
authored one. The break-point model has exactly one representation per
authored intent.

**Why not per-clip section metadata**: clips don't "belong" to
sections — they relate positionally only. A clip can start at a break,
end at a break, cross through one, or sit entirely inside a span. Any
of those relationships is computable from the clip's `[startFrame,
startFrame + duration)` against the sorted break list. Embedding
membership in the clip would force the membership to be recomputed
every time a break or clip moves.

### D2 — Per-clip `sectionBehavior` is a behavior policy, not a section reference

Each clip has `enum SectionBehavior { Normal, Locked }`. At any break:

- **Locked** clips freeze with the playhead — `mapToMediaFrame` keeps
  returning the break-frame's mapped source frame regardless of how
  long the timeline is paused.
- **Normal** clips continue per their `PlaybackMode` (Loop / PingPong /
  Freeze) past the break for as long as the playhead is parked.

The flag is a policy that fires at *any* break, not a reference to a
specific section. This is what lets a clip cross multiple sections
naturally: if the user wants the same clip to behave the same way at
every break, one flag suffices; if they want it Locked at one break
and Normal at another, they split the clip.

### D3 — Continuation-phase decoupling for Normal clips

`PlaybackTimeAuthority::mapToMediaFrame(clip, timelineFrame)` is a
pure function. Pause the timeline → `timelineFrame` stops growing →
all clips freeze. That's exactly right for Locked, and exactly wrong
for Normal: `Loop` and `PingPong` need *something* growing to drive
their wraparound math.

**Decision:** introduce a per-clip ECS component
`ClipPlaybackPhase { double sourcePhaseFrames; bool inContinuation; }`
that the Director advances each tick while the timeline is paused at
a break. `mapToMediaFrame` consults this phase via a
3-arg overload `mapToMediaFrame(entity, clip, timelineFrame)` whenever
both `inContinuation == true` AND `clip.sectionBehavior == Normal`;
otherwise it falls through to the original 2-arg pure-function path.

The 2-arg overload remains byte-identical so unit tests (and any
external caller) keep their existing invariants.

`SectionScheduler` (Director-side) seeds the phase on break-hit
(`sourcePhaseFrames = (breakFrame - clipStart) × (clipFps /
timelineFps)`), advances it each tick during at-break, and clears it
on `go()`. The component is runtime-only — *not* persisted by
`ProjectSerializer`, since the phase is meaningful only relative to a
specific paused-at-break state that is itself transient.

Keyframes (`AnimatedProperties`) intentionally remain keyed on
`Timeline::getCurrentFrame()`. They freeze at the break-time value
while paused, matching the convention operators expect — animated
properties stay stamped at the moment of the break, while the clip
content cycles underneath.

### D4 — Spacebar is hybrid: GO at-break, play/pause otherwise

Spacebar dispatches `SectionGoCommand` when `Timeline::sectionAtBreak()`
returns true, otherwise the existing `TogglePlayPauseCommand`. The
operator never has to look down at a keyboard chart — the same key
means "advance the show" at every break and "rehearse this section"
in between. This is the lighting-console GO-button convention applied
to a video timeline.

The at-break latch must clear when the user resumes by *any* path,
not just `SectionGoCommand`. If a script fires `Play` or the user
clicks the UI play button, the next `tick()` clears the latch on
detecting the Paused→Playing transition past the last break frame.
Otherwise the next spacebar would route to GO and yank the playhead
back to the previous break.

### D5 — Cue numbers are `double`, sorted vector, binary search on lookup

Lighting-console convention: cues are decimal-numbered for in-place
insertion (1 → 1.5 → 1.6 → 2). `double` rather than int captures the
common case directly.

The cue list is a `std::vector<CueTag>` kept sorted by number. Insert
is rare (UI authoring), fire is the hot path (binary search). Duplicate
numbers are rejected at insert time; edits that would create a
duplicate are rejected with a non-fatal failure return.

Cue payload is `{number, timestamp, label}` only — no per-cue actions
yet. Per-cue actions (light cue, OSC out, MIDI program change) belong
to the show-control epics (#1 LTC/MTC, #14 MSC) and are deliberately
out of scope here.

### D6 — Section fade envelopes auto-applied on alignment

When a clip's start frame coincides with a `Section::breakFrame` (±1
frame snap tolerance) AND that break's `fadeSeconds > 0`, an opacity
ramp 0→1 is applied for `fadeSeconds` seconds from the clip's start.
Symmetric on clip end → break alignment for fade-out.

The multiplier is computed Director-side in `PlaybackTimeAuthority`
each tick, stamped onto `bus::ClipRenderState::sectionFadeMultiplier`,
and applied Renderer-side in `PlaybackPresenter::present()`. Mid-clip
crossings and clips that don't touch a break frame get multiplier
1.0 (no envelope) — the feature is opt-in by alignment, not an
ambient effect.

### D7 — Schema evolution stays at PROJECT_VERSION 9

Phase A bumps 8 → 9 to add `cues[]`. Phase B reshapes
`sections[]` from `{start, end}` regions to `{breakFrame, fadeSeconds,
...}` break points and adds `clips[].sectionBehavior` — but does so
within v9 because the loader migrates old `{start, end}` entries
inline (each emits two break points). No separate version step.

`ClipPlaybackPhase` is intentionally not persisted. Loading a project
that was paused at a break never restores the at-break state — that's
ephemeral runtime state, like the GLFW window position or the
ImGui dock layout (the latter is persisted by ImGui itself, not the
project file).

## Consequences

**Enables**:

- Lighting-console-style spacebar GO advance through structured shows.
- Clip-level decision per break: cycle a background loop while a
  foreground clip freezes on its last frame — set them to Normal-Loop
  and Locked-Freeze respectively, and the same break advances them
  both correctly.
- Decimal cue numbers map directly to the operator's mental model.
- Auto-crossfade at boundaries: ending clip aligned to break with
  `fadeSeconds=N` + starting clip aligned same break with same fade →
  visual crossfade with zero per-clip keyframing.

**Forces**:

- The 3-arg `mapToMediaFrame` overload exists alongside the 2-arg
  one. Anyone touching the math touches both paths or risks
  divergence. The 2-arg overload is preserved verbatim as a
  regression guard for unit tests.
- `SectionScheduler` reaches into the registry to seed and advance
  `ClipPlaybackPhase`. That's a Director-side write to ECS state and
  must run before `PlaybackTimeAuthority::buildActiveSet` /
  `buildRenderFrame` each tick (Engine's update loop is the
  ordering authority).
- Section refactor from regions → break points is a one-way schema
  change. The legacy `AddSectionCommand` is preserved as a deprecated
  alias that emits two break points per region, so old test scripts
  still drive the new code path. Old project files load via the same
  migration on the persistence side.

**Costs**:

- The continuation-phase component allocates one component per active
  clip per session (lazy, on first break crossing) and is never freed.
  This is intentional: keeping the component allocated avoids the
  per-break-crossing alloc/free cycle. At hundreds of clips this is a
  few KB total — well below the noise floor.
- Section fade math runs in `buildRenderFrame` for every active clip
  every tick, even when no breaks have fades. The hot-path test is
  `if (sections.empty()) return 1.0f;` so the cost is bounded by the
  authored break count (typically <50 per show).

## Alternatives considered

**Per-section per-clip metadata table** (clip × section → behavior).
Rejected: forces a re-resolve of the table whenever any break or clip
moves, and represents the same information as the per-clip flag plus
the positional relationship.

**Region-based sections** (kept in v8). Rejected per D1.

**Spacebar always = play/pause; GO is a separate keybind**. Rejected:
the operator's hand position during a show is on the spacebar, not
hunting for a separate key. The hybrid behavior is unambiguous in
context (the playhead is parked at a break or it isn't).

**Cue numbers as int + insertion sort with explicit renumbering**.
Rejected: programmers who've worked the lighting side of a show floor
will not tolerate having to renumber. Decimal cues are the
30-year-old standard.

**Continuation via timeline-frame extrapolation** (pretend the
timeline kept advancing while paused, just for active clips). Rejected:
breaks the invariant that `timelineFrame == Timeline::getCurrentFrame()`
everywhere it's used. The decoupled-phase approach localizes the
deviation to clips with `inContinuation == true` and falls through to
the existing pure path otherwise.

**Persist `ClipPlaybackPhase` in the project file**. Rejected: the
phase is meaningful only relative to a specific paused-at-break state.
A project loaded onto a timeline at frame 0 with no breaks yet hit
has no useful continuation phase to restore — and trying to restore
one creates a confusing first-tick where playback "starts" mid-cycle.

## References

- Working plan: `~/.claude/plans/where-are-we-on-sunny-sutherland.md`
- Implementation commits: `707fcd0`, `1c890fa`, `483ee21`, `2409d74`,
  `ec04926`, `6121995`
- Related: `docs/adr/0003-director-renderer-split.md` (the boundary
  contract that `SectionScheduler` and `PlaybackPresenter` respect)
- Memory: `feedback_no_competitor_names.md` — mechanism described
  generically, no source-product names

## Amendment 2026-05-07: hold + fade-after-break + name removal

Three follow-up decisions landed together (working plan
`~/.claude/plans/a-few-notes-on-calm-cascade.md`):

- **(a) `Section::name` removed** (schema bumped 9 → 10). Operator
  workflow showed the field was redundant against the cue-tag label
  channel; one fewer text input in the section context menu, one
  fewer string to migrate when the storage shape moves.

- **(b) Zero-breaks renders as one implicit section** spanning
  `[0, timelineEnd]` using a default tint. The model already permitted
  the break vector to be empty; the UI now treats that case as a
  single span instead of "no sections," eliminating a special
  zero-break branch in the renderer.

- **(c) Fade semantic changed: hold + fade *after* the break.**
  Previously the fade-out window was the last `fadeFrames` *before*
  `clipEnd` and the clip dropped from the active set at `clipEnd`. New
  behavior: clip plays at full alpha through to `clipEnd`, then the
  last decoded source frame is held and faded `1 → 0` over
  `fadeSeconds` *starting at* the break. Motivates the user request
  *"persist until the fade has completed"* — the outgoing image
  shouldn't disappear at the same moment the playhead parks.

  Mechanism:
  - `PlaybackTimeAuthority::isClipActiveAtFrame` extends the clip's
    active span by `sectionFadeTailFrames(endFrame)` when the clip's
    end aligns (±1 frame snap) with a break carrying non-zero
    `fadeSeconds`.
  - `mapToMediaFrame(clip, ...)` short-circuits to the last decoded
    source frame inside that tail window (no decoder seek triggered).
  - `mapToMediaFrame(entity, clip, ...)` prefers
    `ClipPlaybackPhase::tailHoldMediaFrame` if it was snapshotted —
    `SectionScheduler::go()` records what the clip was actually
    showing at the moment of GO so a Loop / PingPong clip that was
    cycling at the break holds the user-visible frame, not whatever
    the timeline-frozen path would have computed.
  - `computeSectionFadeMultiplier` fade-out window is now
    `[clipEnd, clipEnd + fadeFrames)` with `t = 1 - (current - end)
    / fadeFrames`, which yields `1.0` at `currentFrame == clipEnd`
    (matches the at-break held look) and ramps linearly to `0.0`.

  Cross-fade across the break: outgoing clip held + ramping out,
  incoming clip starting + ramping in, both over the same
  `fadeSeconds`.

  `fadeSeconds == 0` is unchanged — tail length 0, instant cut at the
  break, identical to pre-amendment behavior for that case.

## Amendment 2026-05-08: at-break visibility + post-break anchor

Round-2 fixups landed after manual smoke (working plan
`~/.claude/plans/calm-beaming-pebble.md`). Three orthogonal changes;
the first two strengthen the at-break / GO state model, the third
adjusts the snap-to-grid UI semantic.

- **(a) At-break visibility gate.** Clips whose `startFrame` aligns
  with the current break (±1 frame snap) stay at fade multiplier
  `0.0` for the duration of the at-break pause, regardless of
  `fadeSeconds`. The clip remains in the active set so its decoder
  pre-rolls and the texture is fresh at GO; the compositor simply
  draws at alpha 0. User requirement: "things after the break
  shouldn't start yet until we resume." Implemented as an early
  return at the top of
  `PlaybackTimeAuthority::computeSectionFadeMultiplier`, gated on
  `Timeline::sectionAtBreak()` so the suppression releases naturally
  when GO flips that flag false. Authored fade-in (if any) resumes
  on the next tick because the existing fade-in branch sees
  `currentFrame == clipStart` post-resume.

  Rejected alternative: dropping the clip from
  `bus::RenderFrame::activeClips` during the pause. Tested first;
  produced a stale-texture flash on the first post-GO frame because
  the decoder hadn't pre-rolled.

- **(b) Post-break media anchor.** Supplements (and does not replace)
  the Phase D continuation-phase decoupling. At GO, for every
  Normal-mode clip that was *spanning* the break (not ending at it
  — those are already owned by the Phase 6 tail-hold snapshot),
  `SectionScheduler::go()` calls `snapshotPostBreakAnchors`. That
  snapshots, on the clip's `ClipPlaybackPhase`, the source-media
  frame the clip was visibly mapping to at the moment of GO
  (`postBreakMediaAnchor`) plus the timeline frame at which playback
  resumes (`anchorTimelineFrame = breakFrame + 1`). User
  requirement: "the end of the pause should pretty much act as if
  nothing has happened from a playback standpoint."

  Post-GO, both the 3-arg `PlaybackTimeAuthority::mapToMediaFrame`
  and `DecodeSystem`'s per-clip update consult the anchor when
  `postBreakMediaAnchor >= 0`, deriving the source frame as
  `(anchor - mediaStartFrame) + (timelineFrame - anchorTimelineFrame)
  * frameRateRatio` and applying the existing Freeze / Loop /
  PingPong wrap. This avoids the rewind that the natural
  `(timelineFrame - clipStart) * ratio` mapping would produce,
  because the natural mapping discards the source-frame phase that
  accumulated during the pause.

  **Lifecycle**:
  - Anchor is **set** by `go()` after `snapshotTailHoldFrames` and
    before `clearAllContinuation`. Tail-hold owns clips ending at
    the break; the anchor only stamps clips that span past it.
  - Anchor **survives** `clearAllContinuation`. The clear zeros
    only `inContinuation` and `sourcePhaseFrames`; the anchor and
    `tailHoldMediaFrame` are intentionally preserved.
  - On a subsequent break-B crossing, `seedContinuationAt`
    consults `postBreakMediaAnchor` and seeds `sourcePhaseFrames`
    from `(anchor - mediaStartFrame) + (breakB - anchorTimelineFrame)
    * ratio` instead of `(breakB - clipStart) * ratio`. Without
    this, the at-break-B pause would visibly jump backward for a
    multi-break clip.
  - Anchor is **invalidated** by `resetAnchorsAcrossScrub`, fired
    from `tick()`'s discontinuity branch and from the Stopped
    branch. Any anchor whose `anchorTimelineFrame > currentFrame`
    clears (sentinel `postBreakMediaAnchor = -1`,
    `anchorTimelineFrame = 0`). Backward scrubs past the setting
    break thus clear; forward scrubs that skip the at-break pause
    never accrued an anchor in the first place.

  Priority order in 3-arg `mapToMediaFrame` after this amendment:
  (1) tail-hold short-circuit when `timelineFrame >= clipEnd`,
  (2) `inContinuation && Normal` continuation-phase mapping,
  (3) post-break anchor mapping when set, (4) fall through to
  the 2-arg natural-mapping overload.

- **(c) Snap-to-grid floor.** UI semantic change in
  `TimelineWidget::snapTimeToTickGrid`: a click resolves to the
  start of the cell the cursor is in, not the closest tick.
  Frame derivation is inlined (no longer routes through
  `Timeline::timeToFrame`, which itself rounds), and the tick
  alignment uses integer-division floor (no `+ tickEvery/2`
  offset). Side effect: `snapTimeToBest`'s grid candidate is now
  systematically the *previous* tick rather than the closest, so
  drag operations (cue drag, clip trim via best-snap) lean left.
  Tradeoff accepted for predictability — operators reported the
  old round-to-nearest behavior felt "tacked on the back of the
  frame" relative to where they clicked.

  Cue-add and section-break-add modal seeds inherit the floor —
  the right-click position resolves to the start of the clicked
  frame so the modal pre-fills the same value the user perceives
  the cursor is on.

**Tests** (all green at the time of this amendment): existing
`SectionFadeTests`, `SectionBreakTests`, `ClipPlaybackPhaseTests`
unchanged; new `tests/unit/PostBreakAnchorTests.cpp` covers
post-GO anchor mapping (Loop), PingPong cycle parity preservation,
multi-break re-seed using a carry-forward anchor, and backward
scrub clearing the anchor. New integration scripts:
`scripts/integration/at_break_starting_clip_invisible.json` and
`scripts/integration/post_break_no_rewind.json`.

## Amendment 2026-05-07: at-break state ownership + section UX + drag snap

Round-3 fixups landed after a second pass of manual smoke (working
plan `~/.claude/plans/implement-plan-woolly-wilkes.md`).
Six findings clustered into three buckets; the resolutions tighten
state ownership for the at-break latch, polish section UX, and
restore predictable grid-snap on clip drag/trim.

- **(a) At-break state ownership.** `Timeline::sectionAtBreak()` is
  the latch that gates the Phase 5 visibility suppression and the
  spacebar GO dispatch. Round 2 amended-(a) introduced the gate
  itself; round 3 specifies who owns the flag and pins down all
  paths that clear it.

  **Owner:** `SectionScheduler::tick()`'s park-at-break branch is
  the *only* path that raises the flag. Anyone else (manual seek,
  scrub, command playback) only ever clears it.

  **Clearing paths** (any one of these returns the latch to false):
  1. `Timeline::seek()` — every manual seek clears the latch
     unconditionally as its last act, before logging. Catches UI
     scrubs, script `SeekToFrame`, and command-playback seeks
     dispatched through the Timeline directly.
  2. `SectionScheduler::tick()` discontinuity branch — when the
     per-tick `delta` exceeds the threshold, the scheduler also
     clears `m_atBreak` + `m_timeline->setSectionAtBreak(false)`
     and calls `clearAllContinuation()`. Belt-and-braces against
     paths that mutate `currentTime` without going through
     `Timeline::seek()`.
  3. `SectionScheduler::tick()` manual-resume unstick — any
     state-Playing tick where the flag is still set drops it
     unconditionally. The previous `currentTime > m_lastBreakHitFrame`
     guard failed when the user scrubbed BACKWARD before pressing
     Play (the user's reported issue #3).
  4. `SectionScheduler::go()` and the existing Stopped branch
     (unchanged from prior amendments).

  **Bug class fixed:** when `sectionAtBreak()` was stuck true after
  one of paths (1–3) was missed, the Phase 5 visibility gate at
  `PlaybackTimeAuthority::computeSectionFadeMultiplier` would hide
  any clip with `startFrame ≈ currentFrame` — even when the user was
  nowhere near a break. The user reported this as "first-section
  clip won't play until you cross a break" (issue #3) and as
  "clip placed at a break flashes and disappears" (issue #6).

  **Tests:** new unit case `SectionBreakTest.ManualSeek_ClearsAtSectionBreakFlag`
  pins the `Timeline::seek()` clearing semantic at the data-model
  level (no scheduler involved). New integration script
  `scripts/integration/scrub_clears_at_break.json` exercises the
  full park → backward scrub → Play path. New integration script
  `scripts/integration/first_section_plays.json` is the issue #3
  regression guard for a clip entirely inside the first section.

- **(b) Section color palette + dialog-less add.** Pre-round-3 the
  add-section-break flow opened a modal that asked for color and
  fade in addition to the frame; the implicit first segment had no
  color (rendered neutral gray). User feedback: the modal added
  friction the operator didn't want, and the gray first segment
  read as "not a section" rather than "the first section."

  **Resolutions:**
  - 8-entry `entity::sectionPalette` ABGR palette (declared in
    `include/entity/timeline/Timeline.hpp`). `pickColor(index)`
    returns `palette[index % 8]`. Implicit first segment uses
    `palette[0]` whenever the timeline has at least one clip;
    truly-empty timelines (no breaks AND no clips) keep the old
    gray fallback so the band reads as "no content yet" not
    "this is section one."
  - Ruler right-click → "Add Section Break Here" enqueues
    `AddSectionBreakCommand` directly with auto-color
    `pickColor(getSections().size() + 1)` (so palette[0] is
    reserved for the implicit first segment, palette[1] for the
    first user-added break). No modal, no trailing dots in the
    menu label, fade defaults to 0.
  - The section EDIT modal stays put as the override mechanism —
    right-click an existing break line and the same dialog opens
    with both color and fade editable. Add path is fast; edit
    path retains full control.
  - `addSectionBreak`'s `0xFF6090C8` default argument is
    deliberately preserved for backward-compat with serialized
    projects and scripted creation. `palette[0] == 0xFF6090C8`
    too, so default-color creation lands on the same hue the UI
    paints the implicit first segment with. This isn't load-bearing
    but it keeps two surfaces visually consistent.

  **Tests:** new unit file `tests/unit/SectionAutoColorTests.cpp`
  covers palette size = 8, palette[0] match against `addSectionBreak`'s
  default, distinct entries across the palette, and modulo wrap at
  8/9/15/16/99 + a `1<<32` sanity case.

- **(c) Cue-add reachable from the ruler menu.** Round-2 Phase 2
  moved cue ops to a dedicated cue-lane right-click band (so the
  ruler menu wasn't cluttered). Operator feedback after smoke was
  that the cue lane is small and easy to miss when the cursor is
  already on the ruler. Round 3 reverses that decision partially:
  the ruler menu now has BOTH "Add Section Break Here" and "Add
  Cue Here..." (cue add still trails a modal because the operator
  needs to set the cue number + label). The cue-lane menu stays
  as the faster shortcut for users who reach for the lane band.
  Both surfaces yield the same `AddCueCommand` payload.

- **(d) Drag snap honors tick grid.** Pre-round-3 the clip drag
  handler ranked snap candidates (playhead, snapTimeToBest,
  cross-clip edges) by raw distance against the cursor. When a
  raw playhead/clip-edge candidate happened to sit closer than
  the nearest tick, the raw 1-frame-aligned candidate won — even
  at coarse zoom (5f, 10f tick) where the user expected
  grid-aligned drops. User feedback: "I'm at 5f tick and clips
  drop at 1f boundaries."

  **Resolution:** unified `considerCandidate(raw, targetForStart)`
  lambda inside `TimelineWidget::handleTracksInteraction`. A
  `floorToGrid` helper pre-floors every candidate to the tick
  grid when grid-snap is in effect. `gridSnap = !ImGui::GetIO().KeyShift`,
  so:
  - **Default (no shift):** every candidate (playhead, cue,
    section break, cross-clip edge) competes at tick-grid
    resolution. The playhead can no longer "win" with a 1-frame
    pull when the user is dragging on a 5f tick zoom.
  - **Shift held:** candidates compete at raw resolution.
    Playhead/clip-edge snap continues to apply (these are
    operator-meaningful targets, not grid-meaningful), but no
    tick-grid floor is enforced. Frame-precise placement at any
    zoom level.

  Trim mirrors the same shape: the `mouseTime = snapTimeToBest(...)`
  call is gated on `!io.KeyShift`. With shift held, trim lands at
  the raw mouse frame; without it, the existing grid + cue +
  section snap bundle applies.

  Why not invert the modifier (shift = strict grid, default = mixed):
  the user's report was that the default was unpredictable, not
  that grid-snap was unwanted. The shift-bypass mirrors NLE
  convention (Premiere, Resolve, Avid) where the modifier loosens
  snap rather than tightening it.

- **(e) Trim handle reachability at high zoom.** Distinct from
  the snap fix above: at 1f / 2f tick zoom, clip trim handles were
  visually unreachable for clips that extended past the visible
  viewport. Diagnosis traced the bug to `findClipEdgeAtPosition`
  computing `clipX = windowPos.x + timeToPixel(startTime) - m_syncScrollX`
  while `findClipAtPosition` and `renderClip` both compute
  `clipX = windowPos.x + timeToPixel(startTime)` (no scroll
  subtraction). `windowPos == m_tracksScreenPos == GetCursorScreenPos()`
  inside the scrolled child window already accounts for scroll;
  the extra subtraction was a coordinate-space double-count that
  shifted the trim hit-test away from the visible clip by
  `m_syncScrollX` pixels. Worst at 1f/2f zoom because that's
  where horizontal scroll matters most.

  **Resolution:** dropped the spurious `- m_syncScrollX` from
  `findClipEdgeAtPosition`, plus added a clamp on the half-hit-zone
  width (`clamp(TRIM_EDGE_WIDTH/2, 3px, clipWidth * 0.4)`) so
  sub-15px clips still distinguish left and right halves.
  Wider clips keep the original 4px half so default-zoom muscle
  memory is unchanged. No data-model or schema change.

**Tests at this amendment** (all green at landing): existing
`SectionFadeTests`, `SectionBreakTests`, `ClipPlaybackPhaseTests`,
`PostBreakAnchorTests` unchanged; `SectionBreakTests` extended
with the `ManualSeek_ClearsAtSectionBreakFlag` cases; new
`SectionAutoColorTests` for the palette. New integration scripts
`scripts/integration/scrub_clears_at_break.json` and
`scripts/integration/first_section_plays.json`. Drag-snap and
trim-handle changes are pure UI and have no automated coverage —
manual smoke at 1f / 5f / 10f tick zoom is the verification path.

## Amendment 2026-05-07: queued-at-break semantic + in/out points

Round-4 fixups landed after the user reported that a clip aligned
exactly with a section break was running at `mediaStartFrame + 1` on
the first visible post-GO tick, not `mediaStartFrame` (working plan
`~/.claude/plans/quizzical-waddling-mitten.md`). The diagnosis drew a
distinction the prior amendments hadn't named explicitly: a clip
*starting at* a break is structurally different from a clip *spanning*
or *ending at* one. The amendments below pin that distinction in the
data model and add user-editable in/out points so the operator can
re-window a clip without rebuilding the timeline.

- **(a) "Queued at break" semantic.** A clip whose `startFrame`
  aligns (±1 timeline frame) with a section break is **queued** —
  waiting for GO to begin playback — and not running through the
  break. It is a distinct state from the "spanning" case
  (`clipStart < breakFrame < clipEnd`) covered by the round-2
  post-break anchor model. Queued clips do NOT participate in
  continuation phase or post-break anchor stamping:
  `SectionScheduler::seedContinuationAt` and
  `snapshotPostBreakAnchors` both early-out for them. Without this
  skip, the at-break advance would push `sourcePhaseFrames` past
  zero before the clip has visibly started, and the post-GO
  mapping would land on a non-zero source frame instead of the
  user-authored in-point.

- **(b) 1-tick pre-roll.** The at-break tick
  (`currentTLFrame == clipStart == breakFrame`) is invisible by
  design — the round-2 visibility gate holds the clip at fade
  multiplier 0.0 while `Timeline::sectionAtBreak()` is true. The
  first *visible* post-GO tick (`currentTLFrame == breakFrame + 1`)
  must therefore map to `clip.mediaStartFrame`, not
  `mediaStartFrame + 1`. The post-GO anchor for queued clips is
  stamped at `(mediaStartFrame, clipStart + 1)`, threading the
  queued case through the existing anchor branch. (Round-4 originally
  landed this as a 1-frame shift in a separate priority-1 short-circuit
  via `shiftedTimelineFrameForQueued`; the round-5 amendment below
  reverted that design to anchor unification after a multi-break
  drift regression.)

- **(c) Distinct from spanning.** A clip running THROUGH a break
  (`clipStart < breakFrame < clipStart + duration`) still uses
  `postBreakMediaAnchor` and the existing continuation / anchor
  path from the prior round-2 amendment. With the round-5
  reversion (below), queued clips share that same anchor branch
  rather than living in their own priority-1 short-circuit; what
  separates them from spanning is solely the *value* stamped on
  the anchor (`(mediaStartFrame, clipStart + 1)` for queued vs.
  the at-GO source frame + `breakFrame + 1` for spanning), not a
  different code path. The unit regression guard
  `PostBreakAnchorTest.SpanningClipStillUsesAnchor` pins the
  spanning anchor's stamped values so a future change to
  `snapshotPostBreakAnchors` can't silently collapse the two
  cases together.

  Priority order in 3-arg `mapToMediaFrame` after the round-5
  reversion below — same 4-step list as the round-2 amendment:
  (1) tail-hold short-circuit when `timelineFrame >= clipEnd`,
  (2) `inContinuation && Normal` continuation-phase mapping,
  (3) post-break anchor mapping when set, (4) fall through to
  the 2-arg natural-mapping overload. Round-4's separate
  priority-1 queued branch is gone; the queued case is handled
  by branch (3) once `snapshotPostBreakAnchors` stamps the
  anchor at GO.

- **(d) In/out points are user-editable.** PropertyWindow's clip
  info section now exposes the in-point (`mediaStartFrame`) and a
  derived out-point as `ImGui::DragInt` fields with
  `ImGuiSliderFlags_AlwaysClamp`. Out-point edits translate to
  `clip.duration` via the inverse mapping
  `duration = ceil((outPoint - mediaStartFrame) / frameRateRatio)`,
  so the timeline footprint adjusts to match. In-point edits are a
  slip — duration unchanged, the source window slides.

  **No data-model or schema change.** `mediaStartFrame` and
  `duration` were already serialized; existing project files load
  and save unchanged. The edits route through new
  `SetClipMediaStartFrameCommand` and the existing
  `SetClipDurationCommand` via `CommandDispatcher::enqueue` (per
  the `include/entity/director/CLAUDE.md` boundary rule that UI
  panels mutate state through commands, never direct field
  writes), with pre-edit snapshots captured on
  `IsItemActivated()` so undo restores the original values.
  Clamping is symmetric on both sides: the UI uses
  `AlwaysClamp` for live drag bounds, the command's `execute()`
  re-validates on the dispatcher side so a malformed scripted
  invocation is rejected.

  **Known follow-up:** the project loader at
  `ProjectSerializer.cpp` recomputes `clip.duration` from
  `totalMediaFrames * (timelineFps / clipFps)` whenever
  `totalMediaFrames > 0`, treating the saved JSON `duration` only
  as a fallback for malformed clips. A user out-point trim
  therefore does not survive a save+reload — the loaded duration
  reverts to the natural full-length mapping. This is pre-existing
  loader behavior shared with timeline-trim drags and scripted
  `SetClipDurationCommand`, not introduced by this amendment, but
  the new UI surfaces it more visibly. Tracked separately for a
  serializer fix-up.

**Tests** (all green at landing): `tests/unit/PostBreakAnchorTests.cpp`
extended with four cases —
`AtBreakAlignedClipDoesNotAccumulatePhase`,
`AtBreakAlignedClipNoAnchorStamped`,
`AtBreakAlignedPostGoMappingFirstVisibleEqualsInPoint` (covers
both `mediaStartFrame == 0` and a non-zero offset), and
`SpanningClipStillUsesAnchor` as the regression guard for (c).
New integration scripts
`scripts/integration/at_break_clip_starts_at_inpoint.json` and
`scripts/integration/at_break_clip_starts_at_inpoint_offset.json`
exercise the full park-then-GO end-to-end flow with
`AssertClipMediaFrame` pinned to the in-point at `breakFrame + 1`.
The in/out point UI is pure ImGui drag widget; no automated
coverage — manual verification path is the standard PropertyWindow
smoke (drag the In Point slider, verify the source window slides
on the stage; drag the Out Point slider, verify the timeline
footprint adjusts).

## Amendment 2026-05-07: queued-clip multi-break drift fix (anchor unification)

Round-5 fix-forward landed after smoke-testing the round-4 queued
shift surfaced a multi-break drift regression (working plan
`~/.claude/plans/quizzical-wad-merry-dongarra.md`).
The shift solved the at-break tick correctly but introduced a
structural presenter / decoder priority disagreement that broke
playback at every *subsequent* break. The reversion below replaces
the shift with **anchor unification** — queued clips reuse the
existing post-break anchor branch instead of living in their own
priority-1 short-circuit.

- **(a) Regression observed.** With the round-4 design in place,
  any clip whose `startFrame` aligned with a break was rendered
  through a permanent `timelineFrame - 1` shift in the 3-arg
  `mapToMediaFrame` (priority 1) for the entire clip duration. At
  the same time, `DecodeSystem::update` applied the shift only as
  a tentative starting point and then *overwrote* it with
  `phase.sourcePhaseFrames` (continuation override) or the
  anchor-derived frame whenever those fields were stamped on a
  later break. Result: at break-A the presenter and decoder agreed
  (queued semantic, both saw `mediaStartFrame`); past break-A the
  decoder followed natural mapping (presenter stayed on shifted
  mapping, off by 1); at break-B the decoder followed the
  continuation phase walking forward at clip framerate (presenter
  stayed frozen on the timeline-derived shift, drift accumulating
  ~60 frames per second of park); at GO from break-B the anchor
  was stamped from the advanced phase (decoder switched to the
  anchor's source frame, presenter stayed on the shift) producing
  a `frameDelta` large enough to exceed `SEEK_HYSTERESIS=8` and
  trigger a backward seek. The visible symptom was the queued
  clip playing through break-A correctly, then pausing for the
  ring-buffer flush + decode-ahead window at every subsequent
  break.

- **(b) Fix — anchor unification.** Replace the queued shift with
  a stamped anchor at `(mediaStartFrame, clipStart + 1)`:
  - `SectionScheduler::seedContinuationAt` lifts
    `get_or_emplace<ClipPlaybackPhase>` above the queued check so
    the phase exists on both queued and spanning paths. Queued
    clips set `inContinuation = false` and
    `sourcePhaseFrames = 0.0`, then `continue` (no continuation
    accumulation).
  - `SectionScheduler::snapshotPostBreakAnchors` runs the queued
    check **before** the existing `if (!phase.inContinuation)
    continue;` guard (queued phase has `inContinuation == false`,
    so a wrongly-ordered guard would skip the stamp). Queued
    clips receive `phase.postBreakMediaAnchor = clip.mediaStartFrame`
    and `phase.anchorTimelineFrame = clip.startFrame + 1`.
  - The 3-arg `PlaybackTimeAuthority::mapToMediaFrame` queued
    short-circuit is removed entirely. Control falls through to
    the existing anchor branch, which at `timelineFrame ==
    clipStart + 1` computes `timelineDelta = 0` and returns
    `mediaStartFrame + 0` — the in-point.
  - `DecodeSystem::update` similarly drops the queued shift in
    both bootstrap and active paths. The existing anchor override
    below it already steers the worker target by the same anchor
    the presenter is using, so they stay in sync without a
    parallel shift.
  - `include/entity/director/QueuedClipMapping.hpp` is deleted.
    No remaining consumers.

  At every subsequent break (break-B, break-C, …) the queued
  clip is now indistinguishable from a "freshly-anchored" clip:
  `seedContinuationAt`'s carry-forward formula reads
  `postBreakMediaAnchor` and seeds the new continuation phase
  from there, just like a spanning clip. No new branch in the
  hot path; the well-tested spanning code does the work.

- **(c) Why this is structurally safer than the shift.** The
  shift was a permanent priority-1 override that did not
  cooperate with the continuation phase or with any later
  anchor stamp — it kept returning `(timelineFrame - 1) -
  clipStart` regardless of what the rest of the system had
  computed about the clip's source position. Anchor unification
  threads queued clips through the *same* anchor branch as
  spanning clips, so anything that sets, advances, or
  invalidates an anchor (multi-break carry-forward,
  scrub-clear, GO-stamping) automatically does the right thing
  for queued clips too. The presenter and decoder consult the
  anchor through the same branch with the same wrap math; they
  cannot disagree.

- **(d) Test contract update.** `tests/unit/PostBreakAnchorTests.cpp`
  three queued-related cases updated:
  - `AtBreakAlignedClipDoesNotAccumulatePhase` — assertion
    tightened from "phase null OR (`inContinuation==false` AND
    `sourcePhaseFrames==0`)" to "phase exists AND
    `inContinuation==false` AND `sourcePhaseFrames==0`". The
    phase is now always emplaced.
  - `AtBreakAlignedClipNoAnchorStamped` renamed to
    `AtBreakAlignedClipAnchorIsStamped` and inverted: drives
    `seedContinuationAt(60us)` then
    `snapshotPostBreakAnchors(60us)`, asserts
    `postBreakMediaAnchor == clip.mediaStartFrame` and
    `anchorTimelineFrame == clip.startFrame + 1`.
  - `AtBreakAlignedPostGoMappingFirstVisibleEqualsInPoint` —
    assertion values unchanged; both sub-cases now also drive
    `snapshotPostBreakAnchors(60us)` so the anchor branch fires.
    `SpanningClipStillUsesAnchor` and the four pre-existing
    spanning-anchor cases are untouched.

  Integration scripts
  `scripts/integration/at_break_clip_starts_at_inpoint{,_offset}.json`
  pass unchanged — the structural property they assert
  (`mediaFrame == mediaStartFrame` at `clipStart + 1`) is
  preserved by the new anchor-based design.

**Tests at this amendment:** 408/408 enabled green
(137/138/254 skipped/disabled, pre-existing). `ctest -R
PostBreakAnchor` 8/8 passing, including the renamed
`AtBreakAlignedClipAnchorIsStamped` and the four spanning-anchor
guards.

## Amendment 2026-05-22: frame-native section breaks + cue tags (PROJECT_VERSION 24)

Section breaks and cue tags were stored as `Timecode` microseconds even
though both are frame-quantized — a break sits on an integer timeline
frame, never between two. The microsecond representation forced every
consumer to convert back with a hand-rolled
`static_cast<FrameNumber>(us * fps / 1e6)`, which *truncates*. A
round-tripped value (`frameToTime(N)` is `round(N/fps*1e6)`) can land
fractionally below the integer, so truncation reads it back as `N-1`:
frame 250 @ 30fps → 8333333us → truncates to 249.

The visible bugs: a clip whose `startFrame` equalled a section break
stayed fully visible while parked at the break (the at-break gate's exact
`breakFrame == currentFrame` test compared 249 against 250 and missed);
and a layer drag-snapped to a break landed one frame early.

**Decision — markers are frame-native.** `Timeline::Section::breakFrame`
and `CueTag::frame` (renamed from `timestamp`) are now `FrameNumber`
integers. The playhead (`Timeline::m_currentTime`) stays `Timecode`
microseconds — it legitimately needs sub-frame precision during playback.
The only microsecond↔frame conversions are at the playhead boundary, and
they all route through the rounding helpers `Timeline::timeToFrame` /
`frameToTime`. Every hand-rolled `* fps / 1e6` conversion is deleted.
This revises D1 (`breakFrame` is now an integer frame) and D5 (cue
payload `{number, frame, label}`).

**Snap tolerances tightened to exact.** The `±1`-frame tolerances earlier
amendments relied on (`kAtBreakSnapTol`, the at-break `gateSnapTol`, the
fade-in/out and tail-hold `snapTol`, `sectionFadeTailFrames`) existed
only to absorb the truncation fuzz. With exact integer frames they would
over-match — a clip starting one real frame past a break would wrongly
get the at-break gate or a fade envelope. All section alignment is now
exact equality: a clip is "at a break" iff its start/end frame equals the
break frame exactly. Deliberate behavior change.

**Schema v24.** `PROJECT_VERSION` 23 → 24. Section `breakFrame` and cue
`frame` are written as integer frames. The loader migrates pre-v24 files:
microsecond `breakFrame` / `timestamp` values convert via `timeToFrame`
at the project's frame rate (parsed before sections/cues). The
pre-Phase-B `{start, end}` legacy section form migrates the same way.

**Script command JSON is frame-native.** `AddSectionBreak` /
`RemoveSectionBreak` / `EditSectionBreak` and `AddCueAt` / `EditCue` take
integer frames; the cue position key is renamed `timestamp` → `frame`.
The deprecated `AddSectionCommand` keeps its legacy microsecond `{start,
end}` JSON and converts internally on execute.

**Clip drag/trim rounding.** Separately, the clip drag-commit and
trim-commit converted the dragged microsecond position to a frame with a
`float`-precision truncating cast. Those now route through
`Timeline::timeToFrame` (rounding) so a clip snapped to a break / cue /
grid line lands exactly on it.

**Tests:** new `SectionFadeTest.AtBreakGate_*` cases (the headline-bug
red test), `MessageBusSerialization.SectionBreakDetectedRoundTrip`,
`ProjectSerializer.SectionsAndCuesRoundTripV24` +
`PreV24SectionsAndCuesMigrateMicrosecondsToFrames`. `SectionFadeTests`
case 5 rewritten for exact alignment; `PostBreakAnchorTests` /
`CueTagTests` updated to the frame-native API; section/cue integration
scripts rewritten microseconds → frames. 543/543 unit tests green; all
27 section + cue integration scripts green.

## Amendment 2026-05-23: Normal-mode break-aligned active-window extension

D2 said: "Locked clips freeze with the playhead … Normal clips continue
per their PlaybackMode." That phrasing was about *what plays* during the
at-break park — and the implementation took it literally: a Normal-Loop
clip cycled its source frames while parked, a Normal-Freeze clip held
its last source frame, both gated to the clip's authored timeline
window `[startFrame, startFrame + duration)` (plus the section-fade
tail).

In show authoring it's idiomatic to trim a clip's timeline edge to
exactly a section break — "this clip plays for this section." The
clip's right edge lands on the break frame. With D2 as originally
implemented, a Normal-mode clip in that shape lost everything it had
to play past the break the moment the playhead reached `clipEnd` (no
matter how much source content the trimmed in/out range still held).
The operator-visible effect was the clip "pausing" at the break — held
on its final-displayed source frame for the at-break park, then cut
off entirely the moment Section GO fired. The intent of "Normal
continues past the break" was strictly *during* the at-break park, not
*through* it.

**Decision — Normal-mode active window extends to source-out when the
clip's timeline-end aligns with a break.** A Normal-mode clip whose
`startFrame + duration` exactly matches some `Section::breakFrame`
AND has more source content past the break (i.e.
`sourceTimelineFrames > duration`, where `sourceTimelineFrames =
ceil(effectivePlaybackLength(clip) / (clip.framerate /
timelineFrameRate))`) extends its active window to
`startFrame + max(duration, sourceTimelineFrames)`. After Section GO,
the playhead resumes past the break and the existing 3-arg
`mapToMediaFrame` post-break-anchor path naturally plays the
remaining source content through to the source out point; `PlaybackMode`
(Freeze / Loop / PingPong) handles source-end wrap as usual.

Locked clips are untouched. Normal clips whose end *isn't* break-aligned
are untouched (no extension). Normal clips whose source isn't longer
than their authored timeline window are untouched (the `max` is a
no-op). A clip whose extended end happens to align with a second break
gets the standard section-fade tail attached at `extendedEnd`, not at
`clipEnd`.

This narrows the "Normal continues past the break" semantic to its
operator-natural reading: the timeline edge stops being a hard cutoff
for the clip's content, *if* that edge is a break (not arbitrary
clipping) and *if* there's source content to keep playing. The Locked
behavior is unchanged because the explicit point of `Locked` is "pause
at breaks" — the trim is the cutoff there by design.

**Why the gate on break-alignment rather than a universal extension.**
The amendment is deliberately narrow. Trimming a clip's right edge
short of a break is a legitimate authoring choice — "I want this clip
to stop here mid-section." Universalising the extension would change
behaviour for Normal-Loop / PingPong clips whose authors deliberately
trimmed off the back of a cycle. The break-aligned gate keeps the
amendment to the only case the trimming was clearly *incidental* —
the user was carving the clip to the section structure, not picking
a specific source duration.

**Implementation.**

- `PlaybackTimeAuthority::computeExtendedDuration(const Clip&)`
  (public, editor-only): returns `max(clip.duration, sourceTimelineFrames)`
  when Normal + break-aligned, else `clip.duration`. Reads the
  Timeline's section list via the existing `sectionFadeTailFrames` /
  `copySectionsAndRate` thread-safe accessor — callable from editor
  thread (and show thread, since `m_timeline` is set at construction
  and never reassigned).

- File-local helper `computeExtendedDurationImpl(clip, timelineFps,
  endAlignsWithBreak)` shared by editor and show paths — the editor
  wrapper supplies `endAlignsWithBreak = sectionFadeTailFrames(endFrame) > 0`
  from live Timeline state; the show-side wrapper (in
  `mapToMediaFrameFromCatalog`) reads it from the catalog's new
  `endAlignsWithSectionBreak` bool.

- `bus::ClipCatalogEntry` gains `bool endAlignsWithSectionBreak` —
  baked editor-side in `buildSceneSnapshot`. The show side
  reconstructs `extendedDuration` from the catalog's mediaStart /
  mediaOut / framerate + `timelineFrameRate` (already passed in), so
  no Timeline section access is needed on the show thread.
  Backward-compat serialization: defaults to `false`, matches
  pre-amendment behavior for legacy payloads.

- `isClipActiveAtFrame`: after the original `frame < endFrame +
  tailFrames` check, an explicit Normal+break-aligned branch consults
  `computeExtendedDuration` and (when extension applies) returns true
  for `frame < extendedEnd`. A second `sectionFadeTailFrames` lookup
  at `extendedEnd` attaches a held + fade tail if the extended end
  itself aligns with a break.

- Both `mapToMediaFrame` overloads (2-arg + 3-arg) and the show-side
  `mapToMediaFrameFromCatalog` swap their `clipEnd` boundary for
  `realEnd = startFrame + computeExtendedDuration(clip)`. The Fix 5
  hold-last-decoded-frame clamp (Amendment 2026-05-22 follow-up) now
  fires at `realEnd - 1`, not `clipEnd - 1`. For non-extended clips
  `realEnd == clipEnd` — Fix 5 behavior is preserved exactly. For
  extended clips, `localFrame` walks naturally through the extension
  window, hitting Freeze/Loop/PingPong wrap math at source-out as
  before.

- `SectionScheduler::seedContinuationAt` now uses the
  authority-computed active end (`startFrame +
  computeExtendedDuration`) instead of `startFrame + duration` when
  deciding whether the break "fits inside" the clip — so a
  Normal-extended clip ending exactly at this break gets the same
  continuation-phase seed a spanning clip would. During the at-break
  park its source-phase advances on wall-clock as usual; at Section
  GO `snapshotPostBreakAnchors` (also updated to *not* skip break-
  aligned-end clips when extension applies) stamps the
  `postBreakMediaAnchor` so the 3-arg `mapToMediaFrame`'s anchor
  branch picks up natural playback past `clipEnd`.

- `SectionScheduler::snapshotTailHoldFrames` skips Normal-extended
  clips — they're not entering a tail, they're playing through to
  `extendedEnd`. Leaving `tailHoldMediaFrame` unset is safe because
  the held-frame short-circuit is gated on `timelineFrame >= realEnd`,
  which is past `extendedEnd` for extended clips. Locked / non-
  extended Normal clips fall into `snapshotTailHoldFrames` as before
  and the held-frame test (`section_break_tail_no_freeze.json`) still
  asserts the Fix 5 invariant.

**Tests.** Eight new `PlaybackTimeAuthorityTest.NormalExtension_*`
unit cases covering all four predicate combinations
(Normal+break-aligned+long-source, Locked+break-aligned+long-source,
Normal+not-break-aligned, Normal+break-aligned+short-source) plus
natural playthrough, Loop wrap, mixed-fps, and the
fade-tail-at-extended-end edge. New integration test
`normal_mode_extends_past_break.json` exercises the full Play →
at-break-park → SectionGo → playback-past-clipEnd flow. The existing
`section_break_tail_no_freeze.json` is updated to set
`SectionBehavior = Locked` so it stays a held-frame regression guard
under the new semantics (the Normal path now uses extension instead).
624/624 ctest green at `-j 1`.
