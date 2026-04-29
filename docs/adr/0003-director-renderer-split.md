# ADR-0003: Director/Renderer service split with serializable message bus

- **Status:** Accepted
- **Date:** 2026-04-25
- **Context source:** `~/.claude/plans/yeah-1-lets-plan-zazzy-hartmanis.md`,
  cross-referenced from `~/.claude/plans/so-even-with-hap-cosmic-glacier.md` § Decision 4
- **Implemented by:** Phase D entry (11 subtasks, commits `1743f23`,
  `34ad877`, `056c827`, `df645e4`, `6b1eefe`, `fc1ab72`, `36441dc`, `327304d`,
  `9cfb94b`, `ce0221e`, `9c1f45b`)

## Context

Pixera's Manager-Client topology and Watchout 7's four-service split
(Producer / Director / Runner / Asset Manager) both partition "owns the show
state" from "produces pixels" *even when everything runs on one machine*.
Watchout 7 was a v6 ground-up rewrite specifically to land this. The pattern
is what makes "single machine demo" → "multi-machine cluster" a deployment
topology change rather than a rewrite.

Going into Phase D, Entity's `Engine` was a single class owning the registry,
window, timing, playback, project I/O, clip lifecycle, and callbacks — ~2,000
lines, all coupled. Phase D feature work (timecode, OSC, audio, NDI,
preview/program) would each have to attach to that god-class. Each feature
would land at the wrong layer, and we'd re-do them in Phase E when cluster
work forced a split anyway.

## Decision

Split `Engine` into two services with a **serializable message bus** between
them, before any Phase D feature work attaches:

- **`DirectorService`** owns Timeline, ProjectManager, TranscodeManager,
  command dispatch, OSC/timecode/audio input. It is **the time authority** —
  Renderer never computes "what frame should I be on," only asks.
- **`RendererService`** owns `IRenderer`, OutputManager, CompositorSystem,
  DecodeSystem, FrameCache. Per-Renderer cache; never shared across
  Renderers.
- **Message bus** (`entity-bus` static lib) carries a `Message` variant with
  JSON serialization. Transport starts as `InMemoryMessageTransport` (a
  thread-safe queue). The wire format is the abstraction; the transport is
  swappable. Phase E swaps the in-memory transport for UDP without touching
  message shape or endpoint code.

`SceneState` wraps `entt::registry&` with explicit Read/Write handles, so the
single-process registry can be replaced with a network-replicated one in
Phase E without changing call sites.

`AssetId` is a strong typedef on a content hash, threaded through
`ProvisionClipResources`. Phase E content distribution falls out — Director
and Renderer on different machines agree on identity by hash, resolve local
paths independently.

UI side-channel rule: UI windows hold read-only `Director*` and emit writes
exclusively via `CommandDispatcher`. Documented in
`include/entity/director/CLAUDE.md`.

## Consequences

**Enables:**
- Phase D feature work attaches to the right service from day one. Timecode
  + OSC into Director (drives Timeline). Audio into Director-side. NDI on
  the Renderer side. Preview/program is per-Renderer with the same Director.
- Single-machine cluster *is* multi-machine cluster — only the transport
  changes.
- Determinism boundary is explicit: Renderer can't drift from Director clock
  because Renderer never owns the clock.

**Forbids:**
- Direct cross-service access (no Renderer reaching into Timeline; no
  Director touching `IRenderer`). Enforced by the SceneState handle types
  and the bus message contract.

**Forces:**
- Every cross-service interaction is a bus message. Adds ~1.5 weeks of
  upfront work (vs. ~1 week for a direct split with shared pointers) but
  pays off in Phase E.
- Asset lifecycle goes through `ProvisionClipResources` → `ResourcesProvisioned`
  message round-trip with correlation IDs. Capture broker lands as a parking
  lot for in-flight requests.
- Tests grow a `DirectorRendererRoundtrip` fixture so the bus loop is gated
  end-to-end.

## Alternatives considered

- **Direct in-process split (no serializable bus).** ~1 week instead of
  ~1.5; loses the cluster-readiness payoff. Phase E would force a rewrite
  of the cross-service contract — exactly what this ADR exists to avoid.
- **Defer the split to Phase E.** Every Phase D feature lands on the
  god-class first, then gets ripped out and re-attached in Phase E. The
  rip-and-reattach cost dominates the deferral savings.
- **Microservice-style separate processes from day one.** Overkill for
  single-machine; adds IPC overhead with no benefit until cluster work
  materializes.

## Consequences for memory + lifecycle

- Frame cache lives in RendererService. Director never holds frame data.
- `PlaybackController` split into `PlaybackTimeAuthority` (Director-side, owns
  the clock) + `PlaybackPresenter` (Renderer-side, uploads textures).
- `RenderFrame` per-tick message carries the active set + per-clip frame
  numbers. Renderer hashes the same scene state for `integration_director_renderer_smoke`
  to prove byte-equivalence through the bus.

## References

- Pixera Manager-Client: <https://help.pixera.one/en_US/project-management/manager-client-setup>
- Watchout 7 architecture: <https://docs.dataton.com/watchout-7/.architecture.html>
- Director/Renderer working plan: `~/.claude/plans/yeah-1-lets-plan-zazzy-hartmanis.md`
- Bus implementation: `entity-bus/`, `include/entity/bus/`,
  `include/entity/director/`, `include/entity/renderer/`
- See also ADR-0006 (cluster-ready plumbing) for the wider commitments
  this enables.
