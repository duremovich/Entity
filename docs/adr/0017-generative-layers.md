# ADR-0017: Generative layers — procedural-content timeline layers

- **Status:** Accepted
- **Date:** 2026-05-11
- **Implemented by:** Five commits introducing the Muncher kind end-to-end:
  - `4ba7b2f` — First generative layer kind (Muncher v1): end-to-end pipeline
  - `0790f37` — Muncher player movement: input bus + (x, y) axes drive a yellow square
  - `3809e86` — Muncher gameplay: ghosts chase, pellets get eaten, lives reset
  - `4ffdef8` — GLFW joystick → Muncher input bus (gamepad support)
  - `b0f5015` — OSC routes for Muncher controls (Companion-friendly UDLR + analog axes)
  - This commit — Maze walls + collision + lives HUD + ADR
- **Amends:** ADR-0016 (Timeline Layer abstraction). ADR-0016 reserved
  `Layer::Kind::Generative` and stated that sub-kinds dispatch by
  component composition; this ADR is the first concrete generative
  kind to actually use that hook.
- **Relates to:** ADR-0013 (Control-plane plugins route via the command
  dispatcher) — the OSC routing for Muncher controls follows that
  contract.

## Context

ADR-0016 introduced `Layer::Kind::Generative` as a placeholder for
"procedural texture sources on the timeline" — particle systems, audio
visualizers, maze-chase mini-games, anything that produces a frame
without a media file. The slot existed, but no concrete kind had been
implemented and the architectural shape of "what a generative layer
looks like" was still open.

The disguise reference is the **Tennis** layer (originally called Pong;
renamed for legal reasons). Two visible parameters per axis ("left bat
position", "right bat position") that an external controller — typically
a MIDI fader — drives in realtime. Ball speed in "pixels per beat"
ties game tempo to show timing. That pattern — externally-drivable
floats per axis, no source-specific game code — is the one to copy.

The first generative kind shipped is **Muncher**: a Pac-Man-style
maze-chase mini-game where the player is the lower-case "e" Entity logo
and the ghosts are theme-swappable sprites. It exists primarily to
validate the generative-layer architecture; the gameplay itself is the
forcing function. Theme packs (rival vendor logos as ghost art) load
from `assets/muncher/themes/<theme>/*.png` at runtime, so the easter-egg
artwork can ship as an asset bundle without any trademarked names ever
appearing in source.

## Decision

A generative layer is the third archetype of timeline-resident entity,
alongside Clip and ObjectAnimation. Five architectural decisions, in
priority order:

### 1. Sub-kind dispatch is by component composition, not by a Kind enum

Following the same rule ADR-0016 used to distinguish Clip from
ObjectAnimation, every generative kind gets its own state component.
The presence of `MunchersGameState` on a `Layer(Kind::Generative) +
GenerativeLayer` entity is what tells `GenerativeSystem` to treat it
as a Muncher; no enum dispatch. Future kinds (`ParticlesState`,
`VisualizerState`, etc.) add new state components and new views in the
system.

```cpp
// GenerativeSystem.cpp
auto munchers = registry.view<Layer, GenerativeLayer, MunchersGameState>();
for (auto entity : munchers) {
    tickMuncher(state, inputX, inputY);
}
// Future:
// auto particles = registry.view<Layer, GenerativeLayer, ParticlesState>();
// for (auto entity : particles) { ... }
```

`GenerativeLayer` itself carries only routing fields — `targetScreen`,
render dimensions, future per-layer render-target slot. Kind-specific
data lives on the kind-specific component.

The `GenerativeLayerSnapshot::Kind` enum in the bus message is a wire-
side discriminator only: it tells the show thread which baked fields
are meaningful (`muncher_x`, `muncher_y`, `muncher_pelletBits`, etc.).
It exists because the bus payload can't carry C++ component
composition; on the wire, you need to know which sub-struct's fields
apply. New kinds add a new enum value alongside their new baked
fields, never replace an old one (bus rule 3).

### 2. Inputs flow through a shared `InputBus`, gameplay code is source-agnostic

Generative layers respond to realtime inputs by reading named float
channels from a process-wide `InputBus`. The Muncher kind reads
exactly two: `muncher.input.x` and `muncher.input.y`, each in [-1, 1].
The gameplay code (`tickMuncher`) snaps the magnitude-greater axis to a
cardinal direction and moves; it knows nothing about where the values
came from.

Five sources write to the same channels:

1. **OSC plugin** — `/entity/muncher/up..stop` (Companion-style buttons)
   and `/entity/muncher/input/x|y <float>` (TouchOSC faders), routed
   through `CommandDispatcher::enqueue("SetInputChannel", ...)` per
   ADR-0013.
2. **GLFW joystick** — left stick X/Y on joystick slot 1, polled in
   `Engine::update`.
3. **Editor keyboard** — WASD / arrow keys, polled in `Engine::update`,
   gated on ImGui `WantTextInput`.
4. **Script command** — `SetInputChannel` writes the channel directly;
   for headless tests + automation.
5. **Future** — MIDI plugin, audio analyzer, timecode-stepped, networked
   clients. All write to the same channels.

Local sources (joystick > keyboard) share a "only WRITE while active"
guard: they touch the channel while a key is held and for one trailing
zero-write the tick after release, then go silent. This is the seam
that lets scripts and remote sources drive the same channels without
the local poller overwriting them every tick. Joystick takes priority
over keyboard because if both are active the operator intent is clearer
from the analog stick.

The naming convention is `"<kind>.<scope>.<axis>"` — namespaced so a
future audio analyzer writing to `audio.kick` doesn't collide with a
Muncher input, and so multiple Muncher layers on the same project
could be addressed by a numeric suffix (`muncher.input.x` for the
primary, `muncher.2.input.x` for a second, etc.) without changing the
gameplay code's read sites.

Unknown channels read as a caller-supplied fallback; gameplay code
never crashes on a missing wire. Adding a new input source is purely
additive — a channel no plugin writes simply reads 0 forever.

### 3. Editor-thread tick, show-thread snapshot bake — same shape as ADR-0014

The simulation runs on the editor thread (sole registry writer per
ADR-0014). `GenerativeSystem::update` ticks once per editor frame,
sequenced after `AnimationSystem` so animated layer parameters (a
future opacity track on a generative layer, for instance) settle
before the sim reads them. `MunchersGameState` is the registry
component the system writes.

`PlaybackTimeAuthority::buildSceneSnapshot` bakes the per-layer state
into `bus::GenerativeLayerSnapshot` — Muncher position, ghost
positions, pellet bitset, wall bitset, score, lives. The bake is
filtered editor-side to currently-active timeline frames, so the show
thread iterates `RenderFrame::generativeLayers` directly without
re-checking start/duration.

The show side (`CompositorSystem`) reads the snapshot and draws.
Same compose-target-per-screen pattern Clip rendering already uses;
generative draws happen inside the per-screen loop, after Clip draws,
matched by `targetScreen`. Every visible "game entity" (maze wall,
pellet, ghost, Muncher, lives HUD) is a transformed colored quad
through the existing `drawColoredQuad` path. No new D3D12 pipeline
state has been added.

Editor-stall behavior in V1: the Muncher freezes during modal
dialogs / project loads, same as `AnimationSystem` pre-NEW-07. The
snapshot-bake plumbing is in place to add a show-thread re-tick later
without writing the registry, mirroring the NEW-07 fix. Punted for
now; document as a known limitation.

### 4. Wire-format note: 256-bit grids are hex strings, not uint64 arrays

The `muncher_wallBits` and `muncher_pelletBits` fields are
`std::array<std::uint64_t, 4>` (256 bits = 16×16 grid). On the wire
they encode as 64-char hex strings, not arrays of numbers.

This codebase's `nlohmann::ordered_json` build deterministically
stalls the screenshot readback pipeline downstream of a payload that
contains raw `UINT64_MAX` (`~0ull`) values. The exact root cause lives
inside ordered_json's number serialization and is out of scope for
this ADR — hex-string encoding sidesteps it cleanly and is a
reasonable wire format for bitsets anyway (compact, debuggable,
round-trip-stable). The deserializer accepts either uppercase or
lowercase hex.

The two endpoints are in the same process today, so the workaround is
invisible end-to-end. Phase E (cross-process bus) inherits the same
encoding without changes.

### 5. Theme packs are loaded from disk, not compiled into the source

For the eventual sprite-atlas rendering phase, ghost art will load
from `assets/muncher/themes/<theme>/*.png` at runtime. The `Default`
theme ships in the repo; alternate themes — including the rival-vendor-
logo easter egg the Muncher mascot is built around — drop into
`themes/<theme>/` at install time. The code knows ghost slots `0..2`
exist and which color tint each gets; it never knows what the sprite
actually depicts.

This keeps trademarked imagery out of `git log`, ADRs, commit messages,
and CI artifacts. The joke ships as an asset bundle, the source ships
generic.

## Consequences

**Positive**

- The first generative kind exists end-to-end (editor input → simulation
  → snapshot → show thread → projector output) with five real input
  sources. Subsequent kinds reuse the same plumbing — components +
  `tickX` function + snapshot fields + Compositor draw branch — without
  inventing new architecture.
- Gameplay code reads inputs by name. Adding a new controller plugin is
  one new input source writing to existing channels; no game-logic
  change required. Validates the "joystick or OSC or MIDI or
  timecode-stepped, gameplay doesn't care" promise made at the start of
  the Muncher work.
- OSC routing reuses ADR-0013 (control-plane via `CommandDispatcher::
  enqueue`). The osc-receiver plugin gained 60 lines for the new
  routes; nothing else needed to know it happened.
- The architecture is decoupled enough that the sprite-rendering phase
  is a pure substitution — replace `drawColoredQuad` calls with
  `drawTexturedQuadInstanced` against a sprite atlas — with no changes
  to game logic, input plumbing, or snapshot wiring.

**Negative**

- The simulation freezes during editor stalls (modal dialogs, slow
  project load). Documented; the snapshot-bake plumbing is in place
  for a future show-thread fallback.
- Performance ceiling at 256×256 compose target with all 256 pellets
  + 80 wall cells + 3 ghosts + Muncher = ~340 draw calls per layer per
  frame. At 512×512 the screenshot pipeline stalls under this load
  (fence-wait timing). Real fix is sprite-atlas instancing —
  collapses the pellet + wall loops to one or two instanced draw calls.
  Until then, 256×256 is the supported target.
- Game state lives on `MunchersGameState` — ~120 bytes per entity.
  Bigger than the components/CLAUDE.md soft-rule (<64 bytes), but
  there is at most one Muncher per layer and layers aren't iterated in
  a tight view, so cache pressure is irrelevant. Documented exception
  alongside the existing OutputDisplay / Screen ones.

**Neutral**

- The shipped maze is hardcoded as a 16-row ASCII constant in
  `GenerativeSystem.cpp`. Loading the maze from a `.txt` next to the
  theme pack is a small future-work item but not load-bearing.

## Alternatives considered

**Generative kinds as a plugin ABI.** Future-flexibility argument for
defining a `GenerativeKind` interface across the plugin boundary so
each kind could ship as a separate plugin. Rejected for v1 because
the cost is real (new ABI, new vtable contract, lifetime management
across the GPL/Apache boundary) and there's only one kind. Revisit
when a second generative kind exists in core; the first plugin
generative kind is what forces the abstraction.

**Direct atomic write from OSC to the InputBus.** The OSC plugin
runs on its own thread; it could write `InputBus` channels directly
with atomics, bypassing `CommandDispatcher::enqueue`. Lower latency
(one frame less). Rejected because it introduces a second control-
plane mechanism alongside the existing one and ADR-0013 was an
explicit decision to route control-plane through the dispatcher. The
single-frame latency is acceptable for sub-60Hz inputs; revisit if a
future input source needs sub-frame timing (audio-rate visualizer
correlation, real-time audio synthesis, etc.).

**Per-layer render target + textured composite.** Each generative
layer renders to its own offscreen RT, then the compositor samples
that RT as a textured quad on the target screen. The disguise model.
Rejected for v1 because it requires per-layer RT allocation + an
R2D ack round-trip + a new descriptor heap slot policy, and the
current direct-compose approach is visually indistinguishable for
the Muncher's single-layer-per-screen use case. Add it when a
feature actually needs it (post-processing pass on a generative
layer's output, recording the layer's raw output to disk, opacity-
blending two generative layers on the same screen).

**Sprite atlas + instancing before maze walls.** Would have been the
"correct" perf-first order. Chose maze walls first because the
gameplay-feel improvement is bigger and the sprite atlas requires
non-trivial D3D12 work (new PSO, new shader, instance buffer
management) that's a clean separate ADR when it lands.

## See also

- [ADR-0016: Timeline Layer abstraction](0016-timeline-layer-abstraction.md)
  — where `Kind::Generative` was reserved.
- [ADR-0014: Editor/show thread split](0014-editor-show-thread-split.md)
  — the threading constraint the snapshot bake satisfies.
- [ADR-0013: Control-plane plugins route via CommandDispatcher](0013-control-plane-plugins-route-via-command-dispatcher.md)
  — the OSC routing follows this.
- `include/entity/components/GenerativeLayer.hpp` + `MunchersGameState.hpp`
  — the kind components.
- `src/systems/GenerativeSystem.cpp` — the per-tick simulation.
- `include/entity/input/InputBus.hpp` + `src/input/InputBus.cpp` —
  the input channel registry.
- `plugins/osc-receiver/OscReceiverPlugin.cpp` — the OSC routes.
- `scripts/integration/muncher_osc_test.py` — Python OSC end-to-end test.
