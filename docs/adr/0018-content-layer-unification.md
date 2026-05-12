# ADR-0018: Content-layer unification — single compositor path for clip + generative + future kinds

- **Status:** Accepted
- **Date:** 2026-05-12
- **Implemented by:** This commit. Introduces `bus::ContentLayerSnapshot`,
  a two-pass `CompositorSystem::update`, and per-generative-layer compose
  targets allocated via the existing R2D-ack pattern.
- **Amends:** ADR-0017 (Generative layers). 0017's "Decision 3" sketched
  a direct-compose draw inside the per-screen loop and its
  "Alternatives" section explicitly *rejected* the per-layer RT approach
  for V1. This ADR adopts the rejected alternative now that a second
  forcing function has appeared (UV-space transform parity with Clip
  layers; user-articulated content-layer principle).
- **Relates to:** ADR-0014 (editor/show thread split — the R2D ack
  pattern reused here), ADR-0016 (the Layer abstraction these archetypes
  share).

## Context

ADR-0016 introduced `Layer` as the shared archetype across timeline-
resident entity kinds. ADR-0017 added the first generative kind
(Muncher) but rendered it via a kind-specific path inside the per-
screen compositor loop: `drawMuncherPlayfield` issued ~340 colored-quad
draws directly into the screen's compose target, with no layer-level
transform applied.

Clip layers, in contrast, render through a uniform path: each clip
contributes a `(textureRef, transformMatrix, opacity, blendMode)` tuple
to the compositor, which calls `drawTexturedQuad` once per clip into
the screen's compose target. The clip's `Transform` component is in
**screen-NDC space** — i.e. the UV-space of the target compose target.

The asymmetry surfaced when a user asked for transform widgets on
generative layers and asked the architectural question explicitly:
"all layers need a transform, in UV space, not 3D space — same as
clips already do." That framing makes the right model visible:
**every content-producing layer is a `(texture, UV-space transform)`
pair**; the only thing that differs between kinds is *how the texture
gets produced*.

## Decision

Adopt a unified two-pass compositor model with three pieces:

### 1. One bus type for the unified compositor input — `bus::ContentLayerSnapshot`

`bus::Message.hpp` gains:

```cpp
struct ContentLayerSnapshot {
    enum class SourceKind : int { Video = 0, Compose = 1 };

    std::uint64_t entity;
    std::uint64_t targetScreen;
    std::array<float, 16> transformMatrix;
    float         opacity;
    float         sectionFadeMultiplier;
    int           blendMode;
    std::uint32_t zOrder;
    SourceKind    sourceKind;
    std::int32_t  sourceSlot;
    int           colorSpace;
    std::string   ocioColorSpace;
};
```

`RenderFrame::contentLayers` carries all active content layers,
sorted by `zOrder` ascending. The compositor's PASS 2 reads this list
**kind-blind** — no Kind enum dispatch in the per-entry loop. The only
discriminator is `sourceKind`, which tells the renderer which
descriptor pool the `sourceSlot` indexes into.

`ClipRenderState` and `GenerativeLayerSnapshot` continue to exist —
they carry kind-specific producer-side data (mediaFrame, playback
mode, ghost positions, Muncher pellet bits, etc.) that downstream
producers (`PlaybackPresenter`, Generative PASS 1) need. They no
longer drive the per-screen loop. Section fade for clips flows from
the still-existing per-clip path into `ContentLayerSnapshot::
sectionFadeMultiplier` on the show thread before PASS 2 runs.

### 2. PASS 1 — every generative layer renders into its own compose target

`CompositorSystem::update` runs two passes:

```
PASS 1 — Producers populate their own RTs:
    for each active GenerativeLayerSnapshot:
        slot = ensureGenerativeRenderTarget(gl)
        beginComposeTarget(slot)
        drawMuncherPlayfield(gl, opacity=1.0)   // layer-local NDC
        endComposeTarget()

PASS 2 — Unified compositor pass:
    for each visible Screen:
        beginComposeTarget(screen.slot)
        for each ContentLayerSnapshot targeting this screen
            (already sorted by zOrder):
            bind texture by sourceKind:
                Video   → getVideoTexture(sourceSlot)
                Compose → getComposeTargetTexture(sourceSlot)
            drawTexturedQuad(tex, transformMatrix,
                             opacity * sectionFadeMultiplier,
                             blendMode, colorSpace, ocio)
        endComposeTarget()
```

The kind-specific procedural draw (`drawMuncherPlayfield`) draws in
**layer-local NDC** into the layer's own RT — `glm::mat4(1.0f)` is
the background quad, cell positions use the same `quadXf` helper as
before. The UV-space layer transform is **not** applied inside PASS 1;
it travels separately on `ContentLayerSnapshot::transformMatrix` and
is applied by `drawTexturedQuad` in PASS 2.

Generative RT allocation follows the same R2D-ack pattern the show
thread already uses for screen compose targets (ADR-0014):
`CompositorSystem` allocates a slot via `IRenderer::createComposeTarget`,
caches it in `m_pendingGenerativeAllocations` (show-thread-local), and
posts a new `bus::GenerativeLayerRenderTargetAllocated` reply on R2D.
`Engine::drainRendererToDirector` writes the slot back into
`GenerativeLayer::renderTargetSlot` on the editor thread; the next
`buildSceneSnapshot` carries the confirmed slot on
`GenerativeLayerSnapshot::renderTargetSlot`.

Slot pool: generative layers and screens share the same
`MAX_COMPOSE_TARGETS = 32` pool (bumped from 8 in this commit so the
shared pool isn't the bottleneck for typical multi-screen + multi-
generative shows). Revisit when an installation pushes the combined
count past 32, or split into per-archetype pools at that point.

### 3. ECS contract — composition still selects, no Kind enum dispatch

The unification preserves ADR-0016's "compose-not-dispatch" rule.
The editor-side bake walks **two kind-specific views**:

```cpp
// In PlaybackTimeAuthority::buildSceneSnapshot
// (or in show-side buildRenderFrame from already-baked
// activeClips + generativeLayers)
view<Clip, VideoTexture, Transform, MediaLayer>
    → ContentLayerSnapshot{ sourceKind: Video,
                            sourceSlot: videoTex.descriptorSlot, ... }
view<Layer, GenerativeLayer, Transform, MediaLayer>  // active only
    → ContentLayerSnapshot{ sourceKind: Compose,
                            sourceSlot: gen.renderTargetSlot, ... }
```

The compositor's PASS 2 sees only `ContentLayerSnapshot`s. To add a
new content kind (NDI input plugin, image sequence, audio
visualizer):

1. Define the kind-specific component(s) (e.g. `NDIInputLayer`).
2. Producer fills in either an existing video-texture slot
   (NDI / codec providers via the existing VideoTexture path) or its
   own compose target (procedural kinds via the PASS 1 pattern).
3. The editor-side bake (or show-side from existing snapshots) emits
   one `ContentLayerSnapshot` per active instance.
4. **No changes to CompositorSystem PASS 2**, no changes to the
   renderer, no new shader code, no new PSO.

The unifying invariant: **a content layer is anything with a Layer +
Transform + MediaLayer + one kind-specific component that either
populates a video-texture slot or owns a compose target.**

## Consequences

**Positive**

- One compositor path. PASS 2 has zero kind awareness; adding a
  third / fourth / fifth content kind is purely additive in
  producer code and snapshot population.
- UV-space transform semantics are uniform across content kinds.
  Position / rotation / scale on a generative layer have identical
  meaning to position / rotation / scale on a clip layer.
- Better rotation aesthetics for the Muncher: rotating a single
  pre-rendered texture has uniform AA along the rotated edge.
  Direct-draw with the layer transform applied per-cell-quad would
  have per-cell AA seams.
- The PASS 1 producer path is the natural place to add an opacity
  envelope, post-processing, or output-recording per generative
  layer in the future. Currently a no-op (`opacity` is 1.0 inside
  PASS 1, applied as `opacity × sectionFadeMultiplier` in PASS 2),
  but the seam is there.
- Generative layers now use the same R2D-ack flow as screens for RT
  allocation. One pattern, two callsites.

**Negative**

- Two compose targets per generative layer present (one for the
  layer's own RT, one for the screen). `MAX_COMPOSE_TARGETS = 32`
  caps total active compose targets across screens + generative
  layers. Generous for current use cases (typical show: 1-4 screens
  + 1-5 generative layers). Raise the constant or split into
  per-archetype pools when an installation hits the wall.
- One extra full-RT clear + blit per generative layer per frame.
  At 1920×1080 × 60Hz this is microseconds; never the bottleneck
  on the hot path (~340 procedural draws inside PASS 1 already
  dominate). Sprite-atlas instancing for the procedural draws
  remains the next perf-improvement target (ADR-0017 § Alternatives).
- `GenerativeLayerSnapshot` gains `transformMatrix` and
  `renderTargetSlot` fields. Wire format break-tolerant: decoders
  default both fields when absent (identity matrix, slot -1), so an
  older payload still parses, just compositing the playfield at
  identity transform until the editor catches up.

**Neutral**

- `SceneSnapshot::contentLayers` is on the wire but currently unused
  (the show side builds `RenderFrame::contentLayers` directly from
  `activeClips` + `generativeLayers` so it can pick up the
  frame-dependent `sectionFadeMultiplier` for clips). Reserved for
  future producer kinds that don't have frame-dependent state and
  can be fully editor-baked.

## Alternatives considered

**Viewport-multiply direct draw.** Keep the V1 direct-compose path,
pre-multiply every cell-quad's matrix by the layer transform before
submission. Smaller change (~30 LOC). Rejected because it doesn't
unify the compositor — clips and generatives still go through
different paths, and adding a third content kind (NDI, image-sequence,
audio visualizer) would each invent its own draw path. The user's
question was explicitly about unification.

**Add `transformMatrix` to `GenerativeLayerSnapshot` and stop there.**
Half-measure — gets the user-visible transform behavior but leaves
the dual-path compositor in place. Same objection as above.

**`ContentLayer` marker component on the entity.** Every content-
layer-producing entity gets a `ContentLayer` component plus its
kind-specific component. The editor would walk
`view<ContentLayer, Transform, MediaLayer>` directly to bake the
snapshot. Rejected because the component is redundant — the
"is-a-content-layer" predicate is exactly "has Clip+VideoTexture OR
has GenerativeLayer," computable from existing components. Adding
the marker is a maintenance burden (every creation path needs to
remember to emplace it). Composition-by-view at the bake site is
cleaner.

**Unify the R2D ack as a single `RenderTargetAllocated` message.** A
single message type for both Screen and Generative-layer slot acks,
dispatching by the entity's components on the editor side. Rejected
because the existing `ScreenRenderTargetAllocated` is already on the
wire and renaming is a break for any external consumer; the cost of
a parallel `GenerativeLayerRenderTargetAllocated` is one struct +
one drain case. Consider unifying when a third RT-owning archetype
appears.

## See also

- [ADR-0017: Generative layers](0017-generative-layers.md) — defines
  the Muncher kind; this ADR replaces its "direct compose" rendering
  choice with the per-layer RT pattern that 0017's Alternatives
  section reserved for "when a feature actually needs it."
- [ADR-0016: Timeline Layer abstraction](0016-timeline-layer-abstraction.md)
  — the shared `Layer` component this unification builds on.
- [ADR-0014: Editor/show thread split](0014-editor-show-thread-split.md)
  — the R2D-ack pattern reused for generative-layer RT allocation.
