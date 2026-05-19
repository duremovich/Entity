# ADR-0023: Control-plane plugin outbound via process-shared bridge

- **Status:** Accepted
- **Date:** 2026-05-19
- **Implemented by:** the dmx-artnet plugin's Phase 3 outbound path
  (#13 / #57). See `include/entity/dmx/OutboundBridge.hpp`,
  `src/dmx/OutboundBridge.cpp`, the `SetDmxOutCommand` in
  `include/entity/command/Commands.hpp` / `src/command/Commands.cpp`,
  and the plugin-side hook in `plugins/dmx-artnet/DmxArtnetPlugin.cpp`.
- **Relates to:** ADR-0005 (open-core scaffold; control-plane vs
  hot-path plugin tiers), ADR-0013 (control-plane plugins route
  inbound via `IPluginContext::enqueueCommand`).

## Context

ADR-0013 established the routing convention for inbound control-plane
plugins: incoming OSC / DMX / MIDI / timecode arrive on a plugin
worker thread and reach the engine via
`IPluginContext::enqueueCommand(typeName, paramsJson)`, which puts
the work on `CommandDispatcher`'s queue. That ADR explicitly excluded
`entity-bus` from the inbound path — the bus is for
Director↔Renderer and adding plugin-bound routing would expand its
scope.

Phase 3 of the DMX/Art-Net epic (#13 / #57) needs the symmetric
direction: the engine (or a future timeline cell) decides "set
universe 0 channel 1 to value 255," and the dmx-artnet plugin's
outbound sender emits the corresponding Art-Net + sACN packets at the
44 Hz tick rate.

The original Phase 3 plan called for a new `bus::Message` variant
`DmxFrame { universe, channels[512], priority }` so the engine could
`bus->send(D2R, ...)` and the plugin's worker thread could drain the
bus and consume it. Sketching that out surfaced three problems:

1. **Direction mismatch.** `D2R` means Director → Renderer (in
   single-process Phase D, editor thread → show thread). A
   plugin-bound message isn't either — it's editor thread → plugin
   thread. Forcing it onto D2R either lies about the meaning of
   "Renderer" or doubles the show thread's drain workload with
   messages it must filter out.

2. **No drain loop.** Plugins don't have a `processQueue`-style
   contract today. Adding one means the plugin needs a steady-tick
   wake-up the engine doesn't otherwise schedule for it.

3. **Latency floor.** Bus drain happens once per show-thread tick;
   adding "DMX out" semantics to that tick couples DMX latency to
   render cadence, which is the wrong contract for a control-plane
   protocol that has its own (44 Hz) timing.

## Decision

Outbound from engine to a control-plane plugin uses a
**process-shared function-pointer bridge**, not an `entity-bus`
message.

For DMX specifically:

- A small abstract bridge lives in
  `include/entity/dmx/OutboundBridge.hpp`. The engine includes it;
  the plugin includes it. One translation unit (`src/dmx/OutboundBridge.cpp`)
  in `EntityMediaCore` holds the singleton + the atomic function
  pointer.
- The plugin calls `OutboundBridge::instance().setHook(&fn)` during
  register and `setHook(nullptr)` during shutdown.
- The engine's `SetDmxOutCommand` (and future timeline cells) calls
  `OutboundBridge::instance().setChannel(universe, channel, value,
  priority)`. When no hook is installed, the call is a silent no-op.

The plugin owns all DMX state (`OutboundUniverseTable`,
`OutboundSender`); the bridge is a one-line indirection that lets
the engine reach into the plugin without depending on plugin types
or worrying about plugin lifecycle.

## Why this works

- **No new bus direction.** The bus stays Director↔Renderer.
- **Zero shared state with the show thread.** Output cadence is
  governed by the plugin's own 44 Hz worker; the show thread isn't
  pulled into the loop.
- **Same lifecycle envelope as the inbound path.** The plugin
  installs the hook during register and tears it down inside its
  shutdown hook, before the dispatcher / bus get torn down. The
  atomic function pointer makes the wire/unwire transition safe even
  if a command lands during shutdown.
- **Trivially scales to OSC / MIDI / NDI lookups later.** Each
  plugin-bound feature gets its own small bridge header; the engine
  side stays free of plugin types. We're not paying for a generic
  "plugin bus" today.

## What we considered + rejected

**Add a third bus direction (`E2P`, engine → plugin).** Doubles the
bus's surface area for one feature today, and the drain-loop
contract on plugins still has to be invented. Defer until a second
feature actually needs it.

**Static-link the plugin's outbound table into the engine.** Couples
the engine to plugin internals and inverts the dependency rule
(`plugins/` knows about `entity-plugin-api`; the core does not know
about `plugins/`). Bridge avoids this — the engine sees a function
pointer, not the plugin's `OutboundUniverseTable` type.

**Use `IPluginContext` to register a per-feature reverse callback.**
Cleaner than the bridge in isolation but requires extending the
plugin-api ABI for each new outbound feature (one new
`registerXHandler()` per feature). The free-standing bridge per
feature keeps the plugin-api surface small.

## Consequences

- New header `include/entity/dmx/OutboundBridge.hpp` (Apache-2.0 so
  the plugin may include it under the boundary rules in
  `plugin-api/CLAUDE.md`).
- New TU `src/dmx/OutboundBridge.cpp` linked into `EntityMediaCore`
  unconditionally. When the plugin isn't loaded the bridge is a
  permanent no-op, which is the correct degraded behavior.
- The pattern is reusable: a future OSC outbound feature would add
  `include/entity/osc/OutboundBridge.hpp` + `src/osc/OutboundBridge.cpp`
  and the OSC plugin would install its own hook the same way.
- ADR-0013 is not superseded — it remains the canonical statement
  for inbound routing. This ADR addresses the outbound direction
  it left open.
