# AnimationSystem editor-stall fix — design

**Tracking issue**: `docs/reference/CODE_ISSUES.md` NEW-07.
**Status**: **Implemented 2026-05-11.** The actual implementation closely
follows the "Recommended approach: snapshot the tracks, evaluate on show
thread" section below, with one structural choice worth flagging: the
evaluator is duplicated as a small static helper inside
`src/director/PlaybackTimeAuthority.cpp` (anon namespace) rather than
shared with `KeyframeTrack::evaluate`. The duplication is intentional —
keeps the bus<->components boundary clean — but the two implementations
must stay in sync. Whenever you touch one, update the other.
**Why this doc exists**: a careless implementation here can freeze projector output, so we want the design pinned down before code lands. The user has called this out as a historical break-pattern. See the plan at `~/.claude/plans/i-d-like-to-do-stateless-island.md`.

---

## Problem

When the editor thread stalls (Win32 modal dialog, OS resize/move loop, slow project load), `Engine::update()` stops running. That means:

- `AnimationSystem::update()` stops ticking (it's invoked from editor `update`).
- `Transform` and `MediaLayer` registry components stop being refreshed with the current frame's evaluated keyframe values.
- `buildSceneSnapshot()` also stops running, so `ClipCatalogEntry::transformMatrix` / `opacity` stay frozen at whatever they were at stall start.
- The show thread keeps Present-ing frames at 60 Hz — but the *content* shows static, frozen animation.

`Timeline::m_currentTime` and `DecodeSystem::targetFrame` keep advancing because they have show-thread fallbacks (commits `cf103bd` and `8492438`). Animation does not, so users see decode progress while transforms / opacity stand still.

## Why the naive fix doesn't work

The instinct is "also tick `AnimationSystem` from `Engine::showThreadMain()` when the editor heartbeat goes stale," matching the pattern used for Timeline and DecodeSystem. **This violates ADR-0014.**

`AnimationSystem::update` writes registry components — `Transform.position`, `Transform.rotation`, `Transform.scale`, `Transform.dirty`, `MediaLayer.opacity`. Per ADR-0014, the editor thread is the sole writer of the EnTT registry; the show thread must only read. A direct port to the show thread races with editor writes that *could* still come through (e.g., the editor stall ends mid-frame).

The Timeline / DecodeSystem fallbacks work because they only write atomics outside the registry (`Timeline::m_currentTime`, `DecodeWorker::targetFrame`). `AnimationSystem` writes the registry. Different rule.

## The deeper observation

`ClipCatalogEntry` already carries the *result* of animation: `transformMatrix` is column-major, pre-baked from `Transform::updateMatrix()` during `buildSceneSnapshot`; `opacity` is already there. The problem is that those fields are baked on the **editor thread** at editor tick rate. If the editor thread stalls, those fields stop updating.

To keep animation alive during a stall, the show thread needs to be able to *re-evaluate* without writing to the registry and without depending on the editor's snapshot bake. That means the inputs to animation (the keyframe tracks themselves) and the input time (`Timeline::currentFrame`, already show-safe) must both be reachable from the show thread.

## Recommended approach: snapshot the tracks, evaluate on show thread

### Shape

1. **Bake `AnimatedProperties.tracks` into `ClipCatalogEntry`.** The current snapshot has the *evaluated result* (transformMatrix + opacity at the editor's current frame). Change it to also carry the underlying tracks. The tracks change rarely (only when the user adds/edits a keyframe — and during a stall, by definition no edits are happening), so snapshotting them once per editor frame is cheap.

2. **Promote `KeyframeTrack::evaluate(localFrame) -> float` and the per-property dispatch to a pure utility.** Lives next to `AnimationSystem` but is callable without touching the registry. Operating on snapshot data only, it's safe for any thread.

3. **Show thread evaluates per render frame.** In `Engine::buildRenderFrame()` (or a small new helper `applyAnimationToRenderFrame()`), iterate the snapshot's clipCatalog: for each clip with non-empty tracks, evaluate at the current `Timeline::getCurrentFrame()` (already show-safe), compose a transform matrix from the evaluated PositionX/Y/Rotation/ScaleX/Y values, and write the result into `ClipRenderState::transformMatrix` and `ClipRenderState::opacity`.

4. **Editor-side `AnimationSystem::update()` stays for now.** It keeps writing `Transform` and `MediaLayer` registry components so the PropertyWindow and other UI surfaces show live animated values. This is belt-and-suspenders during the transition; if it turns out the UI works fine reading from the snapshot/tracks directly, this can be deleted in a follow-up.

### Why this works during a stall

- `ClipCatalogEntry::tracks` is stale during the stall (no new snapshot bakes), but stale tracks are correct tracks because nothing edited them. The keyframe data still describes the right curve.
- `Timeline::getCurrentFrame()` keeps advancing because Timeline already has a show-thread fallback.
- Show-thread evaluation reads stale tracks + fresh frame → fresh animated transform / opacity per render frame.
- No registry writes anywhere on the show thread.

### What the new field looks like

In `include/entity/bus/Message.hpp`, inside `ClipCatalogEntry`:

```cpp
// AnimatedProperties snapshot (empty when clip has no animation).
// Show thread re-evaluates these per render frame at Timeline::getCurrentFrame()
// so animation stays alive during editor stalls.
struct BakedKeyframe {
    FrameNumber frame{0};
    float       value{0.0f};
    int         interpolation{0};  // KeyframeInterpolation enum as int
};
struct BakedTrack {
    int                          property{0};  // AnimatableProperty enum as int
    bool                         enabled{true};
    std::vector<BakedKeyframe>   keyframes;
};
std::vector<BakedTrack>          tracks;
```

Tracks travel through the bus, so they get an enum-to-int dance and a serialization update per the bus boundary rules in `include/entity/bus/CLAUDE.md`. `AnimatableProperty` and `KeyframeInterpolation` already exist as enums in `AnimatedProperties.hpp`; adapt them for wire transport the same way `BlendMode` is handled today.

### Files to touch when implemented

- `include/entity/bus/Message.hpp` — add `BakedTrack` / `BakedKeyframe` / `tracks` field to `ClipCatalogEntry`.
- `src/bus/Serialization.cpp` — encode/decode the new field (with the enum-as-string rule).
- `include/entity/systems/AnimationSystem.hpp` + `src/systems/AnimationSystem.cpp`:
  - Extract `evaluateTracks(const std::vector<BakedTrack>&, FrameNumber localFrame) -> EvaluatedAnimation { mat4 transformDelta; float opacity; bool hasOpacity; ... }` as a pure free function.
  - Keep the existing `update()` for editor-side belt-and-suspenders.
- `src/core/Engine.cpp` (`buildSceneSnapshot`) — bake `AnimatedProperties.tracks` into the new field.
- `src/core/Engine.cpp` (`buildRenderFrame` on show thread) OR a new helper called from there — apply evaluated animation to each `ClipRenderState`.
- `include/entity/systems/CLAUDE.md` — update the AnimationSystem entry to note the show-side evaluation path.
- `docs/reference/SYSTEM_ORDERING.md` — update the show-thread fallback coverage table (AnimationSystem ✓).
- `docs/reference/CODE_ISSUES.md` — close NEW-07.

## Alternative considered: split-evaluation + write-back

Have a `AnimationEvaluator` (read-only, show-safe) and a thin
`AnimationApplier` (editor-only, writes registry). The editor calls both;
the show thread calls only the evaluator and writes results into the
`ClipRenderState`.

Same end result, more moving parts. Recommend the snapshot-bake above
unless we discover the editor UI actually needs the registry writes to
go through `AnimationSystem::update()` specifically (PropertyWindow,
keyframe gizmos in TimelineWidget).

## Performance expectation

Track evaluation is a binary search per keyframe + cubic interpolation —
microseconds per clip. For 100 clips, well under 1 ms total per show
frame. Tracy should show a new `applyAnimationToRenderFrame` zone in
show-thread frames; budget it inside `buildRenderFrame`.

Memory: each `BakedTrack` is two pointers + a vector of small POD
keyframes. The full `tracks` array is bounded by user authoring effort —
projects with hundreds of clips × tens of keyframes are bytes, not
megabytes. No allocation pressure expected.

## Verification plan

When implemented, verify in this order:

1. **Smoke (does it still work?)**. Load `stress_animation_4layer`, play
   through. Compare visually against current behavior. No regression in
   normal playback.

2. **Tracy capture** (`scripts\perf\capture.ps1 stress_animation_4layer`)
   pre- and post-. Compare with `scripts\perf\compare.ps1`. Expect
   AnimationSystem editor zone to shrink (still ticks editor-side for
   UI), new applyAnimationToRenderFrame zone to appear in show thread,
   total per-frame budget unchanged or slightly higher (the show-side
   work is new).

3. **Editor-stall reproducer**. Start playback of an animated project.
   Grab the editor window's title bar and drag it for 3+ seconds. Watch
   the projector output (preview window or actual output). Animation
   should continue smoothly — no freeze, no skip when the stall ends.
   The fix is correct iff this works.

4. **Stop-and-resume**. Drag for 5s, release, drag again, release. The
   timeline frame at the end should match (5+5)s of wall-clock advance
   — Timeline's show-thread fallback has been doing this correctly for
   a while.

5. **Integration test** (per `CLAUDE.md` wall-clock guidance — use
   `WaitSeconds`, not `WaitFrames`). Add a test under `tests/integration/`:
   - Create a clip with one Opacity track keyframed `0 → 0` then jumping
     to `1` at some later frame.
   - Start playback.
   - Simulate editor heartbeat going stale (set `m_lastEditorTickNs` to
     a value > 50ms in the past).
   - Wait 0.5s.
   - Assert that the `ClipRenderState::opacity` reflects the post-jump
     value, not the pre-jump value. (Currently it would still be
     pre-jump because AnimationSystem hasn't ticked.)
   - This also covers the NEW-09 gap (no regression test for the
     fallback pattern).

6. **`ctest -j 1`**. Per project memory — parallel ctest is flaky on
   this project. Expect 430/430 passing (current baseline).

## Rollback shape

The bus message-format additions can't be easily rolled back once
deployed — they become part of the wire format. So implement the
serialization in one commit (additive, default values mean old clients
ignore the field) and the show-thread evaluation in a second commit. If
the show-thread evaluation causes a regression, revert just the second
commit; the bus field stays harmlessly empty.

If something *deeper* breaks — say show-thread evaluation introduces a
race we hadn't anticipated — the editor-side registry writes from
`AnimationSystem::update()` are still happening, so visually the editor
UI still works. Output animation just regresses to the pre-fix behavior
(frozen during stalls). That's the safety net the "belt-and-suspenders"
keep-editor-AnimationSystem-running decision buys us.

## What not to do

- Don't tick `AnimationSystem::update()` from `Engine::showThreadMain()`
  without first removing all of its registry writes. That's the
  ADR-0014 violation. The whole point of this redesign is to make show-
  thread participation possible *without* the writes.
- Don't try to share state between editor and show via a shared mutable
  per-clip cache. The bus snapshot mechanism is the contract; new state
  goes there, not in side-channel maps. (See
  `feedback_show_thread_priority.md` in user memory and the
  CompositorSystem `m_pendingAllocations` comment for the pattern.)
- Don't bundle this with the SectionScheduler fix (NEW-08) in one PR.
  They have similar shapes but distinct enough that mixing them makes
  debugging harder if either side regresses.

## See also

- `docs/adr/0014-editor-show-thread-split.md` — threading rules these
  designs work within.
- `docs/reference/CODE_ISSUES.md` NEW-07 — the open issue.
- `docs/reference/SYSTEM_ORDERING.md` — show-thread fallback coverage
  table this fix closes one gap in.
- `include/entity/bus/CLAUDE.md` — bus boundary rules for adding new
  fields to wire types.
- `docs/design/section-scheduler-snapshot-bake.md` — sibling design for
  NEW-08 (same shape, different system).
