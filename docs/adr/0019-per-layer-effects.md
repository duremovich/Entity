# ADR-0019: Per-layer effects — ordered shader chain with stack + graph editors over one data model

- **Status:** Accepted (amended 2026-07-17 — see "Amendment
  2026-07-17" at the bottom: Phases 4 + 6 shipped, plus generators,
  combiners, DAG evaluation, and a scratch-pool executor replacing the
  ping-pong + R2D-ack RT scheme)
- **Date:** 2026-05-12
- **Implemented by:** Issue #54, commits `85bc6ac` … `44676c3`
  (Phases 1, 2, 3, 5); amendment implemented on branch
  `effects-node-graph` (2026-07-17).
- **Relates to:** ADR-0014 (editor/show thread split — the snapshot
  bake + R2D ack patterns reused here), ADR-0018 (content-layer
  unification — PASS 1.5 slots between PASS 1 generative and PASS 2
  composite without breaking kind-blindness), CODE_ISSUES NEW-07
  (animation-snapshot-bake pattern this builds on).

## Context

Every modern compositing tool — broadcast switchers, real-time VJs,
NLE colour pipelines, video-mapping servers — exposes a per-layer
"effect stack": an ordered chain of GPU passes that transforms a
layer's source texture before it lands on the final composite. Entity
shipped through Phase D without one. Adding one was a Phase D feature
ask, with the user-stated requirement that "the effect stack on a
layer can also be viewed and edited in a node graph editor."

Two architectural questions had to be settled before code landed:

1. **Where do effects live in ECS?** Per-layer vector? Separate
   entities? Component on the layer that points at child entities?
2. **How do effects animate?** The existing `AnimatedProperties`
   keyframe machinery is closed-enum (10 fixed `AnimatableProperty`
   values for clip transforms + OA 3D axes); effect parameters are
   open-ended (Blur radius + direction, ColorCorrect shadows.rgb +
   mids.rgb + highlights.rgb + saturation + hue, …).

The user's framing — same data model behind stack and node-graph
views — drives the answer to (1): the data must be graph-shaped (DAG
of nodes + typed sockets + connections) from day one, with the linear
stack as a degenerate case. The node-graph editor itself can ship in a
later phase, but the underlying data layout has to be ready for it
when it does.

## Decision

Adopt **effects-as-entities** with a per-layer `EffectChain`
component, PASS 1.5 in the show-side compositor, an editor-thread-owned
`EffectKindRegistry` (engine effects + future user packs), and a
snapshot-bake / show-thread-re-read flow that respects the
editor/show split from ADR-0014.

### 1. ECS data model — effects-as-entities

Each effect is its own entity. The owning layer (Clip, Generative,
future content kinds — see ADR-0018) carries an `EffectChain`
component that references effect entities in declaration order and
holds the explicit graph topology (empty in v1; populated by the
node-graph editor in Phase 4).

Components, all POD per `docs/reference/ECS_PRINCIPLES.md`:

| Component | Lives on | Carries |
|---|---|---|
| `EffectChain` | layer entity | `vector<entt::entity> nodes`, `vector<EffectConnection> connections`, `entt::entity outputNode` |
| `Effect` | each effect entity | `uint32_t kindId` (FNV-1a hash), `bool enabled`, `float graphX/Y` |
| `EffectParameters` | each effect entity | `vector<ParamValue> values` (positional per the kind's schema) |
| `EffectAnimatedParameters` | each effect entity (optional) | `vector<NamedTrack>` — paramName-hash-keyed keyframe tracks |
| `EffectChainRenderTargets` | layer entity (lazy) | ping-pong slot mirror written by R2D ack |

**Why not vector-of-effects-on-layer:**

```cpp
// Considered, rejected:
struct EffectChain { std::vector<EffectInstance> effects; };
struct EffectInstance { uint32_t kindId; vector<ParamValue> values; ... };
```

The variant-of-types problem inside `EffectInstance` would break
ECS principles (no virtuals, POD layout, snapshot-bakeable). Effects-
as-entities sidesteps that — each effect's parameters live in their
own components, animatable via the existing keyframe infrastructure
on a sibling component.

**Why a separate `EffectAnimatedParameters` component instead of
extending `AnimatedProperties`:** the existing enum-keyed `tracks`
vector is hot-path code for clip transforms (every active clip
hits it per snapshot bake). Generalising it to hash-keyed tracks
would break every `switch` in `AnimationSystem` and the bake path.
A parallel name-keyed component preserves the fast-path Transform
animation while giving effects an extensible param store. Both
components can coexist on the same effect entity (and do, when
created from the UI).

### 2. PASS 1.5 in the show-side compositor

`CompositorSystem::update` becomes a three-pass loop:

| Pass | Owns | Reads | Writes |
|---|---|---|---|
| 1 | Generative producers (PASS 1 in ADR-0018) | `GenerativeLayerSnapshot` | per-layer compose target |
| 1.5 | Per-layer effect chains (this ADR) | `ContentLayerSnapshot.effects` + ping-pong slots | post-effects compose target |
| 2 | Unified composite (PASS 2 in ADR-0018) | `ContentLayerSnapshot.{source,postEffects}Slot` | per-screen compose target |

PASS 1.5 walks each `ContentLayerSnapshot` with non-empty `effects`,
ensures the two ping-pong RTs are allocated (same `createComposeTarget`
+ R2D-ack pattern as PASS 1's per-generative-layer RT alloc — see
ADR-0018), and runs one fullscreen-quad pass per enabled effect:

```cpp
for each effect in cl.effects (enabled only):
    beginComposeTarget(currentOutputSlot)
    drawEffectPass(currentInput, effect.kindIdHash, effect.paramBlob, ...)
    endComposeTarget()
    currentInput = compose(currentOutputSlot)
    swap(currentOutputSlot, nextOutputSlot)
cl.postEffectsSlot = currentInput.slot
```

PASS 2 prefers `postEffectsSlot` over `sourceSlot` when set — one
added branch, no kind-dispatch change. The kind-blindness of PASS 2
(ADR-0018) is preserved: PASS 1.5 always writes a Compose-kind slot
regardless of whether the original source was Video (a clip's video
texture) or Compose (a generative's PASS 1 output).

### 3. EffectKindRegistry

`include/entity/effects/EffectKindRegistry.hpp`. Engine owns one;
pointer propagated to both editor-side bake (`PlaybackTimeAuthority`)
and show-side renderer (`D3D12Renderer::setEffectKindRegistry`). Two
kinds of effects:

- **Engine-shipped** (v1: 9 effects across Color + Stylize): static
  HLSL files in `shaders/effects/`, offline-compiled to `.cso` by
  CMake, registered in `EffectKindRegistry::registerBuiltins` with
  fixed `ParamSchema`. Hash = `fnv1a32("core.gaussian_blur")` etc.
- **User-authored** (Phase 6, deferred): project-local
  `<project>/effects/*.hlsl` + sibling `.json` manifests, compiled
  in-process via `RuntimeShaderCompiler` (the existing OCIO HLSL
  splicer extends to take effect manifests). Hot-reload via
  `ContentScanner` filesystem watcher.

The registry is editor-thread-owned. Show-thread access is read-only
(immutable after `registerBuiltins`; user-effect mutation is
editor-only).

### 4. Snapshot bake + show-side fold-in

Editor-side `PlaybackTimeAuthority::buildSceneSnapshot` walks
`view<EffectChain>` per snapshot, builds one `LayerEffectsSnapshot`
per layer (entity, ping-pong slot mirrors, baked effects list).
Per-effect bake:

1. Look up the kind via `m_effectKindRegistry->find(fx.kindId)`
2. Marshal `EffectParameters.values` into a 256-byte cbuffer-layout
   `paramBlob` (one vec4 slot per ParamSchema entry, Float → `.x`,
   Vec2 → `.xy`, etc. — see `shaders/effects/_effect_common.hlsli`)
3. Evaluate any `EffectAnimatedParameters` tracks at the editor's
   clip-local current frame and overwrite the corresponding slot.
   Also copy the track into `BakedEffectTrack` (paramName-keyed, per
   the wire-stability rule) for show-side re-evaluation during
   editor stalls.

Show-side `buildRenderFrame` linear-scans `scene.layerEffects` by
entity and folds the result into the matching `ContentLayerSnapshot`:
copies the `effects` vector + the `slotA` / `slotB` slot mirrors
into per-snapshot fields. PASS 1.5 reads everything off `cl` —
no side-table lookup in the compositor's hot path.

`MAX_COMPOSE_TARGETS` bumped 32 → 64 in `IRenderer.hpp` and
`DescriptorHeapLayout.hpp` to leave headroom for ping-pong RTs
alongside screens and generative outputs.

### 5. Wire-stable parameter identity, undoable commands

Commands route through `CommandDispatcher` (Editor affinity):
`AddEffectCommand`, `RemoveEffectCommand`, `SetEffectEnabledCommand`,
`SetEffectFloatParamCommand`. The float-param command keys on
parameter *name* (string), not slot index, so renaming params in an
engine effect schema doesn't silently shift values in old recorded
scripts. Slot index is resolved at execute-time via the registry.

Only `Float` params are editable in v1 (matches the engine effect
catalog — every shipped kind uses Float only). Vec2 / Color / Bool /
Int params and their respective commands arrive when first needed by
a real effect.

### 6. Project serialization (schema v16)

Each clip layer's JSON gains an `"effects"` array. Per-effect entries
carry `kindIdHash` + enabled + graphX/Y + a flat `params` float
array indexed by schema slot. Pre-v16 projects load with no effect
chain attached (forward-compat: missing key = empty). `outputNode`
and `connections` not serialized in v1 — linear stacks only until
Phase 4.

Round-trip tests: `ProjectSerializer.EffectChainRoundTrip` and
`ProjectSerializer.V15ProjectLoadsWithEmptyEffectChain` in
`tests/unit/ProjectSerializerTests.cpp`.

## Consequences

### Good

- **One data model behind two UIs.** PropertyWindow renders the chain
  as a linear stack today; the node-graph editor in Phase 4 will
  render the same components as a DAG with sockets. Switching views
  doesn't migrate data.
- **Effects integrate cleanly with the editor/show split.** PASS 1.5
  reads only the snapshot — zero registry reads on the show thread.
  Animation continues during editor stalls via the same
  `applyBakedAnimation`-style pattern that ADR-0014 / NEW-07
  established for clip animation. (Phase 2 only does
  editor-thread evaluation; show-side re-eval lands in a later phase
  but the bake plumbing is ready.)
- **PASS 2 stays kind-blind.** Effects are an interception point
  before PASS 2, not a new kind handled by it. The unified composite
  path from ADR-0018 keeps working unchanged.
- **9 engine effects.** Brightness/Contrast, Gaussian Blur, Hue/
  Saturation, Vignette, Pixelate, Sharpen, Chromatic Aberration, Edge
  (Sobel), Invert. Enough to demonstrate the system and to validate
  the schema across category dimensions (Color + Stylize).

### Trade-offs / open

- **Effect animation freezes during editor stalls.** Phase 2 bakes
  param values at editor's currentFrame; the show thread reads them
  verbatim. When the editor stalls (Win32 modal, project load), the
  paramBlob freezes. The track data already travels through to the
  show thread (`BakedEffectTrack` carries the keyframes), but show-
  side re-evaluation isn't wired yet. **Follow-up needed** mirroring
  the NEW-07 clip-animation fix.
- **Color management through effects is unsolved.** PASS 2's OCIO
  input-transform happens *after* effects in the chain. Effects on
  video clips operate in whatever colour space the decoder produced
  (sRGB, YCoCg, …), then PASS 2 applies the OCIO transform to the
  effect output. Linear-light–only effects (Blur, Sharpen) read
  mixed-space data and produce visually-wrong results on non-linear
  inputs. **Open question** to resolve before promoting to a v2.
- **Param marshalling lossy when EffectKindRegistry missing.** If the
  registry pointer isn't set on the renderer (e.g. headless tests
  pre-Engine-init), `drawEffectPass` logs + skips. Existing tests
  don't exercise effects so this is benign, but future scripted tests
  that create effects must run after `Engine::initialize`.
- **`MAX_COMPOSE_TARGETS = 64` is still a soft cap.** Per-kind pools
  (screens / generatives / effects) are the proper fix when an
  installation pushes past the shared limit. Not blocking for v1.
- **Project files store FNV-1a hashes, not stable IDs.** The hash is
  build-stable for engine effects, but cross-build hash collisions
  (extremely unlikely) would silently misbind. User-authored effects
  in Phase 6 require storing the stableId string alongside.

## Alternatives considered

### Vector-of-effects-on-layer (rejected)

Already covered above — the variant-of-types problem inside the
per-entry struct breaks ECS principles. Effects-as-entities lets
each kind's parameters live in their own components.

### Generalize `AnimatedProperties` instead of adding `EffectAnimatedParameters` (rejected)

Would force every existing switch in `AnimationSystem` and
`PlaybackTimeAuthority::buildSceneSnapshot` to grow a string/hash
arm. Hot path cost too high to justify; existing 10-enum machinery
stays as a fast tuple-key store, effects get their own
hash-key store.

### Bake `LayerEffectsSnapshot` directly into ContentLayerSnapshot (replaced original side-table approach)

The earlier design carried `LayerEffectsSnapshot` as a side-table on
SceneSnapshot. Show-side `buildRenderFrame` did a linear-scan lookup
per content layer. Worked but added an indirection layer — the
compositor needed `rf.layerEffects` which didn't exist on
RenderFrame, only on SceneSnapshot.

Resolved by folding the slot data directly into
`ContentLayerSnapshot.{effects,effectChainSlotA,effectChainSlotB}` at
buildRenderFrame time. PASS 1.5 reads everything off `cl`. The
`SceneSnapshot.layerEffects` side-table still exists as the
editor→show carrier, but the compositor no longer touches it.

### Effects as `IEffectProvider` plugins (deferred to a future ADR)

`plugin-api/` already houses Apache-2.0 plugin interfaces (per ADR-
0005). An `IEffectProvider` hook would let binary plugins register
additional kinds against `EffectKindRegistry`. Out of scope for v1
— engine effects are GPL core, user-authored HLSL packs are
project-local files. A follow-up ADR adds the plugin boundary when
a third-party effect pack appears.

### Per-screen post-FX (deferred)

A second effects location — after PASS 2 composite, before the
output swap chain — would model "output color grade" effects that
the user applies to the assembled scene rather than to individual
layers. Different snapshot shape (per-screen, not per-layer),
different UX (lives on the OutputDisplay or Screen, not Clip), no
ping-pong RT needed (single pass against the screen's compose
target). Defer until the per-layer plumbing has settled.

## Implementation pointers

- **Components:** `include/entity/components/Effect*.hpp`
- **Registry:** `include/entity/effects/EffectKind{,Registry}.hpp` +
  `src/effects/EffectKindRegistry.cpp`
- **Renderer path:** `D3D12Renderer::drawEffectPass`,
  `createEffectRootSignature`, `createEffectConstantBufferRing`,
  `getOrBuildEffectPso` in `src/render/D3D12Renderer.cpp`
- **Compositor:** `CompositorSystem::update` PASS 1.5 block + the new
  `ensureEffectPingPongTargets` helper in
  `src/systems/CompositorSystem.cpp`
- **Bake:** `PlaybackTimeAuthority::buildSceneSnapshot` end-of-function
  effect chain bake in `src/director/PlaybackTimeAuthority.cpp`
- **UI:** `renderEffectsSection` in `src/ui/PropertyWindow.cpp`
- **Commands:** Four classes at the bottom of
  `include/entity/command/Commands.hpp` + impls
- **Serialization:** `serializeEffectChain` /
  `deserializeEffectChain` next to `serializeAnimatedProperties` in
  `src/project/ProjectSerializer.cpp`
- **Shaders:** `shaders/effects/` (9 PS files + 1 shared VS + 1
  shared header)

## Amendment 2026-07-17 — graph evaluation, generators, combiners, scratch-pool executor

The deferred phases shipped, plus the "replace the layer" half of the
AE model. Summary of what changed relative to the original decision;
the rest of this ADR remains accurate.

### 1. DAG evaluation is real (Phase 4 complete)

One shared resolver — `effects::buildEffectExecutionPlan`
(`include/entity/effects/EffectChainTopo.hpp`) — turns an
`EffectChain` into an enabled-only, topologically ordered execution
plan. Linear stacks are the degenerate case of the same code path.
Kahn's sort runs editor-side at every bake, uncached (chains are tiny;
caching would buy invalidation bugs). Disabled nodes are
bypass-rewired: consumers re-point through runs of disabled nodes to
the first enabled producer. Cycles and graphs needing more than
`kMaxEffectGraphLiveIntermediates` (4) simultaneously-live
intermediates degrade to the linear plan with a throttled log — the
emitted plan is always valid; cycle *rejection* is the connect
command's job, the bake fallback is defense-in-depth.

Wire: `EffectSnapshot` gained `inputs` (per texture-input-socket
producer index; -1 = layer source, -2 = unconnected) and `inputCount`;
`LayerEffectsSnapshot`/`ContentLayerSnapshot` gained
`outputIndex`/`effectsOutputIndex`. The bake emits effects pre-sorted,
so the show thread executes a straight-line plan with zero graph
logic. Legacy payloads without `inputs` synthesize prev-feeds-next.

The same resolver orders the PropertyWindow stack view when a chain is
graph-driven, so the stack and the engine can never disagree about
evaluation order. `ConnectEffect` / `DisconnectEffect` /
`SetEffectGraphTopology` / `ReorderEffect` commands mutate topology
with whole-snapshot undo; the first graph gesture materializes the
implicit linear chain (`materializeLinearTopology`).

### 2. Ping-pong + `EffectChainRenderTargetAllocated` ack → show-local scratch pool

The original per-layer ping-pong RT pair with an R2D ack round-trip is
gone. PASS 1.5 now acquires intermediates from a show-thread-local
scratch pool (`CompositorSystem::acquireEffectScratch`): exact-size
reuse first, then resize-a-free-slot, then a fresh compose target;
released at each step's last use (liveness), the final output held
until next frame's reset. This is ADR-0014-compliant — no registry
writes, and the editor never needed those slots (`postEffectsSlot`
already travels via show-side RenderFrame mutation). Linear chains
cost the same 2 RTs as before; the pool is shared across layers so
compose-pool pressure went *down*. `EffectChainRenderTargets` and the
ack message remain wire-inert for one version per bus rule 3.

Critical correctness detail discovered by pixel-probing:
intra-frame chain reads must sample the compose target's **write sub**
(`TextureRef::composeWrite`), not the stable sub — the stable sub is
one frame old, and a scratch slot reused within a frame would hand a
consumer a different node's output entirely. This also retired the
old ping-pong path's one-frame-per-chain-step latency on animated
content.

### 3. Generators and combiners (the AE model)

A kind's role derives from its sockets (`EffectKind::
textureInputCount()` — no separate flag to drift): 0 = generator,
1 = filter, 2+ = combiner. Five `core.gen.*` generators (linear
gradient, checkerboard, fractal noise, plasma, shape) and three
`core.comb.*` combiners (blend, mask, displace) shipped. Generator-led
chains run with no source texture at all; a new `SolidLayerState`
generative kind (ADR-0018 recipe, one `drawColoredQuad` in PASS 1) is
the classic hosting surface. The effect root signature grew to four
single-descriptor SRV tables (t0–t3) + CBV; every register always
binds a valid descriptor — unconnected/absent inputs get a 4x4
TRANSPARENT-black fallback texture (blend weight 0 = passthrough).
`_effect_common.hlsli` gained `g_input1..3`, `g_timeSeconds`
(renderer-local aesthetic clock — deliberately not the timeline clock,
ADR-0025), and the reminder that Int/Enum slots are `asint()` bit-cast.

### 4. Resolved open questions

- **Effect animation during editor stalls** — closed.
  `BakedEffectTrack::slotIndex` is baked where name→slot resolution
  happens; `buildRenderFrame` re-evaluates animated Float params at
  the live frame and patches the per-frame paramBlob copy,
  registry-free (NEW-07 shape).
- **Project files store hashes, not stable IDs** — closed. v29
  serialization writes `stableId` (hash retained as fallback), typed
  name-keyed params, `animatedParams` keyframe tracks (fixing silent
  keyframe loss on save/load), and graph topology as effects-array
  indices. Generative layers persist chains too (was clip-only).
- **Phase 6 user HLSL + hot reload** — closed. ContentScanner watches
  `<project>/effects` (changed files re-emit Added, unlike media);
  `EffectKindRegistry::hotReload()` re-scans; a new
  `InvalidateEffectPso` D2R message evicts stale PSOs into a
  frame-aged graveyard. User bytecode is `shared_ptr<const vector>`
  behind a mutex — fixing the pre-existing editor-write/show-read
  race on `m_userArtifacts`.
- **RT sizing** — effect RTs and generative compose targets now size
  from source content dims (`ContentLayerSnapshot::sourceWidth/
  Height`; clamp 16..4096), so blur radii and pixelate cells are
  texel-true to the source.

Still open: color management through effects (PASS 1.5 runs before the
PASS 2 OCIO transform — unchanged from the original trade-off), and
`Backend::HLSLCompute` (declared, unimplemented; particles are the
motivating follow-up).
