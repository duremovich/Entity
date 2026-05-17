# ADR-0022: Content Routing library + Feed Maps

- **Status:** Accepted
- **Date:** 2026-05-17
- **Implemented by:** L1 (`3ba7be0` — asset/ref split, RoutingLibrarySystem,
  v20 migration, PropertyWindow rewire), L2 (`229183f` — library browser
  + Tiled authoring), L3 (`af1c5f6` — Feed Map kind + SVG template
  export), L4 minimal (`f30ae12` — selection context strip).
- **Relates to:** ADR-0021 (the two-tier mapping baseline — this ADR
  extends Plane A from inline to library-asset form), ADR-0014 (editor
  is sole registry writer; the library lives in the registry; show
  thread sees only the baked routes).

## Context

ADR-0021 shipped Plane A content routing as a `ContentRouting`
component emplaced inline on each `Clip` / `GenerativeLayer` entity. M2
covered Direct mode and a one-screen target. M3 added Tiled mode (one
source carved across N screens with per-screen UV crops) plus a
dedicated authoring window. Both shipped.

Three subsequent needs surfaced in tandem:

1. **Reuse.** The inline-per-layer model has no story for "the same
   routing applies to N clips." Productions routinely have 20 clips
   all destined for the same screen, or 8 clips all using the same
   Tiled split across an LED wall. Authoring the same `targets` array
   8 times is mechanical and error-prone, and there's nothing to bulk-
   edit when the screen layout changes.

2. **Auto-generated per-Screen direct routings.** The industry
   convention for video-mapping software is that every Screen
   automatically has a "direct" routing — pick a clip, pick a screen,
   done. The pre-ADR-0022 inline model conflated "pick the auto
   routing for Screen A" with "author a custom inline routing whose
   single target happens to be Screen A." The UX read identically;
   the data was needlessly bespoke per clip.

3. **Feed Maps.** A common authoring workflow for LED-wall content
   delivers a single wide source canvas with **named regions** ("Left
   Wall," "Stage Right," "Floor"), each region pre-laid-out to land
   on a specific physical screen. The content creator needs a template
   document describing the canvas layout; the operator needs an
   authoring surface that captures region names alongside the per-
   region UV rectangle. Tiled mode handles evenly-sliced cases but
   has no concept of named regions or a printable template, and
   conflating it with the named-region workflow would muddy the
   simple "carve evenly into N" case.

ADR-0021's Alternatives section explicitly deferred a "named, reusable
routings" entity to a future ADR. This is that ADR.

## Decision

Promote routing data from an **inline component** to a **library of
named asset entities** that layers reference by ID. Add a third
authoring kind, **Feed Map**, alongside Direct and Tiled, with
authoring affordances for named regions and a printable SVG template.

### Data shape

`ContentRoutingAsset` is a standalone library entity (no `Layer`, no
`Transform`):

```cpp
enum class RouteMode : uint8_t {
    Direct  = 0,   // identity uvRect, single screen
    Tiled   = 1,   // multi-target, derived uvRects (count + axis)
    FeedMap = 2,   // multi-target, explicit uvRect + region name
};

struct RouteTarget {
    entt::entity screen{entt::null};
    std::array<float, 4> uvRect{0, 0, 1, 1};
    std::string name;        // FeedMap regions; empty for Direct/Tiled
};

struct ContentRoutingAsset {
    std::string name;
    RouteMode kind{RouteMode::Direct};
    std::vector<RouteTarget> targets;
    uint8_t tiledCount{1};   // Tiled wizard
    uint8_t tiledAxis{0};
    uint32_t sourceWidth{1920};   // FeedMap canvas
    uint32_t sourceHeight{1080};
    entt::entity autoBoundScreen{entt::null};   // lifecycle marker
    std::string lastSyncedScreenName;            // autosync bookkeeping
};
```

`ContentRoutingRef` is the layer-side pointer:

```cpp
struct ContentRoutingRef {
    entt::entity asset{entt::null};   // null == "render on all visible"
};
```

The legacy `Clip::targetScreen` / `GenerativeLayer::targetScreen`
fields stay readable for one version (matches ADR-0021's existing
backward-compat window) and are removed in v21.

### Auto-direct lifecycle

`RoutingLibrarySystem` runs on the editor thread once per tick and
reconciles the library against the live Screen set:

- Every `Screen` gets a `ContentRoutingAsset` with `kind = Direct`,
  one target pointing at itself with identity `uvRect`,
  `autoBoundScreen` set to the Screen entity, and `name = Screen::name`.
- When a Screen is renamed, the system updates the asset's name iff
  the user hasn't manually diverged it (compare `name` to
  `lastSyncedScreenName`; diverged means autosync is broken).
- When a Screen is destroyed, the system cascade-deletes its bound
  auto-direct asset and clears any `ContentRoutingRef::asset` pointing
  at it.
- Manually deleting an auto-direct asset is benign — it gets
  regenerated on the next tick because the Screen still exists.

User-created assets (`autoBoundScreen == null`) are untouched by the
reconcile pass.

### Snapshot bake

`PlaybackTimeAuthority::buildSceneSnapshot` resolves
`ContentRoutingRef → ContentRoutingAsset` on the editor thread before
populating `bus::ContentLayerSnapshot.routes`. The bus wire format
from ADR-0021 is **unchanged** — the show thread has zero awareness of
the asset library. This is intentional and important:

- `entity-bus` rule 3 (additive-only wire) stays satisfied.
- Show-side rendering needs no Plane A logic beyond "for each route,
  draw the uvRect crop on this screen" — exactly what M3 already did.
- The library is editor-side only; future Phase E cluster work can
  ship the resolved routes per frame without ever sending the asset
  graph across the bus.

### Authoring surface

`PropertyWindow` relabels its dropdown from "Target Screen" to
"Content Routing" and sources items from the library: "Default (All
visible)" first, then auto-direct assets alphabetical by name, then
user-created assets alphabetical by name. A kind badge + "Edit..."
link below the dropdown focuses the dedicated window.

`ContentRoutingWindow` becomes a two-pane library browser. Left: list
+ "+ Add" popup with Direct / Tiled / Feed Map options. Right: detail
editor for the selected asset (name, kind, per-kind authoring extras,
targets table, schematic preview, selection-context strip).

### Feed Map workflow

The Feed Map kind keeps Tiled's wire shape (it's `targets[{screen,
uvRect, name}]`) but exposes:

- A source canvas size (`sourceWidth × sourceHeight`).
- Per-target name editing alongside screen + UV.
- An **Export Template** button that writes an SVG to
  `<cwd>/.feed-templates/<assetName>.svg`. The SVG carries one
  outlined `<rect>` per region with two `<text>` labels (region name +
  routed screen name). Content creators receive the SVG, author the
  matching source video, the operator applies the Feed Map to the
  delivered file, and the regions auto-route on play.

SVG was chosen over PNG: it's text, requires no font dependency to
emit labels, and round-trips through every browser / vector editor.
PNG export is straightforward to add later if a creator's pipeline
demands it.

## Consequences

### Enabled

- **Reuse.** A single "LED Wall — Tiled 4x" asset routes 20 clips at
  once. Renaming a Screen renames its auto-direct asset (so dropdowns
  update everywhere). Adjusting a Feed Map's region layout propagates
  to every clip using it.
- **Discoverability.** The Content Routing window now shows the
  library, not just the selected clip's routing. New operators see
  the full set of routings at a glance.
- **Template-then-author pipeline.** SVG export closes the loop with
  content creators: send template, receive matching content, apply
  routing, play.

### Forbidden

- **Wire-format growth.** The library doesn't cross the bus. If a
  future feature needs the asset graph on the show side (e.g. live
  re-routing from a clustered controller), that's a new ADR — not a
  silent wire-format change.
- **Free-floating routing data.** Every routing now has a name and
  a stable identity in the library; the inline-per-layer escape
  hatch is gone after the v20 migration window closes. This is the
  cost of reuse: every routing is shared infrastructure.

### Costs

- **Component soft-rule exception.** `ContentRoutingAsset` carries
  `std::string` and `std::vector` — heap allocations the
  `components/CLAUDE.md` soft rule discourages. Acceptable because
  library assets are never iterated in a per-frame view. Documented
  alongside the `Clip` / `Transform` exceptions.
- **Migration complexity.** v19 projects with inline `ContentRouting`
  components need translation: single-target Direct → auto-direct
  asset; multi-target → new "Custom Routing N" asset. Done in the
  serializer's clip-load pass; covered by the four existing
  `ContentRouting*` round-trip tests.
- **Direct registry mutations in the editor UI.** L2 / L3 edits
  bypass `CommandDispatcher` and write the registry directly. That's
  a temporary deviation from the standard "every UI mutation goes
  through an UndoableCommand" rule — undo / scripting support for
  library mutations is L2b polish. Acceptable because the layer-side
  binding (`ContentRoutingRef` on a Clip) still routes through the
  existing `SetClipTargetScreenCommand` for compatibility.
- **No poster-frame preview in v1.** The schematic + SVG export
  cover the verification need; sampling the selected clip's
  `VideoTexture` under the overlay would need a per-slot descriptor-
  handle accessor on `D3D12Renderer` plus careful show-thread
  synchronization. Deferred to a follow-up issue.

## Alternatives considered

### Keep routings inline; add a "duplicate routing from..." button

Solves reuse only at the cost of "snapshots that drift" — once you
duplicate, edits to the source routing don't propagate. Misses the
auto-direct + Feed Map workflows entirely. Rejected.

### Make `ContentRoutingAsset` a plain data type, stored in a flat
`std::vector` on a singleton "library" entity

Avoids the `std::string` soft-rule exception on a component. But it
gives up everything EnTT buys us — registry views over the library,
signal-based lifecycle, the natural fit with the editor/show
threading model. The "exception" cost is one component type with
strings; the savings are uniformity. Rejected.

### Three independent `RouteMode` values with compositor differences

Have `Tiled` derive uvRects at render time from `(count, axis)`
without persisting them; have `FeedMap` persist named regions
without an authoring wizard. Cleaner in theory; surprising in
practice — what shows up on screen depends on which mode you saved
in, even if the persisted target list looks identical. Rejected in
favor of "all kinds persist materialized routes; kind only changes
authoring affordances and the schematic legend."

### Embed Feed Map regions as a sidecar component instead of fields
on `ContentRoutingAsset`

ADR-0016 / ADR-0018's composition-over-Kind-enum principle would
argue for a separate `FeedMapMetadata` component carrying source
size + region names. The cost in this case (extra component, extra
view, separate serializer path) outweighs the purity benefit because
Feed Map regions are 1:1 with route targets — they're a per-target
field, not a parallel data structure. Rejected; `RouteTarget.name`
+ asset-level `sourceWidth/Height` are simpler.

## References

- Working plan: `~/.claude/plans/let-s-flesh-out-the-elegant-dawn.md`
  (the L1-L4 milestone breakdown that became this ADR).
- ADR-0021: the two-tier baseline this extends.
- ADR-0014: the threading invariant the library lives under
  (editor thread is sole writer; show thread reads baked
  `ContentLayerSnapshot.routes` only).
- ADR-0018: the unified PASS 2 compositor path the resolved routes
  flow into.
- Implementation commits: `3ba7be0`, `229183f`, `af1c5f6`, `f30ae12`.
