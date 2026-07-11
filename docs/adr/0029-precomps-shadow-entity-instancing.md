# ADR-0029: Precomps — nested timeline containers via shadow-entity instancing

- **Status:** Proposed
- **Date:** 2026-07-11
- **Context source:** Precomp feature planning session (three codebase
  exploration passes + design pass, 2026-07-11). Self-contained — no private
  planning files required.
- **Tracked by:** the Precomps epic on the issue tracker (Phases A–G as
  sub-issues; created together with this ADR).

## Context

Users need After Effects-style **precomps** (Watchout's analog is the
*composition* media type — not auxiliary timelines, which are parallel
control-cue tasks): a container holding its own timeline — tracks, clips,
per-layer effects — that drops onto the main timeline **as if it were a
normal clip**. The instance must support trim, Freeze/Loop/PingPong end
behavior, duplication, move, opacity/blend/z-order, per-instance effect
chains, and per-instance speed. Edits to the source precomp timeline must
propagate to every instance.

Architecture facts that constrain the design:

- There is exactly **one `Timeline`**, owned by Director
  (`include/entity/director/Director.hpp:86`); every playback system holds a
  non-owning pointer to it. No container/nesting concept exists anywhere in
  the codebase.
- Every downstream playback mechanism is keyed **per clip entity**: decode
  workers and steering (`src/systems/DecodeSystem.cpp:126`
  `view<Clip, FrameBuffer>`), R2D resource provisioning
  (`bus::ProvisionClipResources.entity`), video descriptor slots plus the
  #90 generation guards (`ClipCatalogEntry::descriptorSlot/-Generation`),
  PlaybackPresenter display state, FrameCache entries, WarmSet spans, and
  SeekSync readiness predicates.
- The clip time-mapping math (`timelineFrame → localFrame → sourceLocalFrame
  = floor(localFrame · clipFps/timelineFps) → Freeze/Loop/PingPong wrap`)
  exists in four lockstep copies:
  `PlaybackTimeAuthority::mapToMediaFrame` 2-arg
  (`src/director/PlaybackTimeAuthority.cpp:463`) and 3-arg (`:513`),
  `wrapSourceLocalFrame` (`src/director/CatalogClipMath.cpp:19`),
  `src/systems/DecodeSystem.cpp:347`, and `src/systems/AudioSystem.cpp:377`.
- The compositor is three-pass and kind-blind (ADR-0018/0019): any layer
  that produces a texture rides `ContentLayerSnapshot`
  (`include/entity/bus/Message.hpp:420`) with
  `sourceKind ∈ {Video, Compose}`; per-layer effects (PASS 1.5) and screen
  routing work identically for both kinds.
- The editor/show thread split (ADR-0014) requires all show-thread inputs to
  be baked into `bus::SceneSnapshot`; generative layers are the precedent
  for "editor authors it, snapshot carries it, show thread renders it into a
  compose target" (`GenerativeLayerSnapshot`, Message.hpp:273; PASS 1
  producer, `src/systems/CompositorSystem.cpp:78-100`; R2D slot ack drained
  at `src/core/Engine.cpp:5642`).

**The hard problem.** Two instances of the same precomp can be active at the
same instant (overlapping tracks, or a looped instance next to a plain one)
and map the outer playhead to **different inner frames simultaneously**.
Per-entity keying means one shared set of inner clip entities cannot serve
two frames at once.

## Decision 1 — Shadow-entity instancing (over shared-entity re-keying)

Alternatives considered:

- **A. Shadow entities (chosen).** The definition holds authored "master"
  layers, excluded from all playback systems. Each dropped instance
  materializes hidden per-instance clones ("shadows") of the definition's
  member clips — ordinary `Clip` entities that flow through decode, upload,
  present, and cache **unchanged**; only their frame input is substituted.
- **B. Shared entities, per-(instance,clip) keying.** One set of inner
  entities; re-key decode workers, FrameCache, descriptor slots, presenter
  state, and the editor/show wire by an (instance, clip) pair. Rejected:
  this rewrites the highest-risk keying surface in the codebase for a memory
  optimization that only matters when instances of the same definition
  overlap.
- **C. Forbid overlapping instances of one definition.** Rejected: breaks
  the core requirement ("loop or duplicate as if a normal clip").

Approach A makes two overlapping instances literally two independent clip
sets — **the exact case the engine already handles when a user duplicates a
clip today**, so decoder/cache cost is precedented, and the #90 stale-slot
generation guards, SeekSync readiness, and WarmSet logic apply without
modification.

## Decision 2 — Definitions live in a `PrecompLibrary` owning real `Timeline` objects

`PrecompDefinition { id (opaque uuid string), name, canvasWidth,
canvasHeight, frameRate, duration, version (uint64), unique_ptr<Timeline> }`
in an Engine-owned `PrecompLibrary` (modeled on the content-routing asset
library). Reusing `Timeline` (heap-owned; non-copyability irrelevant) gives
the authoring tab its tracks, clip-edit machinery, and an **edit playhead
for free** via the otherwise-inert transport half — Director's Timeline
remains the only transport that Engine/PTA/SectionScheduler/DecodeSystem/
AudioSystem ever read. Definition timelines are never handed to playback
systems. Sections/cues are root-timeline-only; the sections UI is hidden in
precomp tabs.

Master layer entities live in the shared registry on the definition
timeline's tracks, tagged `PrecompMember{definitionId}`. Instances reference
definitions by the stable `id` string (`name` is display-only), following
the persist-by-stable-name idiom (`targetScreenName` etc.).

**Nesting:** v1 is single-level — a definition may not contain a precomp
instance (validated at authoring time). The schema needs no change to relax
this later: a nested instance is just a master layer carrying
`PrecompInstance`; only the validation rule and a cycle check are future
work.

## Decision 3 — One pure mapping function; wrap primitive extracted, not copied

Phase A extracts the Freeze/Loop/PingPong wrap into
`include/entity/timeline/PlaybackWrap.hpp`:

```cpp
struct WrapResult { FrameNumber frame; bool reverse; };
WrapResult wrapLocalFrame(PlaybackMode mode, FrameNumber sourceLength,
                          FrameNumber localFrame);
```

and migrates the four lockstep copies to it (pure refactor guarded by the
existing golden-hash tests). The new instance-level mapping is **one** pure
function in `include/entity/timeline/PrecompMath.hpp`:

```cpp
struct PrecompInstanceParams {
    FrameNumber instanceStartFrame;  // outer frames
    FrameNumber instanceDuration;    // outer frames
    FrameNumber innerStartFrame;     // trim into definition (definition frames)
    FrameNumber definitionDuration;  // definition frames
    double      definitionFrameRate;
    double      speed;               // clamped [0.01, 100]; negative banned v1
    PlaybackMode playbackMode;       // instance-level Freeze/Loop/PingPong
};
struct PrecompFrameResult { bool active; FrameNumber innerFrame; bool pingPongReverse; };
PrecompFrameResult mapOuterToInnerFrame(const PrecompInstanceParams&,
                                        FrameNumber outerFrame,
                                        double outerTimelineFps);
```

Math: `active = outerFrame ∈ [start, start+duration)`;
`local = outerFrame − start`;
`playLen = definitionDuration − innerStartFrame`;
`sourceLocal = floor(local · (definitionFrameRate/outerFps) · speed)`;
wrap via `wrapLocalFrame(playbackMode, playLen, sourceLocal)`;
`innerFrame = innerStartFrame + wrapped`. Stateless (no accumulator ⇒ no
drift), matching the existing clip math's floor-of-product style. Edge
cases: `playLen ≤ 0` or `instanceDuration == 0` ⇒ inactive; Freeze past end
⇒ `innerStartFrame + playLen − 1`; PingPong reflection is frame-identical
to clip-level PingPong because it *is* the same primitive.

**Composition contract:** consumers evaluate member clips by running the
**unchanged** existing clip math (`isClipActiveAtFrame`,
`mapToMediaFrameFromCatalogEx`) with `timelineFrame = innerFrame` and
`timelineFrameRate = definitionFrameRate`. Double-wrap (instance Loop ×
member PingPong) is well-defined because both wraps are stateless functions
of absolute frame. Reverse (negative speed) is banned in v1 — it requires a
backward-seek decoder audit (listed follow-up).

## Decision 4 — Shadows are trackless, inner-coordinate clips; substitution happens at four named sites

A shadow is `{Layer, Clip, Transform, MediaLayer, VideoTexture, FrameBuffer,
PrecompShadow{instance, masterEntity, innerZOrder}}` (+ a deep `EffectChain`
copy when the master has one). Its `Clip` fields stay in **definition-local
coordinates**; it lives on **no track** (registry-only), so root-timeline UI
and selection can never see it. Consumers substitute the frame input:

- `PlaybackTimeAuthority::buildSceneSnapshot` clip walk
  (`PlaybackTimeAuthority.cpp:904`) — shadows included; their
  `ClipCatalogEntry` carries a new `precompInstance` field.
- `buildRenderFrame` (`PlaybackTimeAuthority.cpp:1326`) — computes
  `mapOuterToInnerFrame` once per active instance, then runs the unchanged
  catalog math at `innerFrame` for that instance's member entries.
- `DecodeSystem::update` (`DecodeSystem.cpp:126` walk) — shadow targets
  derived from the mapped inner frame.
- WarmSet — a shadow contributes the **instance's outer window** as its warm
  span (conservative and correct for looped instances).

**Normative exclusion table** (tags `PrecompMember` on masters,
`PrecompShadow` on shadows; enforce with `entt::exclude<>`):

| Site | Masters | Shadows |
|---|---|---|
| `SectionScheduler.cpp:224/330/350/387/446/531` phase seeding walks | exclude | exclude (no `ClipPlaybackPhase` ever) |
| `PlaybackTimeAuthority.cpp:726/904` bake walks | exclude | include (with `precompInstance` set) |
| `DecodeSystem.cpp:126` editor steer | exclude | include (inner-frame substitution) |
| `AudioSystem.cpp:187` + `Engine.cpp:791` audio walks | exclude | exclude (v1 video-only; shadows never get `AudioSource`) |
| `Engine.cpp:749/3118` project-load/refresh `view<Clip>` walks | exclude | audit each: include only where the walk is resource provisioning |
| `MediaBinWindow.cpp:545` media usage counts | count | exclude (double-count) |
| Root `TimelineWidget` / PropertyWindow selection | structurally excluded (masters live only on definition-timeline tracks; shadows are trackless) | same |
| GenerativeSystem / TextSystem / AnimationSystem ticks | excluded v1 (clips-only inside) | n/a |

Debug asserts back the table: a shadow acquiring `ClipPlaybackPhase` or
`AudioSource` is a programming error. Any **new** `view<Clip>` added to the
codebase must consult this table (the table is normative, not advisory).

## Decision 5 — The instance renders as a Compose content layer; new PASS 1.6 producer

The instance entity carries the standard content-layer set (`Layer{kind =
Precomp}` — new `Layer::Kind::Precomp = 4`, informational only per
ADR-0016 — plus `MediaLayer`, `Transform`, routing components, optional
`EffectChain`, `AnimatedProperties`) and a `PrecompInstance{definitionId,
innerStartFrame, speed, playbackMode, materializedVersion, renderTargetSlot,
renderTargetGeneration, canvasWidth, canvasHeight}` component.

Show side, per render frame:

- `buildRenderFrame` emits the instance as a `ContentLayerSnapshot{
  sourceKind = Compose, sourceSlot = instance RT}` at the "future content
  kinds plug in here" seam (`PlaybackTimeAuthority.cpp:1442`), exactly like
  the generative fold at `:1584` — so per-instance effect chains (kind-blind
  PASS 1.5), screen routing, opacity/blend/z-order, and section fade all
  work **unchanged**.
- Member (shadow) entries are emitted with a new
  `ContentLayerSnapshot::precompTarget` field set to the instance; **PASS 2
  skips them** (they must never reach a screen directly).
- A new **PASS 1.6 precomp producer** in `CompositorSystem` (structural
  clone of PASS 1): per active instance — ensure a compose target at
  **canvas size** (lazy `createComposeTarget` + R2D
  `PrecompRenderTargetAllocated` ack, mirroring the generative ack drained
  at `Engine.cpp:5642`; `resizeComposeTarget` in place when canvas dims
  change), `beginComposeTarget(slot)`, draw the instance's member layers in
  `innerZOrder` order using the same texture-resolve + `drawTexturedQuad`
  logic PASS 2 uses (member transforms are canvas-NDC), `endComposeTarget`.
  PASS 1.5 is split so member effect chains run before 1.6 and instance
  chains after it.
- `PrecompInstanceSnapshot` (new `SceneSnapshot::precompInstances` vector,
  modeled on `GenerativeLayerSnapshot`) carries the mapping params, canvas
  dims, RT slot/generation, routing/transform/opacity/blend/zOrder, and
  baked animation tracks. Carrying canvas dims on the snapshot fixes, for
  precomps, the hardcoded-1920×1080 gap noted at
  `CompositorSystem.cpp:418`.

**Compose-target budget:** the pool is `MAX_COMPOSE_TARGETS = 64`
(`include/entity/render/IRenderer.hpp:70`) with no release API. Each
materialized instance costs 1 slot (+2 if it has an effect chain). Realistic
existing load (~6 screens + ~10 generatives + ~10 effect layers × 2) ≈ 36
slots, leaving headroom for ≈ 8–12 simultaneously materialized instances.
Mitigations: editor-side soft-cap warning when projected demand ≥ 48; resync
**reuses** the instance's slot via resize instead of reallocating; a
`releaseComposeTarget` API is a filed follow-up, out of v1 scope.

## Decision 6 — Sections apply at the instance level only

The instance is a normal root-timeline layer: it windows via the standard
active check and takes the standard section-fade multiplier. When the outer
playhead parks at a break, `mapOuterToInnerFrame`'s output freezes, so all
inner content freezes — correct by construction. v1 explicitly does **not**
support Locked-style keep-cycling continuation for precomp content: no
`ClipPlaybackPhase` on instances or shadows; `SectionBehavior` on instances
is fixed to Normal, and the break-aligned Normal extension
(`endAlignsWithSectionBreak`) is not baked for instances. Follow-up path: a
phase anchor at the *instance* level feeding the same pure function.

## Decision 7 — v1 inner content is clips-only

Video/image clips only inside definitions. **Text** generative layers are
the first planned amendment (editor-side raster to a video-pool slot plus
kind-blind composite makes it architecturally cheap, but it needs
per-shadow `TextLayerState` duplication and TextSystem inclusion rules — a
clean, separately landable phase). Rejected inside precomps, each for one
structural reason: **Muncher** (per-instance stateful simulation is
incoherent when instances sit at different inner frames), **ObjectAnimation**
(targets stage objects — Screens/Props — which do not exist inside a
canvas), **Signal** (fires on the global bus; time-mapped/looped re-emission
semantics are undefined and dangerous mid-show).

## Decision 8 — Persistence: schema v29

- Factor the per-layer save/load dispatch (`ProjectSerializer.cpp:404-740`
  save, `:1746-2262` load) into helpers parameterized on the target
  `Timeline`, then reuse them for both the root timeline and each
  definition. Land the refactor before the format change, guarded by
  existing round-trip tests.
- New top-level `"precompDefinitions": [ { id, name, canvasWidth,
  canvasHeight, frameRate, duration, tracks: [...] } ]`, loaded **before**
  the timeline so instances resolve.
- Instance layer object: `"kind": "precomp"` with `definitionId`
  (+`definitionName` for diagnostics), `innerStartFrame`, `speed`,
  `playbackMode` (string enum per bus rules), plus the shared placement/
  MediaLayer/Transform/routing-by-name/EffectChain/AnimatedProperties
  fields.
- `PROJECT_VERSION` 28 → 29 with the standard additive changelog entry
  (`ProjectSerializer.hpp:175`).
- Shadows and `materializedVersion` are **never persisted** — rebuilt on
  load by the materializer. A missing definition disables the instance
  (logged, kept in the file) rather than dropping it.

## Decision 9 — Resync is versioned destroy/recreate; undo opts in

`PrecompInstancingSystem` (editor tick): for each instance whose
`materializedVersion != definition.version`, tear down its shadows through
the same destroy path clip-delete uses (releasing workers/slots via the
existing lifecycle), re-clone from the masters, fire the clip-created
callback so `ProvisionClipResources` re-provisions, stamp the version.
`PrecompLibrary::touch(definitionId)` bumps the version from (a) a
CommandDispatcher post-execute hook when the mutated entity carries
`PrecompMember`, (b) the TimelineWidget edit-commit callback when the bound
Timeline belongs to a definition, (c) definition-level edits (canvas,
duration, track ops). Touches are debounced to once per tick. v1 resync is
full destroy/recreate (no diffing — decoder reopen on source edit is
accepted and documented; diff resync is a follow-up). v1 rule: **a
definition cannot be deleted while instances reference it.**

Instance delete/undo opts **in** (unlike Signal layers): new
`Timeline::DeletedLayerKind::PrecompInstance` with ~5 scalar fields riding
the existing snapshot machinery (`Timeline.cpp:290` branch,
`Engine::materializeClipFromSnapshot` `Engine.cpp:5102` branch ending in the
shared routing/effects tail); shadows regenerate on restore exactly as
decoder/GPU state regenerates for clips today. Copy/paste/duplicate/split
ride the same machinery (split = two instances with adjusted
`innerStartFrame`).

## Edit UX (summary; detail lives in the epic's Phase G)

Tabs on the timeline panel: double-click an instance (or open from the
MediaBin's precomp section, drag payload `PRECOMP_DEF`) opens the
definition's Timeline in a tab (`TimelineWidget::setTimeline` exists,
`TimelineWidget.hpp:99`; re-point safety audit, fallback = one widget per
tab). While a precomp tab is active, the stage preview shows a hidden
**preview instance** whose outer→inner mapping is identity against the tab's
edit playhead, flowing through the standard snapshot → PASS 1.6 path so the
preview pixel path is the same code as the show path. De-scope fallback if
the gated compose-target read proves awkward: v1.0 ships tabs for editing
with preview via instances on the main timeline.

## Consequences

**Enables:** reusable motion-graphics building blocks; loopable/duplicable
composed content; per-instance effects/speed; source-edit propagation; a
future path to nesting depth, Text-in-precomp, and instance-level
continuation without schema changes.

**Costs / limitations (v1):**
- Memory/decoder duplication when instances of one definition overlap
  (precedented — identical to duplicated clips; watch the 2026-05-23
  cache-thrash failure mode in the Phase D perf pass).
- Compose-pool pressure: ~8–12 simultaneous instances guidance; soft-cap
  warning; no release API yet.
- Video-only inside precomps; clips-only content; no negative speed; no
  Locked/continuation semantics for precomp content; sections/cues are
  root-only; single-level nesting.
- Full destroy/recreate resync causes a decode-worker reopen on every
  definition edit (editor-time only; show output protected by #90
  generation guards).
- Snapshot grows by one `PrecompInstanceSnapshot` per active instance plus
  one catalog entry per shadow — measured in Phase D before acceptance.

## Alternatives considered

Covered inline: shared-entity re-keying (Decision 1B), overlap prohibition
(1C), a bespoke `PrecompTimeline` container instead of reusing `Timeline`
(Decision 2 — rejected: duplicates track/edit machinery the tab needs
anyway), a fifth copy of the wrap math (Decision 3 — rejected: the four-copy
lockstep is existing debt, not a pattern to extend), per-instance render of
shared entities via multi-target PASS 2 generalization (subsumed by 1B), and
allowing Text/Muncher/OA/Signal inside v1 definitions (Decision 7).

## References

- ADR-0003 (Director/Renderer split), ADR-0014 (editor/show thread split,
  compose-target gate protocol #75/#89), ADR-0016 (Layer kinds select by
  composition), ADR-0017/0018 (generative layers, content-layer
  unification — the render-path template), ADR-0019 (kind-blind per-layer
  effects), ADR-0021/0022 (content routing).
- Implementation phases A–H with per-phase tests: the Precomps epic
  (sub-issues filed alongside this ADR).
