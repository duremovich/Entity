# ADR-0006: Cluster-ready plumbing from day one

- **Status:** Accepted
- **Date:** 2026-04-25
- **Context source:** `~/.claude/plans/so-even-with-hap-cosmic-glacier.md`
  § Decision 2, Decision 5
- **Implemented by:** Phase D entry — bus serialization, SceneState handles,
  AssetId typedef (commits `1743f23`, `056c827`, `9c1f45b`)

## Context

Multi-machine deployment is not on the immediate 12-month roadmap, but
every architectural decision from here forward should assume cluster is the
eventual deployment topology. The pattern across Disguise, Pixera, Watchout
is: hardware genlock + content-hash asset distribution + per-Renderer cache
+ Director-as-time-authority.

The cost of building this in incrementally is roughly 2-3 weeks across
Phase C.10/C.12 and Phase D entry. The cost of retrofitting it later is
months, because cluster work touches the message format, asset lifecycle,
cache ownership, and the time-authority contract — all of which calcify
once Phase D features ship on top of single-machine assumptions.

The decision to spend 2-3 weeks now in exchange for not rewriting the
engine for cluster in Phase E is what this ADR records.

## Decision

Five concrete architectural commitments that pay off in Phase E+:

1. **Director/Renderer message bus is network-serializable from day one.**
   See ADR-0003. JSON envelope, in-memory transport now, UDP transport in
   Phase E with no message-format change. Wire format is the abstraction;
   transport is swappable.

2. **Asset references use content-hash IDs**, not absolute paths. Steal
   Disguise's pattern: every imported file gets a content hash on ingest;
   project files reference assets by hash; the local asset path is
   resolved per-machine. `AssetId` strong typedef threaded through
   `ProvisionClipResources` (commit `9c1f45b`).

3. **Frame cache is per-Renderer-process**, never shared. Each render node
   owns its own cache. The Director never holds frame data. Implemented
   in Phase C #10.1 (FrameCache lives in RendererService).

4. **Time authority lives in the Director**, never in the Renderer. Even on
   single machine: Renderer asks "what frame should I be on right now?"
   rather than computing it locally. This is what makes
   single-Director-many-Renderer work without state duplication. Implemented
   in Phase D entry subtask 6 (`PlaybackController` →
   `PlaybackTimeAuthority` + `PlaybackPresenter` split).

5. **No singletons that assume one process.** Engine subsystems become
   services with explicit lifecycle. Phase D entry subtask 5 extracted
   `Renderer` as a service; future cluster work extracts the rest.

**Cluster sync model — hardware-deferred.** When cluster work lands in
Phase E+, do not invent a software replacement for hardware genlock. Pixera
and Disguise both buy NVIDIA Quadro Sync II + feed it BlackBurst /
Tri-Level / PTPv2 SMPTE 2059-2; the GPU never speaks PTP directly.
Software-only sync caps the product at non-broadcast use cases (~2ms
drift, fine for projection blends, terrible for camera-facing LED walls).
The d3net-equivalent message bus carries transport state + parameter
changes; the Quadro Sync card carries frame-presentation timing. **Both,
not one.**

## Consequences

**Enables:**
- Phase E cluster work is wiring up a UDP transport + a Quadro Sync card,
  not redesigning the engine.
- Single-Director-many-Renderer is the same code path as
  single-Director-one-Renderer.
- Asset distribution via content hash means a project file moves between
  machines without absolute-path breakage.

**Forbids:**
- Software-only multi-machine sync as a feature. If a customer wants
  multi-machine, they buy the Sync card.
- Renderer-side time computation. Anywhere in the engine that asks "what
  time is it?" goes through the Director.
- Cross-Renderer cache sharing. If two Renderers need the same frame, they
  decode it independently. Memory isn't shared; the cache is private.

**Forces:**
- ~2-3 weeks of upfront work distributed across Phase C.10 (cache shape),
  Phase C.12 (color pipeline being per-Renderer-friendly), and Phase D
  entry (the split itself).
- Discipline against expedient shortcuts. Every "I'll just compute the
  current frame here" Renderer-side change is a regression of decision #4.

## Alternatives considered

- **Software-only cluster sync.** Watchout 7 ships this (~2ms NTP drift).
  Acceptable for projection blends. Unacceptable for camera-facing video
  walls. Targeting the lower bar locks Entity out of the higher-end
  market — exactly the market where "Disguise replacement" matters.
- **Defer everything to Phase E.** The cost of the deferred path is
  months of rewrite. The cost of the up-front path is weeks. The
  rewrite cost dominates by an order of magnitude.
- **Build full cluster now.** Solo-developer time budget can't absorb it.
  This ADR commits to the *plumbing*, not the cluster work itself.

## References

- Pixera Genlock: <https://help.pixera.one/1214969-synchronize-outputs-genlock-framelock-setup>
- Disguise Genlock: <https://help.disguise.one/designer/configuration/genlock-configuration>
- Disguise IP-VFC PTPv2 SMPTE 2059-2: <https://help.disguise.one/hardware/ip-vfc/ip-vfc-genlock>
- Watchout 7 NTP-only sync (counter-example): <https://docs.dataton.com/watchout-7/.architecture.html>
- Working plan: `~/.claude/plans/so-even-with-hap-cosmic-glacier.md`
- Bus implementation: `entity-bus/`, `include/entity/bus/`
- See also ADR-0003 (Director/Renderer split) for the structural change
  this rides on top of.
