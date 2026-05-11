# ADR-0016: Timeline Layer abstraction — Layer component + ObjectAnimationLayer kind

- **Status:** Accepted
- **Date:** 2026-05-11
- **Context source:** Working plan
  `~/.claude/plans/a-few-notes-i-gentle-sunset.md`
  (Phase 3 layer abstraction, commits 3.1–3.8).
- **Implemented by:** Phase 3, eight commits:
  - 3.1 — `Layer` component introduced; `TimelineTrack` migrated from
    `clips` to `layers` (`std::vector<entt::entity>`).
  - 3.2 — `TimelineTrack::clips` renamed to `layers`; `Layer` retroattached
    onto existing Clip entities via `syncLayerFromClip`.
  - 3.3 — `ObjectAnimationLayer` + `ObjectAnimationOutput` components;
    `AnimationSystem` OA branch; `LayersWindow` UI.
  - 3.4 — `ObjectAnimationOutput` folded into `ScreenSnapshot` in
    `buildSceneSnapshot` (editor-side, Phase 3.4).
  - 3.5 — `LayersWindow` drag-to-timeline, interactive OA keyframe
    controls in `PropertyWindow`.
  - 3.6 — Project schema v15: `layers[]` replaces `clips[]` in
    serialized track data; `ProjectSerializer` round-trips OA layers.
  - 3.7 — `bus::ObjectAnimationLayerSnapshot` baked into
    `SceneSnapshot`; show thread re-evaluates OA tracks per render
    frame via `buildRenderFrame` (stall-safety mirror of NEW-07).
  - 3.8 — `SectionScheduler` OA freeze hook; `ObjectAnimationLayer::frozen`;
    `AnimationSystem` frozen-skip; ADR-0016 + doc updates (this ADR).
- **Amends:** ADR-0012 (Timeline sections and cues). ADR-0012 defines
  sections and the section-behavior contract for Clip entities; this ADR
  extends that contract to ObjectAnimation layer entities.

## Context

Before Phase 3, the timeline only contained `Clip` entities. Each
`TimelineTrack` held a `std::vector<entt::entity>` where every element
was assumed to carry a `Clip` component. This worked for a pure-media
timeline but blocked several planned features:

1. **Object animation.** Keyframed transforms for `Screen` and `Prop`
   entities need their own timeline placement — start frame, duration,
   owning track — but are not media clips. Stuffing OA data onto a Clip
   entity would conflate two unrelated concepts and leak OA-specific
   fields (target entity, 3D axis tracks) into the Clip hot path.

2. **Future layer kinds.** Generative layers, effects layers, and LTC
   sync markers are on the roadmap. Each kind needs timeline placement
   but not a `Clip`.

3. **Type-safe systems.** `AnimationSystem`, `buildSceneSnapshot`, and
   `buildRenderFrame` need to select by kind without reading a string
   tag at runtime. EnTT's component combination view is the right
   mechanism; it requires kind-specific components.

## Decision

Introduce a `Layer` component as the universal timeline-placement
contract. Every entity that lives on a `TimelineTrack` carries `Layer`
alongside its kind-specific data.

### `Layer` component (`include/entity/components/Layer.hpp`)

```cpp
struct Layer {
    enum class Kind { Clip, ObjectAnimation, Generative };
    FrameNumber   startFrame{0};
    FrameNumber   duration{0};
    int           trackIndex{0};
    Kind          kind{Kind::Clip};
    std::string   label;
    ImVec4        color{0.4f, 0.6f, 0.9f, 1.0f};
};
```

`startFrame` and `duration` mirror `Clip::startFrame` / `Clip::duration`
during the Phase 3 migration window. `Clip` fields remain the source of
truth for Clip entities; `Layer` fields are synced via `syncLayerFromClip`
at every edit site. Promoting those fields onto `Layer` outright is a
Phase 4 cleanup.

### `ObjectAnimationLayer` component (`include/entity/components/ObjectAnimationLayer.hpp`)

Marks a layer entity as a keyframed transform animation targeting a
`Screen` or `Prop` entity.

```cpp
struct ObjectAnimationLayer {
    entt::entity    target{entt::null};
    SectionBehavior sectionBehavior{SectionBehavior::Normal};
    bool            frozen{false};
};
```

`frozen` is set by `SectionScheduler::seedContinuationAt` when a
`Locked` OA layer is active at a section break. `AnimationSystem` skips
re-evaluation while `frozen == true`, preserving the last
`ObjectAnimationOutput` so the held screen position is stable during the
at-break pause. `clearAllContinuation` (called on GO and Stop) resets
`frozen` for all OA layers.

### Systems select by component combination, not `Layer::kind`

```cpp
// Clip-only path
registry.view<Clip, AnimatedProperties>()

// OA-only path
registry.view<AnimatedProperties, ObjectAnimationLayer, Layer>()
```

`Layer::kind` exists for the UI badge, serialization disambiguation, and
logs — not runtime branching in systems.

### Snapshot-bake for stall-safety (Phase 3.7)

`bus::ObjectAnimationLayerSnapshot` travels inside `bus::SceneSnapshot`
alongside `bus::ClipCatalogEntry`. The editor bakes active OA tracks in
`buildSceneSnapshot`; the show thread re-evaluates them per render frame
in `buildRenderFrame::applyOAAnimation`. This mirrors the NEW-07 clip
animation fix: during editor stalls the show thread re-evaluates stale
baked tracks at the advancing Timeline frame, so OA-driven screen
positions continue updating even when the editor is blocked.

### SectionBehavior for OA layers (Phase 3.8)

OA layers obey the same `SectionBehavior` enum as Clip entities:

- `Normal` — OA layer continues evaluating through the at-break pause.
  `AnimationSystem` re-evaluates tracks every tick. No freeze flag is
  set.
- `Locked` — OA layer freezes at the break. `seedContinuationAt` sets
  `ObjectAnimationLayer::frozen = true`; `AnimationSystem` skips the
  track re-evaluation loop, leaving `ObjectAnimationOutput` at its
  last-evaluated values. `clearAllContinuation` (GO / Stop) resets
  `frozen = false` so evaluation resumes.

## Consequences

**Enables:**
- Any number of layer kinds can be added without touching `Clip`,
  `TimelineTrack`, or the compositor hot path. Add the kind-specific
  component and register the kind in `Layer::Kind`.
- `AnimationSystem` cleanly handles both Clip keyframe animation (the
  first view) and OA layer animation (the second view) in a single
  `update()` pass with no shared mutable state between the two paths.
- OA layers participate in the section-break contract already defined
  by ADR-0012, without duplicating the SectionScheduler state machine.

**Forces:**
- Dual source of truth during Phase 3: `Clip::startFrame` /
  `Clip::duration` are the authority for Clip entities; `Layer::startFrame`
  / `Layer::duration` are the authority for OA and future kinds. Every
  Clip edit site must call `syncLayerFromClip`. This is tracked as a
  Phase 4 cleanup (consolidate onto `Layer`).
- Serialization must emit `"kind": "clip"` or `"kind": "object_animation"`
  in the `layers[]` array (schema v15+) so the loader can dispatch to the
  correct branch. An unknown kind is logged and skipped (not promoted to
  a Clip silently).

**Does not change:**
- The `bus::RenderFrame` / `CompositorSystem` hot path — the compositor
  still walks `rf.activeClips` and never sees OA entities directly.
- Show-thread threading rules (ADR-0014): OA components are written only
  on the editor thread; the show thread reads only `ObjectAnimationLayerSnapshot`.
