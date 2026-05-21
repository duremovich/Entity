# ADR-0025: Timeline clock — separate rate authority from position authority

- **Status:** Accepted
- **Date:** 2026-05-21
- **Implemented by:** Rate axis — roadmap #3 (audio playback): the
  `RateSource` interface, `SystemRateSource`, `AudioRateSource`, and the
  selector inside `PlaybackTimeAuthority`. Position axis — roadmap #1 (LTC
  ingest): the `PositionSource` interface and `LtcChaseSource`. This ADR
  records the decision ahead of both so each lands against a documented
  contract instead of re-deriving it.
- **Relates to:** ADR-0006 (time authority lives in the Director — this
  ADR refines *how* that authority derives time), ADR-0003 (Director/
  Renderer split), ADR-0014 (editor/show thread split — the show thread
  pumps `Timeline::update`).

## Context

Today the timeline clock has exactly one input. `PlaybackTimeAuthority::
updateTiming()` samples `high_resolution_clock`, computes a wall-clock
delta, and the show thread pumps `Timeline::update(dt)` once per show
frame, advancing the `m_currentTime` atomic when Playing. One clock, one
path.

Two queued features both need to touch this clock, and they pull in
opposite directions:

- **Audio playback (#3)** wants the audio device to drive timing, so the
  video you see stays locked to the audio you hear.
- **LTC/MTC timecode ingest (#1)** wants an external timecode source to
  drive the playhead, so Entity follows a stage manager's console.

The naive framing — "add a priority list of clock sources, audio device
beats wall clock, LTC beats audio device" — is **wrong**, and the
broadcast / show-control industry has known it's wrong for decades. It
conflates two genuinely different things:

- **Rate / frequency reference** — *how fast time passes.* This is
  genlock (video) and word clock (audio): a pure frequency edge with no
  position information. Genlock literally replaces a device's internal
  crystal so frames "hit the same beat."
- **Position / transport reference** — *where in the show we are.* This
  is timecode (LTC/MTC). Timecode is a metadata identifier; it does
  **not** clock a device. A device that "chases" timecode reads
  *position* from it and free-runs its *rate* off a separate, stable
  reference.

Drift-free sync in a real facility requires **both, separately**: a
reliable timecode source for position *and* genlock/word clock for rate.
Collapsing them into one ranked list produces a clock that ticks off
LTC's bit jitter — which is exactly what jam-sync (below) exists to
avoid.

An audio *file* is never a clock. An audio *device* has a crystal, and
that crystal is a legitimate rate reference — one of several, not a
privileged master. This ADR draws the rate/position line so #1 and #3
build on a model that holds up.

## Decision

The timeline logic clock has **two independent axes.** They are
different questions answered by different sources, and code names them
separately.

```
            RATE axis                         POSITION axis
   "how much real time elapsed?"      "where should the playhead be?"
   ----------------------------       ------------------------------
   RateSource (ranked, best wins)     PositionSource
     GenlockRateSource   (future)       InternalTransport  (default)
     AudioRateSource     (#3)           LtcChaseSource     (future, #1)
     SystemRateSource    (always)
              |                                   |
              +---------------> PlaybackTimeAuthority <---------+
                                          |
                                   Timeline::m_currentTime
```

### 1. Rate axis — `RateSource`

A `RateSource` answers exactly one question: monotonically accumulated
real-time seconds. It is a frequency reference and nothing else — it
carries no position, no transport state.

`PlaybackTimeAuthority` selects the **highest-priority healthy** source
and derives `dt` by diffing successive readings *of that source*. On a
source switch it re-anchors (per-source last-reading bookkeeping) — only
`dt` matters, never the absolute origin.

| Priority | Source | Availability |
|---|---|---|
| 1 | `GenlockRateSource` | future — hardware genlock card (Pro) |
| 2 | `AudioRateSource` | when the audio output stream is running |
| 3 | `SystemRateSource` | always — the floor |

- **`SystemRateSource`** — `high_resolution_clock` / QPC. Always healthy.
  This is today's `updateTiming()` behavior, extracted behind the
  interface with no functional change.
- **`AudioRateSource`** — derived from the audio device's sample clock
  (`IAudioClock::GetPosition()` on WASAPI): consumed samples ÷ sample
  rate. Engaged whenever the audio output stream is running — including
  during silence, because the crystal ticks regardless of whether any
  clip is producing sound. **Health-aware:** on device underrun or a
  stalled position counter it reports `healthy() == false`, and the
  authority falls back to `SystemRateSource` for that interval. The
  projector keeps moving; the cost of an underrun is one audible click,
  never a frozen show (show-output-priority).
- **`GenlockRateSource`** — reserved for the Pro genlock-card driver.
  Listed so the priority order is fixed now; not implemented here.

**LTC is deliberately absent from this table.** Timecode is jittery and
positional; ticking a logic clock off it is the mistake this ADR exists
to prevent.

### 2. Position axis — `PositionSource`

A `PositionSource` establishes and disciplines *where the playhead is*.
It never supplies rate.

- **`InternalTransport`** (default) — play / pause / seek / cues /
  sections. Position is integrated from the active `RateSource` by
  `Timeline::update`. This is what `Timeline` already does today;
  the ADR only gives it a name.
- **`LtcChaseSource`** (future, #1) — chases incoming LTC/MTC.

### 3. Jam-sync — the bridge between the axes

When an external position reference (LTC) is engaged it does **not**
become the rate clock. It **jam-syncs** onto whatever `RateSource` is
active:

1. Read incoming timecode; on lock, **snap** the playhead once.
2. **Free-run** the playhead on the active `RateSource`.
3. Periodically compare playhead position against incoming timecode.
4. Re-discipline within a **freewheel** tolerance band; on excess drift,
   re-snap. Manual transport input is disabled while locked.

This is the standard broadcast jam-sync model. It is why the two axes
can coexist without one corrupting the other.

### 4. Scope split across the two roadmap cards

- **Audio playback (#3) builds the rate axis only:** `RateSource`,
  `SystemRateSource` (extraction), `AudioRateSource` (new), the selector.
  It must *name* the `PositionSource` seam but builds nothing on it.
- **LTC ingest (#1) builds the position axis:** `PositionSource`,
  `LtcChaseSource`, the jam-sync loop — on top of a rate axis already in
  place.

## Consequences

### Positive

- **Zero A/V drift without resampling artifacts.** Video frame selection
  and audio playback ride the *same* crystal (`AudioRateSource`). They
  cannot drift apart. On a commodity ±50 ppm crystal, a wall-clock-master
  design would accumulate ~180 ms of lip-sync error per hour — grossly
  broken over a show. This design eliminates it structurally; no
  varispeed, no sample drop/insert.
- **LTC drops in cleanly.** #1 writes an `LtcChaseSource` against a
  documented contract — no second invasive surgery on `m_currentTime`.
- **Graceful degradation.** Audio underrun demotes the rate source to
  `SystemRateSource`; time keeps advancing; the show never freezes.
- **Vocabulary stops lying.** "Clock" no longer means two things. Rate
  questions and position questions are separated in the type system.
- **Refines ADR-0006 cleanly.** Decision #4 ("time authority lives in
  the Director") is unchanged — this ADR specifies how that single
  authority derives its rate and position, which is cluster-compatible.
- **Extra stall robustness.** With `AudioRateSource` active, the logic
  clock rides a hardware crystal independent of editor *and* show CPU
  scheduling.

### Negative

- **One extraction refactor.** `PlaybackTimeAuthority::updateTiming()`
  becomes `SystemRateSource` behind the interface. Low risk, near-zero
  behavior change, but it touches the time authority.
- **Source-switch discontinuity handling.** Underrun mid-show switches
  rate sources; the per-source last-reading bookkeeping is an edge case
  that needs an explicit test.
- **The logic clock now depends on audio device health.** A device
  underrun has a (handled) timeline consequence, not just an audio
  glitch. Acceptable given the fallback, but it widens what "audio
  device failure" can affect.
- **Slightly more structure than a single-machine, no-audio engine
  needs today.** Justified because #1 and #3 both land within the phase;
  the seam pays for itself almost immediately.

### Future consideration — genlock vs. the audio crystal

If a genlock card is ever the rate source *and* audio output is active,
the audio device must be word-clocked to the same house reference
(facility wiring) or the audio engine must resample to the genlock rate
— otherwise audio and video drift again. Deferred: the genlock card is
E-future / Pro. Note this is the timeline *logic* clock; display
*scanout* genlock (ADR-0006's Quadro Sync path) is a separate lower
layer. A hardware genlock card can feed both.

## Alternatives considered

### Single ranked clock list, LTC included (rejected)

The first-draft framing: one `ClockSource` priority list, LTC > audio
device > system timer. Rejected because it puts a jittery *position*
reference in the same list as *rate* references and implies the logic
clock could tick off LTC. The industry jam-syncs precisely to avoid
that. LTC is not a rate source.

### Timeline stays wall-clock master; audio mixer drift-corrects (rejected)

Lowest blast radius — no change to the time authority. The audio mixer
continuously resamples each clip to chase `Timeline::getCurrentTime()`.
Rejected: wall clock and the audio crystal genuinely run at different
rates, so correction is *continuous*, not occasional — varispeed
(pitch wobble) or sample drop/insert (clicks), forever. Wrong foundation
for a server whose job is to stay locked across a multi-hour show.

### Audio device directly drives `m_currentTime`, ad-hoc wall-clock fallback (rejected)

Functionally the same end state as the rate axis, but without the clean
seam — the "fall back to wall clock when no audio" branch is hand-rolled
inside `updateTiming()`. Rejected: LTC would then need its own invasive
retrofit of the same function. Building the `RateSource` interface is
barely more work and absorbs LTC for free.

### Defer the abstraction; ship audio on QPC now (rejected)

Ship audio against today's wall-clock master, refactor later. Rejected:
QPC-master means audible, accumulating A/V drift from the first build,
and LTC (#1) is the very next roadmap card — the refactor would land
weeks later anyway. Do it once, now.

## See also

- [ADR-0006: Cluster-ready plumbing from day one](0006-cluster-ready-plumbing-day-one.md)
  — decision #4 (time authority in the Director) that this ADR refines.
- [ADR-0003: Director/Renderer split](0003-director-renderer-split.md)
- [ADR-0014: Editor/show thread split](0014-editor-show-thread-split.md)
  — the show thread pumps `Timeline::update`; `RateSource` is queried there.
- Roadmap [#3](https://github.com/duremovich/Entity/issues/3) (audio —
  rate axis) and [#1](https://github.com/duremovich/Entity/issues/1)
  (LTC — position axis).
