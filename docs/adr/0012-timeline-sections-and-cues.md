# ADR-0012: Timeline structural playback — break-point sections, cue tags, and continuation-phase decoupling

- **Status:** Accepted
- **Date:** 2026-05-06
- **Context source:** Working plan
  `~/.claude/plans/where-are-we-on-sunny-sutherland.md`. Implements
  the four-phase epic "Sections + Cue tags".
- **Implemented by:** Six commits on `master`:
  - `57d835c` — Phase A: cue tags (data + UI + persistence at v9)
  - `7b32633` — Phase B: section break-points + Locked playback semantics
  - `7e150d6` — Phase C: Normal continuation phase
  - `885e628` — Phase D: section fade envelopes
  - `b5b836c`, `d527357` — review-driven fixups

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
- Implementation commits: `57d835c`, `7b32633`, `7e150d6`, `885e628`,
  `b5b836c`, `d527357`
- Related: `docs/adr/0003-director-renderer-split.md` (the boundary
  contract that `SectionScheduler` and `PlaybackPresenter` respect)
- Memory: `feedback_no_competitor_names.md` — mechanism described
  generically, no source-product names
