# ECS / Data-Oriented Design Principles

Entity follows **Entity Component System** (ECS) + **Data-Oriented Design**
(DOD). This file is the canonical statement of *why* and *how*. The shorter
operator-facing rule sheets live in `include/entity/components/CLAUDE.md` and
`include/entity/systems/CLAUDE.md`; this file is the longer reference they
both point back to.

If anything in this file disagrees with current code, **the code wins** —
file an issue and update this doc.

---

## The model

- **Entities** are just IDs (`entt::entity`). They carry no data and no
  behavior. An entity is the *junction* where a set of components meets.
- **Components** are pure data structs attached to entities. They describe
  *state*, never *behavior*.
- **Systems** are classes that read and write components in bulk via
  `registry.view<Components...>()`. They contain all the behavior.

The pattern that falls out: any time you're about to add a method to a
component, ask "does this belong in a system instead?" Default answer is
yes.

---

## Why bother — what we get

The reason we hold the line on this is not aesthetics. ECS + DOD buys us
three concrete properties that matter for a realtime media server:

1. **Cache coherency.** EnTT packs components contiguously in memory
   (sparse-set storage). When `CompositorSystem` iterates
   `view<Transform, MediaLayer>()`, those components sit in dense arrays
   that march through cache lines instead of chasing pointers. Putting
   logic + vtables inside components fragments those arrays and turns a
   sequential read into a random walk.

2. **Threading discipline.** ADR-0014 makes the editor thread the *sole
   writer* of the registry. The show thread reads from a baked
   `SceneSnapshot` and never touches `registry.emplace`/`replace`/`destroy`.
   This works only because components are value types — if a component
   owned threaded resources or had non-trivial copy semantics, baking
   into a snapshot would be impossible.

3. **Inspectability.** A component is just data. You can print it, diff
   it, serialize it, copy it, lock-free-publish it. Behavior in
   components hides in dynamic dispatch; data in components is visible.

These three properties are why the rules below exist. If you find
yourself bending a rule, the test is: "does this break one of those
three?" Usually that's where the answer is.

---

## Component rules

### Hard rules (never break)

- **No virtual functions.** Vtables add 8 bytes per instance and break
  contiguous storage. They're the single most expensive thing you can
  add to a hot component.
- **No component-to-component pointers.** Entities get destroyed; raw
  pointers between components dangle. Use `entt::entity` IDs and look
  the target up through the registry instead.
- **No reaching outside own data.** A method that takes
  `entt::registry&` and queries other components is a system in
  disguise. Make it a system.

### Soft rules (break only with reason)

- Target **<64 bytes** per component (one cache line). Bigger is fine
  for components that aren't iterated in the hot path — `OutputDisplay`
  and `MappingSurface` are deliberately large because there are tens of
  them, not thousands.
- Avoid `std::string` in components iterated per-frame. It's a heap
  allocation per assignment and 24-32 bytes for the SSO buffer even
  when empty.
- Prefer default member initializers + aggregate construction. Most
  components have no explicit constructor at all.

### Documented exceptions

These exceptions are intentional and reviewed. Don't add new ones
without an ADR-level discussion.

- **`Clip`** has a destructor + move-only semantics that free its
  FFmpeg `AVFormatContext` / `AVCodecContext`. RAII is safer than a
  scattered "don't forget to call close()" contract. Copies are
  deleted to prevent double-free. This is the only component in the
  tree that owns heap resources.

- **`Transform`** has `updateMatrix()`, `setPosition()`, `getMatrix()`
  etc. that mutate only the component's own fields with a dirty-flag
  cache of the computed matrix. No virtuals, no external reach, cache-
  friendly. A dedicated `TransformSystem` would iterate every
  `Transform` every frame just to recompute matrices that already
  maintain a dirty flag. Pragmatic trade-off over strict purity.

- **`AnimatedProperties`** / `KeyframeTrack` have `addKeyframe()`,
  `evaluate()` etc. that operate only on the track's own data. Same
  reasoning as Transform — operations are cache-local, no virtuals, no
  cross-component state. Splitting into free functions would mean
  rewriting all call sites for no runtime benefit.

The unifying principle: **a component method is acceptable if it (1)
has no virtuals, (2) touches only its own fields, and (3) doesn't add
construction or destruction cost.** That preserves the properties we
actually care about (cache layout, entity independence) without dogma.

Interactive logic that *does* reach outside the component (input
handling, UI presets, multi-component side effects) belongs in a
system. See the Camera → CameraControlSystem migration as a worked
example.

---

## System rules

- Operate on `registry.view<Components...>()` for iteration. Don't keep
  your own entity lists alongside the registry — that data drifts.
- Systems are mostly stateless. Per-entity state goes in a component,
  not a `std::unordered_map<entt::entity, ...>` on the system. The
  test: "if I added a second instance of this system, would they
  fight over this state?"
- The base class `System` has virtuals (`update`, `initialize`,
  `shutdown`, `getName`). This is fine — systems aren't iterated in
  packed arrays, so per-frame virtual dispatch is one indirect call
  per system per frame. Trivial.

### Communication

Systems talk to each other **through components**, not direct calls.
Pattern: System A writes a component field; System B's view includes
that component and reads it.

```cpp
// TimelineSystem writes
auto view = registry.view<Clip, FrameBuffer>();
for (auto [e, clip, buf] : view.each()) buf.targetFrame = ...;

// DecodeSystem reads
auto view = registry.view<Clip, FrameBuffer>();
for (auto [e, clip, buf] : view.each()) decode(buf.targetFrame);
```

Direct setter calls between systems (`m_decode->setTimeline(t)`)
appear in setup code (`Engine::initialize`) but should not appear in
the per-frame path. Phase D's bus-message rework will route the last
of those through `bus::IMessageTransport` instead.

### Update order

Defined imperatively in `Engine::update()` (editor thread) and
`Engine::showThreadMain()` (show thread). See
`docs/reference/SYSTEM_ORDERING.md` for the dependency graph and the
show-thread fallback coverage table.

### Systems that hold non-trivial state — acceptable exceptions

- **`DecodeSystem::m_workers`** — `unordered_map<entity, shared_ptr<DecodeWorker>>`.
  Decode workers are threads with FFmpeg contexts; they can't live in
  components. The map is the bridge between ECS entities and the
  thread world. Acceptable.
- **`CompositorSystem::m_pendingAllocations`** —
  `unordered_map<entity, PendingAllocation>`. Show-thread-local cache
  covering the editor-acknowledgment round-trip after
  `createComposeTarget`. Direct consequence of ADR-0014's editor-only-
  writes constraint — the show thread can't write
  `Screen::renderTargetSlot` itself, so it caches its own allocation
  until the editor acks. Acceptable. See the inline comment at
  `CompositorSystem.hpp:67-68`.

---

## Threading model in one paragraph

The editor thread is the sole writer of the EnTT registry. The show
thread runs in parallel and reads only from `bus::SceneSnapshot`
(baked once per editor frame) plus a handful of explicitly-safe
atomic fields (e.g. `Timeline::m_currentTime`). When the editor
stalls (Win32 modal, project load, slow file dialog), critical time-
driven systems also tick on the show thread, but **only those that
don't write the registry**. Today: Timeline and DecodeSystem fall
back; AnimationSystem and SectionScheduler don't (they write registry
components — see `CODE_ISSUES.md` NEW-07 / NEW-08 for the planned
fix). Full rationale: `docs/adr/0014-editor-show-thread-split.md`.

---

## When to break the rules

The rules exist to preserve the three properties at the top of this
file. If you can argue your exception preserves all three, it's
probably OK. Examples that do:

- Trivial inline accessors (`getOpacity()`, `isVisible()`) — no
  cache hit, no thread issue, no inspectability loss. Fine.
- Move-only RAII for owned external resources (FFmpeg, COM). Cache-
  neutral, thread-aware (move is cheap), still inspectable. Fine —
  but document it next to the existing Clip exception.

Examples that don't:

- "It's just one virtual function." Forces vtable on every instance
  in the packed array. Not fine.
- "I'll just store a `Clip*` in this other component." Dangling
  pointer on entity destroy. Not fine — use `entt::entity` and look
  it up.
- "This map<entity, X> in the system is easier than adding a
  component." Per-entity state outside the registry, can't be
  snapshot-baked, breaks single-source-of-truth. Not fine — make it a
  component.

---

## See also

- `include/entity/components/CLAUDE.md` — operator-facing component rule sheet
- `include/entity/systems/CLAUDE.md` — operator-facing system rule sheet, threading model
- `docs/reference/ENTITY_ARCHETYPES.md` — what combinations of components define a "Clip", "Screen", etc.
- `docs/reference/SYSTEM_ORDERING.md` — per-frame system update order + dependency graph
- `docs/adr/0014-editor-show-thread-split.md` — threading architecture
- `docs/reference/CODE_ISSUES.md` — open / fixed ECS-related issues

---

*This doc replaces the deleted `include/entity/core/ECS_GUIDELINES.hpp`
(2024-11-24), whose "Refactoring Plan (Phase 4+)" section was stale —
Transform / Clip / AnimatedProperties were retained as documented
exceptions rather than refactored to pure POD.*
