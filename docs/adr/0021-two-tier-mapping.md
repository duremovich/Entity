# ADR-0021: Two-tier mapping — Content Routing (Plane A) vs Feed Output (Plane B)

- **Status:** Accepted
- **Date:** 2026-05-16
- **Implemented by:** Phase M1 (this ADR + dead-code purge +
  `MappingSurface` → `OutputSurface` rename). Phases M2-M4 in the
  same mini-phase add the `ContentRouting` component, Tiled mode,
  authoring UI, and final window rename.
- **Relates to:** ADR-0014 (editor/show thread split — the
  snapshot-bake pattern `ContentRouting` flows through), ADR-0016
  (Layer abstraction), ADR-0018 (content-layer unification — the
  uniform PASS 2 path `ContentRouting` extends).

## Context

Entity's content-routing model up to this ADR consists of one
`entt::entity targetScreen` field on `Clip` and on `GenerativeLayer`.
Two values are legal: a specific Screen entity, or `entt::null`
meaning "render on every visible screen." That's it. There is no
support for the actual workflow modern video-mapping rigs need: one
piece of source content sliced across multiple screens with
per-screen UV crops — a panoramic video file driving three LED
panels, a stitched plate covering two stage-side walls, etc.

A second, structurally unrelated problem lives nearby. The
output-side primitives — `OutputDisplay` (physical raster + source
selection + input-region crop) and `MappingSurface` (corner-pin warp
quad) — handle "which screen feeds which hardware output, warped how"
correctly, but the C++ name `MappingSurface` and the UI window
`MappingWindow` both make this look like a *content* concept. The
existing dead `FeedMapping` struct in `Screen.hpp` doubles down on
the confusion: it carries content-mapping fields (UV offset, scale,
rotation) but borrows the word "feed" which in the standard
industry vocabulary (Disguise et al.) means hardware output, not
content. The struct has been instantiated zero times since it
landed.

The two problems share one root cause: the codebase doesn't draw a
clean line between **how content lands on screens** and **how screens
land on physical outputs.** Solving either in isolation perpetuates
the conflation. This ADR draws the line, fixes the vocabulary, and
commits to a structural extension of the content-routing side that
ships the multi-screen feature.

## Decision

Recognize two distinct planes in code, vocabulary, and UI:

```
Clip / GenerativeLayer
      |  Plane A — Content Routing
      |    ContentRouting{ mode, targets[{screen, uvRect}] }
      v
Screen compose target           (CompositorSystem PASS 2)
      |  Plane B — Feed Output
      |    OutputDisplay{ sourceScreen, inputRegion, surfaces[OutputSurface] }
      v
Physical output swap chain      (OutputManager::renderOutputs)
```

### 1. Vocabulary

| Concept | Code term | UI term |
|---|---|---|
| Plane A — content → screen routing | `ContentRouting` component, `RouteTarget`, `RouteMode` enum | "Content Routing" window/menu |
| Plane B — collective output side | `OutputDisplay`, `InputRegion`, `OutputSurface` (rename of `MappingSurface`) | "Outputs" window/menu |
| Per-output warp quad | `OutputSurface` | (surface row inside the Outputs window) |

The word **"feed"** is reserved for Plane B in documentation. Code
type names avoid "feed" entirely to dodge the vendor-vocabulary
overload (Disguise's "feed map" is Plane A; their "feed output" is
Plane B — the term is unhelpful as a discriminator). Per the global
"don't name competitors in commits / ADRs / code" rule, comparisons
to specific products are conversational only.

### 2. Plane A — Content Routing component (M2/M3)

A clip or generative layer carries one `ContentRouting` component:

```cpp
enum class RouteMode : uint8_t {
    Direct = 0,    // Single target screen, identity uvRect — current behavior
    Tiled  = 1,    // Multi-screen: list of routes, each with its own uvRect (M3)
    // Cylindrical, Spherical reserved for a future ADR (deferred)
};

struct RouteTarget {
    entt::entity screen{entt::null};
    std::array<float, 4> uvRect{0.0f, 0.0f, 1.0f, 1.0f};  // x, y, w, h in source UV
};

struct ContentRouting {
    RouteMode mode{RouteMode::Direct};
    std::vector<RouteTarget> targets;
    // empty `targets` == legacy null-targetScreen semantics ("render on all visible")
};
```

The component is bake-friendly (POD by the rules in
`ECS_PRINCIPLES.md`) and lands as a new field
`ContentLayerSnapshot::routes` on the bus (alongside an existing
`targetScreen` field retained as a deprecated alias for one
project-format version).

PASS 2 of `CompositorSystem` (ADR-0018) gains a route lookup in
place of the single-target filter. Single-target `Direct` mode is
the M2 endpoint with `routes.size() == 1` and identity `uvRect` —
behavior identical to today. Multi-target `Tiled` mode (M3) admits
`routes.size() > 1`, each route contributing its own
`drawTexturedQuad` with `uvRect` composed into the UV math. No new
PSO; no new shader variant. The clip's UV-space `Transform`
(ADR-0018) composes on top of each route's `uvRect`.

Cylindrical, Spherical, and Perspective modes are explicitly
deferred. They require new pixel-shader variants and sidecar
components (composition rule, ADR-0016). When a real installation
needs one, it gets its own ADR.

### 3. Plane B — Feed Output cleanup (M1, structurally unchanged)

The output-side data model is correct. M1 cleans up the names:

- **Delete `FeedMapping` + `FeedMappingType`** from
  `include/entity/components/Screen.hpp`. Zero references outside
  the declaration site; pure dead code carrying misleading
  vocabulary.
- **Rename `MappingSurface` → `OutputSurface`** for the C++ type
  and source file. The bus wire-string `"MappingSurfaceSnapshot"`
  is pinned per `include/entity/bus/CLAUDE.md` rule 3 (don't break
  serialized keys); only the in-tree C++ identifier changes.
- **Function names** (`drawMappingSurface`, `renderMappingSurfaces`)
  defer to M4 — function-name churn would cascade across the
  renderer surface without buying anything M2/M3 needs.
- **`MappingWindow` → `OutputsWindow`** also defers to M4 for the
  same reason; keeping its current name through the structural
  M2/M3 work avoids two UI-rename diffs.

Multi-source raster packing on `OutputDisplay` (one physical output
carrying screens A and B side-by-side in a single raster) is
deferred. The current `sourceScreen` / `sourceProjector` single-source
model covers the v1 use case. If a real installation needs raster
packing, that's a Plane B sidecar feature with its own ADR.

### 4. UI split (M3/M4)

Two windows along the Plane A / Plane B boundary:

- **`OutputsWindow`** (renamed from `MappingWindow` in M4): Plane B
  authoring. Physical-display assignment, source routing
  (`sourceScreen` / `sourceProjector`), input-region crop, per-output
  calibration / OCIO / brightness, and the corner-pin canvas where
  `OutputSurface` quads are warped.
- **`ContentRoutingWindow`** (new in M3): Plane A authoring. Per
  selected clip / generative layer, the mode dropdown
  (Direct | Tiled) and the routing table — rows of
  `{screen, uv x, uv y, uv w, uv h}`. Adding a row sends the same
  content to another screen with its own crop.

`PropertyWindow` keeps the single-screen dropdown for `Direct` mode
(the 90% case) and adds a "Multi-target…" button that opens the
routing window for clips with `mode == Tiled`.

### 5. Backward compatibility

- `Clip.targetScreen` and `GenerativeLayer.targetScreen` stay as
  deprecated alias fields for one project-format version. On load,
  the alias is migrated into `ContentRouting` and never read again
  at runtime. `ProjectSerializer.cpp` dual-writes both fields
  during M2/M3; M4 stops writing the alias. The reader fallback
  persists until M5.
- The bus wire-string for `MappingSurfaceSnapshot` is pinned. Future
  C++ identifier renames go through `Serialization.cpp`'s explicit
  wire-type table, not implicit RTTI.

## Consequences

### Positive

- **Vocabulary stops lying.** "Mapping" no longer straddles two
  unrelated concerns; `MappingSurface`'s name no longer hints at a
  content-routing role it never had. The dead `FeedMapping` struct
  stops poisoning code review.
- **One headline feature shipping (M3).** One piece of content
  carved across N screens with per-screen UV crops. The actual
  workflow the existing single-target model couldn't express.
- **Clean composition seam for curved modes.** `RouteMode` is the
  extension point. Cylindrical/Spherical bring a sidecar component
  + a new PS variant; the data layout doesn't change. No design
  rework when those land.
- **Snapshot bake pattern reused unchanged.** `ContentRouting`
  flows through `bus::SceneSnapshot` → `bus::RenderFrame::contentLayers`
  via the same editor-bake / show-read discipline ADR-0014 codified.
  No new threading work.
- **PASS 2 stays kind-blind (ADR-0018).** Routes apply to every
  `ContentLayerSnapshot` regardless of `sourceKind`; clip and
  generative layers gain Tiled support simultaneously.

### Negative

- **One project-format version of dual-write.** Saves carry both
  `targetScreen` (deprecated) and `contentRouting` (new) until M4.
  Files are slightly larger; readers tolerate either side; the
  alias drops once on-disk projects have all been re-saved.
- **PASS 2 gains a route lookup per content layer.** In Direct
  mode (single target, identity uvRect) the cost is one vector
  iteration of size 1 per layer per screen — measurable only in
  worst-case scenarios. Tiled mode adds one extra
  `drawTexturedQuad` per route per matching screen, only when
  authored. Capture a Tracy baseline after M3 against
  `stress_3layer_prores_1080p`; expect PASS 2 p95 within ±5%.
- **Two new commands** (`SetContentRoutingCommand` in M3, possibly
  finer-grained add/remove/edit-route commands later). The
  command set grows alongside the feature; acceptable.
- **One more window** in the editor menu after M3. Mitigated by
  the property-panel "Multi-target…" entry point — users only
  open `ContentRoutingWindow` when they actually need Tiled.

## Alternatives considered

### Mode-enum-only on Clip (Option C, rejected)

Add a `RouteMode` field directly on `Clip` and resolve per-screen
crops from supplementary fields keyed by mode. Wins for pure
Cylindrical/Spherical (single screen + projection params), loses
for Tiled (a list of arbitrary `{screen, uvRect}` pairs doesn't
fit a flat mode-keyed struct). The actual M3 feature is a list
of routes; cramming it into a mode-keyed sidecar is awkward.
`ContentRouting` collapses to the mode-enum case anyway when
Cylindrical/Spherical land (composition rule, ADR-0016).

### Reusable `ContentMap` entity referenced by clip (Option B, deferred)

A `ContentMap` is its own entity; clips and generative layers
reference it by ID. Reuse story: copy the routing from clip X to
clip Y by pointing both at the same map. Pros: real DRY when
authoring a show with twenty clips on the same LED-wall slicing.
Cons: adds an indirection layer for every layer in the system,
loses tight `clip.targetScreen` mental model, brings cross-entity
reference semantics the rest of the ECS doesn't have. Verdict:
pay this cost when a real "copy these routes" workflow appears.

### Keep `MappingSurface` name (rejected)

Leaves the dual-meaning vocabulary in place. Small short-term
savings; long-term confusion every time a new contributor reads
`MappingSurface` in `OutputDisplay::surfaces` and expects it to
mean content mapping. The rename is mechanical (29 files, no
behavior change) and ships in M1 alongside the dead-code purge.

### One-window UI with tabs (rejected)

Keep `MappingWindow` and add a "Content Routing" tab next to
"Outputs." Less menu churn. Loses the conceptual split visually
— users would still think of both as "Mapping." Two windows
forces the question "which plane am I editing?" before the user
touches anything.

### Multi-source raster packing in M3 (deferred)

`OutputDisplay` becomes `vector<{sourceScreen, rasterRect}>` so
one projector raster can show screens A and B side-by-side. Not
requested in the v1 ask. Defer until a real installation needs
it; the v1 single-source model isn't load-bearing for the
multi-screen content workflow this ADR enables.

## See also

- [ADR-0014: Editor/show thread split](0014-editor-show-thread-split.md)
  — the snapshot-bake pattern `ContentRouting` flows through.
- [ADR-0016: Timeline Layer abstraction](0016-timeline-layer-abstraction.md)
  — the composition-not-dispatch rule that gates how
  Cylindrical/Spherical attach later.
- [ADR-0018: Content-layer unification](0018-content-layer-unification.md)
  — the kind-blind PASS 2 path Tiled extends without modification.
- Working plan:
  `~/.claude/plans/i-think-we-need-shimmying-pond.md` —
  the M1-M4 sequencing this ADR opens.
