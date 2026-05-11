# Entity Archetypes

What combinations of components make a "Clip", a "Screen", a "Projector"?
That contract isn't enforced by the registry — EnTT will happily attach any
component to any entity — but the rest of the codebase assumes specific
shapes. This file is the inventory.

If you're adding a new archetype, document it here. If you're touching one,
keep this file in sync.

---

## Conventions used below

- **Required** components are assumed present by at least one system. The
  archetype doesn't function without them.
- **Optional** components add capability. The archetype works without them.
- **Invariant** is the runtime contract the systems rely on.
- **Created at** lists the canonical creation sites — usually one or two
  places.

---

## Layer — Generic timeline-resident entity (Phase 3, ADR-0016)

`Layer` is the component that says "this entity lives on a `TimelineTrack`."
Every timeline-resident archetype (Clip, ObjectAnimation, future Generative)
carries a `Layer` alongside its kind-specific data components. Single
contract — start frame, duration, owning track, label, color, Kind enum —
shared across kinds. See `include/entity/components/Layer.hpp`.

**Kinds shipped with the abstraction**:
- `Kind::Clip` — paired with the `Clip` archetype below
- `Kind::ObjectAnimation` — paired with `ObjectAnimationLayer` (commit 3.3,
  not yet present)
- `Kind::Generative` — reserved for future generative layers

**Phase 3 migration window**: for Clip-backed layer entities,
`Clip::startFrame` / `Clip::duration` remain the source of truth and the
`Layer` mirror is synced via `syncLayerFromClip()`. Promotion of those
fields onto `Layer` outright is deferred to a Phase 4 cleanup PR.

**Composition, not inheritance**: systems select by component
combination (`view<Layer, Clip>` vs. `view<Layer, ObjectAnimationLayer>`),
never by reading `Layer::kind` at runtime. The Kind enum exists for the
UI badge, serialization disambiguation, and human-readable logs.

**Created at**: commit 3.1 introduces the component. Retroattach onto
existing `Clip` entities lands in commit 3.2.

---

## Clip — Video / image source placed on the timeline

| Required | Optional |
|---|---|
| `Clip` | `MediaLayer` |
| `Transform` | `AnimatedProperties` |
| `ClipDecodeState` | `ClipPlaybackPhase` |
| `VideoTexture` | `FrameBuffer` (transitional tag) |

**Invariant**: `startFrame ≤ currentFrame < startFrame + duration` (in
timeline frames) → clip is *active* and will appear in `SceneSnapshot::activeClips`.

**Created at**:
- `src/timeline/Timeline.cpp` (drag-onto-timeline, split, duplicate)
- `src/project/ProjectSerializer.cpp` (project load)
- `src/core/Engine.cpp` (assorted command handlers and demo paths)

**Notes**:
- `Transform` is a separate component (not embedded in `Clip`) precisely
  because `CompositorSystem` iterates `view<Transform, MediaLayer, Clip>()`
  — keeping Transform separate keeps the dense iteration efficient.
- `MediaLayer` is technically optional but in practice every active clip
  has one (z-order + opacity + blend mode). Treat as required for new code.
- `AnimatedProperties` is attached only when the user adds at least one
  keyframe. Absent for static clips.
- `ClipPlaybackPhase` is allocated lazily by `SectionScheduler` at the
  first section break.
- `FrameBuffer` is currently a marker tag — `Clip` alone is sufficient
  in practice. Likely to disappear in a future cleanup.

---

## Screen — Projection surface (LED wall, projection target)

| Required | Optional |
|---|---|
| `Screen` | — |

**Invariant**: A Screen is a render-target source for an `OutputDisplay`.
`Screen.modelEntity` references a separate `Model` entity for geometry.
`Screen.visible == true` and `renderTargetValid == true` → eligible for
output routing.

**Created at**:
- `src/core/Engine.cpp` (Stage UI "Add Screen" command + script command)
- Test / project-load paths

**Notes**:
- Screens carry their own `position` / `rotation` / `scale` arrays *inside*
  the `Screen` struct rather than using a separate `Transform` component.
  Different convention from Clip — see "Positional state inconsistency"
  at the bottom.
- The compose target slot (`renderTargetSlot`) is assigned by
  `CompositorSystem` via the snapshot-ack roundtrip (see ADR-0014).

---

## Projector — Virtual camera that maps content onto ProjectionSurface screens

| Required | Optional |
|---|---|
| `Projector` | — |

**Invariant**: `enabled == true` AND `linkedOutput` references a live
`OutputDisplay` entity → projector renders to that output. With
`targetSurfaceCount == 0` it illuminates all `ProjectionSurface` screens;
non-zero restricts to the listed surface entities.

**Created at**:
- `src/render/OutputManager.cpp` and projector UI

**Notes**:
- Carries own positional state (position / rotation / fovDegrees /
  nearClip / farClip), same convention as Screen / Prop.
- `calibrationPoints` is an unbounded `std::vector` inside the component.
  Acceptable because there are few projectors (typically <10) and
  calibration points are bounded by user workflow (~16-50).
- Calibration math is in ADR-0011 — touch carefully.

---

## Prop — Stage geometry for editor pre-visualization only

| Required | Optional |
|---|---|
| `Prop` | — |

**Invariant**: Props are *editor-only*. They're explicitly excluded from
`SceneSnapshot::screens` and the show thread never sees them. Per ADR-0014,
if/when projector masking lands, it'll add a separate `propCatalog` rather
than fold props into screens.

**Created at**:
- `src/ui/StageWindow.cpp` (Add Prop command)

**Notes**:
- References a `Model` entity for shared geometry (one chair.obj, five
  placements).
- Same own-positional-state convention as Screen / Projector.

---

## OutputDisplay — Physical or virtual output (projector, monitor, NDI, preview)

| Required | Optional |
|---|---|
| `OutputDisplay` | — |

**Invariant**: `enabled == true` AND (`sourceProjector != null` OR
`sourceScreen != null`) → this output renders something.
`sourceProjector` takes priority over `sourceScreen` when both are set.

**Created at**:
- `src/render/OutputManager.cpp` on display enumeration
- Project load (persisted output assignments)

**Notes**:
- Big struct (~450 bytes) because it bundles display identification, OCIO
  state, calibration overlay state, and window state. Acceptable: there
  are tens of outputs at most, not thousands, and it isn't iterated in a
  per-frame hot path.
- Carries `CalibrationOverlay` sub-struct (runtime-only, not persisted).

---

## MappingSurface — Projection warp quadrilateral

| Required | Optional |
|---|---|
| `MappingSurface` | — |

**Invariant**: `visible == true` → drawn by `renderMappingSurfaces`. Its
`outputIndex` ties it to a specific `OutputDisplay` for raster routing.

**Created at**:
- `src/ui/MappingWindow.cpp`

**Notes**:
- Self-contained quad + soft edges + source-region UV state. Carries
  several geometric helpers (`translate`, `scale`, `containsPoint`,
  `findNearestCorner`) that touch only own fields — accepted under the
  components soft-rule. See `ECS_PRINCIPLES.md`.

---

## Model — Shared 3D mesh

| Required | Optional |
|---|---|
| `Model` | — |

**Invariant**: A Model entity exists per unique mesh asset. Multiple
Screen and Prop entities can reference the same Model via their
`modelEntity` field. The mesh data + GPU upload slots live on the Model.

**Created at**:
- Model-import paths in `src/render/`

---

## Camera — Editor viewport camera (not a scene-graph entity)

| Required | Optional |
|---|---|
| `Camera` | — |

**Invariant**: Single Camera entity per editor 3D viewport (currently
just one — the Stage window). Drives `Stage3DRenderer`'s view + projection
matrices.

**Created at**:
- `src/render/Stage3DRenderer.cpp` (single registry-emplace at startup)

**Notes**:
- Currently a single Camera entity in the editor. If we ever add multiple
  3D viewports (split view, etc.), we'd need to disambiguate via a tag
  or a Camera-active flag.
- Carries `MIN_PITCH` / `MAX_PITCH` / `MIN_DISTANCE` / `MAX_DISTANCE`
  constants for orbit clamping — those are camera-system policy, not
  per-entity data, but kept on the component for locality.

---

## FeedMapping — Advanced clip-to-screen routing (defined but minimally used)

| Required | Optional |
|---|---|
| `FeedMapping` | — |

**Invariant**: Defined in `Screen.hpp`. Allows clips to be mapped to
screens with custom UV transforms (Direct / Perspective / Cylindrical /
Spherical / Custom). Currently the default `Direct` mapping is what
real projects use; the other modes are scaffolded for future feature
work.

---

## Positional state inconsistency (known)

Two patterns exist for how entities encode position:

1. **Separate `Transform` component**: Used by `Clip`. Reason: enables
   `view<Transform, MediaLayer, Clip>()` packed iteration in
   `CompositorSystem`.

2. **Embedded position/rotation/scale arrays**: Used by `Screen`,
   `Projector`, `Prop`. Reason: these aren't iterated in the per-frame
   compositor view; only one of each is touched per frame at most.

Both are defensible. We're not unifying because the cost (move ~6
fields per archetype, audit ~50 call sites) doesn't buy a property
that matters today. If a future feature requires iterating Screens or
Props at high frequency, revisit.

---

## See also

- `docs/reference/ECS_PRINCIPLES.md` — the rules these archetypes follow
- `docs/reference/SYSTEM_ORDERING.md` — which systems read/write these archetypes per frame
- `docs/adr/0014-editor-show-thread-split.md` — why some archetypes are excluded from the show-thread snapshot
- `include/entity/components/CLAUDE.md` — operator-facing rule sheet
