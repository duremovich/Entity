# Section-Break Stutter + Crossfade Slowdown — Tracy Capture Playbook

Operator observation (2026-05-23):
1. **Stutter at the section break crossing** — visible glitch the moment
   the playhead hits a break, *before* GO. Most pronounced on 4K H.264.
2. **Crossfade slowdown** — playback pace drops on both projector
   output and the stage visualizer when two clips are visible
   simultaneously via section fade or overlap.

This doc is the procedure to capture the two scenarios in Tracy so we
can identify the dominant cost and propose a targeted fix.

---

## Prerequisites

- Editor built with `-DENTITY_ENABLE_TRACY=ON` (default, under `build/`).
- `Tracy.exe` 0.13.1 available — download from
  https://github.com/wolfpld/tracy/releases. Don't use a different
  version; the wire protocol changes between point releases.
- A project with the failing content open. **Use 4K H.264** for the
  stutter case — that's the codec the symptom reproduces on.

## What's new in this build

Three changes since the last capture you took:

- **Stdout logs gated.** `Seek: jump from X to Y` and `[DECODE PACE]`
  no longer print by default. Set the env var
  `ENTITY_DECODE_VERBOSE=1` before launching the editor to re-enable
  them for diagnostic sessions. They were sustained 0.5-2ms/line on
  Windows, distorting the capture's own timing.
- **New Tracy zones** on the section-break apply path:
  `SectionScheduler::handleBreakAt`, `seedContinuationAt`, `go`,
  `advanceContinuation`. Look for these in the editor-thread timeline
  next to the existing `SectionDetect` zone on the show thread.
- **New plot: `Cache-miss recoveries / tick`.** Counts how many times
  per editor tick the Fix 4 cache-miss recovery force-seeks fire. Goal
  under healthy load: 0/tick. Sustained ≥1/tick == LRU thrash.

---

## Scenario 1 — Section-break stutter

**Repro setup:**
1. Load a project with one 4K H.264 clip spanning a section break
   (clip plays continuously through the break — NOT a clip whose end
   aligns with the break, since that's the new extension path now).
2. Confirm `SectionBehavior` on the clip is whatever you were testing
   with when you saw the stutter (note it down).
3. Seek to ~3 seconds before the break.

**Capture:**
1. Launch `Tracy.exe`. Click Connect (waits for the editor).
2. Launch the editor.
3. Once both connect, press Play.
4. Let the playhead cross the break and park.
5. Wait ~2 seconds at the break.
6. Press Spacebar (Section GO).
7. Let playback continue ~3 seconds past the break.
8. Press Pause.
9. In Tracy.exe, File → Save As →
   `docs/perf/section-break-stutter-<YYYYMMDDhhmmss>.tracy`.

**What to look for in the capture:**

- Open Find Zone → search `SectionDetect` on the show thread timeline.
  This is the moment the break is detected. The frames immediately
  before and after are the stutter window.
- Look at the show-thread frame times across this window. A spike means
  the show thread missed its vsync — the stutter is in the render
  pipeline.
- Look at the editor-thread `handleBreakAt` zone (new). Its duration is
  the apply cost. If it's >1ms it's eating part of the editor budget.
- Look at the `Cache-miss recoveries / tick` plot across the break. A
  spike at the break crossing == LRU evicted frames the worker needs.
- Look at `Decode queue depth` plot. Rising at the break == decoder
  fell behind.

---

## Scenario 2 — Crossfade slowdown

**Repro setup:**
1. Two 4K H.264 clips with overlap on the timeline, OR a single section
   break with `fadeSeconds > 0` so the trailing clip's fade-out
   overlaps a leading clip's fade-in.
2. Confirm both clips' `SectionBehavior` (note it down).
3. Seek to ~3 seconds before the overlap.

**Capture:**
1. Launch `Tracy.exe`, click Connect.
2. Launch the editor.
3. Press Play.
4. Let playback run through the entire overlap.
5. Press Pause.
6. File → Save As →
   `docs/perf/crossfade-slowdown-<YYYYMMDDhhmmss>.tracy`.

**What to look for:**

- `Cache-miss recoveries / tick` during the overlap. If it sustains
  ≥2/tick, that's the LRU thrash — the two decoders are evicting each
  other's working sets and the recovery is re-seeking both. For 4K
  H.264 each force-seek is 50-200ms of decode work, which absolutely
  shows up as visible slowdown.
- `FrameCache hit rate %` during the overlap. <90% sustained == cache
  budget too small for the active working set.
- Decoder zone density. Two decoders both saturated == raw CPU
  bound. Mitigations there are HW decode (DXVA/NVDEC) and lower
  resolution proxies, not a code fix.

---

## What to send

Just the two `.tracy` files (or a `.zip` of them). I'll open them and
read the per-frame breakdown. If they're too big, the
`tracy-csvexport.exe` tool can shrink them — but raw is fine first.

If the editor crashes mid-capture, send the partial `.tracy` anyway —
Tracy writes incrementally and the last seconds are usually intact.

---

## Sanity check before capturing

Before you spend time on Tracy, build the new editor (it compiled
green at HEAD just now) and re-run the same scenario you were just
testing. The stdout-noise reduction alone might lessen the visible
symptoms — `Seek: jump` and `[DECODE PACE]` were firing dozens of
times per second under cache pressure, and Windows stdout serialization
is not free. If the stutter and slowdown are noticeably smaller, that's
useful data on its own (it means stdout cost was a contributor) and
the Tracy capture will be a cleaner picture of what's actually left.
