# ADR-0020: Object Animation layers — absolute values + Hold-by-default end behavior

- **Status:** Accepted
- **Date:** 2026-05-15
- **Implemented by:** Plan
  `~/.claude/plans/the-keyframes-for-object-swirling-axolotl.md`,
  commit landing this ADR.
- **Relates to:** ADR-0014 (editor/show thread split — Hold-mode
  snapshot bake reuses the same R2D / show-side re-eval patterns),
  ADR-0016 (Timeline Layer abstraction — defines the OA layer
  archetype), CODE_ISSUES NEW-07 (animation-snapshot-bake the show
  re-eval builds on).

## Context

Object Animation (OA) layers shipped in Phase 3 with the keyframe
authoring UI half-built: `renderOAKeyframeControls` rendered the
stopwatch + diamond + < > buttons, but `renderOALayerProperties`
never paired them with a value-edit widget the way Clip's
`renderTransformSection` does. Operators could add keyframes —
the keyframe captured `track->evaluate(frame)` which is `0.0f` on
an empty track — but had no way to set what value the keyframe held.
The reported symptom: "the keyframes for Object Animation don't seem
to do anything. There's no way to actually change position."

Wiring the missing value sliders forced answers to two semantic
questions that hadn't been settled in code:

1. **How do OA values relate to the target Screen/Prop's base
   transform?** The editor-side fold-in
   (`PlaybackTimeAuthority::buildSceneSnapshot`) replaces
   `ScreenSnapshot.position/rotation/scale` outright with
   `ObjectAnimationOutput` values whenever an OA layer is active.
   That's *absolute* override semantics — but it was never
   explicitly chosen vs. a *relative* (additive offset) model.

2. **What happens at the boundaries of the layer's active
   window?** Phase 3's `AnimationSystem::update` cleared
   `ObjectAnimationOutput` the frame the playhead left the active
   window, so the screen snapped back to its Stage-configured
   base position. For live shows this is the wrong default: you
   want the screen to *settle* wherever the animation parked it,
   not pop back when one cue ends and the next hasn't fired yet.

Disguise's analogous feature ("Animate Object" layers + Configuration
Presets) was the user's reference point. Two questions came out of
the comparison conversation:

- "What is the ground truth of the object's location?"
- "Does animate add/subtract from the screen's position, or set it
  absolutely? And what happens when we get to the end of a control
  layer?"

This ADR settles those questions for Entity. A separate future
epic — preset-based authoring layered *on top of* the same keyframe
data model — was explicitly deferred (see Alternatives below).

## Decision

### 1. OA values are absolute, not relative.

The `Screen` / `Prop` component's `position` / `rotation` / `scale`
fields define the **base / resting state** — where the target lives
when no OA layer is active.

While an OA layer is active and has set its has* flags, its
`ObjectAnimationOutput` values *replace*
`ScreenSnapshot.position[i]` / `rotation[i]` / `scale[i]` outright.
Same on the show-side re-eval path in `buildRenderFrame`.

Position adds cleanly; rotation and scale do not (they're
matrix-multiplicative — adding two rotations as scalar values is
mathematically wrong). Mixing modes per-channel would be confusing
to authors. We keep one rule for all three: absolute.

### 2. End-of-layer behavior defaults to Hold, with per-layer Reset opt-in.

Added: `ObjectAnimationLayer::EndBehavior { Hold = 0, Reset = 1 }`
with default `Hold`.

- **Hold** — when the playhead is past `startFrame + duration`,
  `ObjectAnimationOutput` retains its last evaluated values.
  `buildSceneSnapshot` continues folding them into the target's
  `ScreenSnapshot`. The target stays parked wherever the animation
  left it until another OA layer or operator action moves it. This
  is the show-friendly default — animation cues settle into their
  end state without a jarring snap.
- **Reset** — `ObjectAnimationOutput` is cleared the moment the
  playhead leaves the active window;
  `buildSceneSnapshot` falls through to the screen's
  Stage-configured base position. This is the pre-ADR-0020 behavior,
  kept available as an explicit per-layer opt-in via the End behavior
  combo in PropertyWindow.

Before the layer's start frame both modes reset (no last-evaluated
value exists to hold).

The Hold / Reset choice rides through the editor-side fold-in, the
`bus::ObjectAnimationLayerSnapshot` published on D2R, the show-side
re-eval in `buildRenderFrame`, and `.entity` project schema v17. The
loader defaults pre-v17 OA layers to Hold (the new show-friendly
behavior), not pre-ADR Reset — old projects deliberately upgrade.

### 3. Drag auto-creates a keyframe.

The PropertyWindow OA value sliders skip the "click stopwatch first"
gate that Clip's value sliders use. Because OA layers have no static
state — no own `Transform` — every value *is* a keyframe; dragging
the slider implicitly upserts a keyframe at the current layer-local
frame and lazy-emplaces `AnimatedProperties` / the track on first
edit. Drag-end commits one `UpsertKeyframeCommand` for undo.

Drag is disabled visually when the playhead is outside the layer's
active window (`getCurrentOAFrame` returns -1) — telegraphs why
editing has no effect there.

### 4. Precedence when multiple OA layers target one screen.

Both the editor-side fold-in and the show-side re-eval use the same
two-tier precedence:

1. Active layers (currentFrame ∈ [start, start+duration)) beat
   after-end-Hold layers.
2. Within each category, lower `trackIndex` wins.

Reset-mode layers never appear in the after-end consideration set —
they're filtered out editor-side. This matches the Phase 3
"lower trackIndex wins" policy unchanged for active layers.

## Consequences

**Enables**

- Authors can now actually change position/rotation/scale on OA
  layers — the value sliders write the keyframe values, the
  evaluation pipeline carries them through to the output.
- Animations settle naturally at the end of a layer instead of
  snapping back — live-show behavior matches what an operator
  expects ("go to that position, stay there until the next cue").
- `RotationZ` (Z-axis roll) is now a first-class channel alongside
  `RotationX` / `RotationY`. The legacy `Rotation` alias (Rotation→Y
  for OA back-compat with Phase 3 projects) stays mapped, so already-
  authored OA `Rotation` keyframes keep producing Y-rotation.

**Forces / forbids**

- Editor-side `AnimationSystem` is the sole writer of
  `ObjectAnimationOutput`. Hold mode is implemented by *not writing*
  during the after-end window, not by reading old state — keeps
  ADR-0014's editor-as-sole-writer invariant.
- Editor-side `buildSceneSnapshot` filters out after-end-Reset OA
  layers from `bus::SceneSnapshot::objectAnimationLayers` entirely;
  after-end-Hold layers ride the bus so the show thread can keep
  applying them during editor stalls. The show-side re-eval must
  honor the same precedence ladder as the editor-side fold-in;
  divergence here = output drift.
- Schema v17. Pre-v17 OA layers load with the new Hold default —
  this is a deliberate upgrade-to-new-behavior, not a back-compat
  preservation of pre-ADR Reset. Operators with legacy projects who
  want the snap-back back must flip the per-layer toggle.

**Costs**

- Two precedence ladders to keep in lockstep (editor-side fold-in +
  show-side re-eval). The shared invariant — *active beats
  after-end-Hold, lower trackIndex wins within either* — is
  documented inline at both sites and exercised by the
  `OAShowSideReevalTest.EndBehavior_*` and
  `OALayerTest.EndBehavior_*` unit tests.
- Hold mode means a buggy OA author can leave a screen visibly off
  its base position indefinitely. Mitigation: the operator can
  always flip End behavior to Reset for cleanup, or move the
  screen explicitly via Stage.

## Alternatives considered

**Relative (additive) OA values.** OA delta + base = effective
transform. Pros: no snap-back issue at end (delta=0 ⇒ base = 0
delta). Cons: rotation/scale don't add — matrix-multiplicative.
Mixed per-channel rules (position adds, rotation/scale don't) are
confusing to authors. Rejected on simplicity grounds.

**Reset-by-default with optional Hold.** Pre-ADR behavior with an
opt-in. Cons: defaults shape habits — most operators would never
discover the Hold opt-in, so the show-friendly behavior would be
the path-less-taken. Rejected; the show-friendly choice is the
right default for a media server.

**Configuration Presets (Disguise model) instead of direct
keyframes.** Named transform snapshots; OA layers keyframe transitions
between presets. Considered and deferred to Phase 2. Presets don't
change any of the semantic questions above — they're a UX
reusability layer over the same keyframe data model. Capturing a
preset is "save current Screen transform under a name"; applying it
at frame F is "drop N keyframes onto the OA layer at frame F." So:
implement the foundation now (direct keyframes work correctly,
absolute, Hold-by-default), layer presets on top later when
reusability becomes the dominant pain point. The data model is
forward-compatible — Phase 1 projects load fine in a Phase 2 editor
that adds presets.

**Auto-Reset on next cue / next break.** Reset triggered by a
SectionScheduler boundary rather than by frame-window exit. More
complex precedence: which layer ends "owns" which break? Rejected
in favor of simple per-layer choice. Cue-triggered resets can be
authored explicitly with a second OA layer whose only keyframe sets
the base transform.

## References

- Plan: `~/.claude/plans/the-keyframes-for-object-swirling-axolotl.md`
- ADR-0014: editor/show thread split (Hold-mode snapshot bake + show
  re-eval reuse the same patterns).
- ADR-0016: Timeline Layer abstraction (defines the OA layer
  archetype).
- ADR-0019: per-layer effects (animation-snapshot-bake reference —
  the show-side re-eval architecture this builds on).
- Disguise AnimateObjectPreset:
  `help.disguise.one/designer/layers/layer-types/previsualisation/animateobjectpreset`
  (reference for the deferred preset-system alternative).
