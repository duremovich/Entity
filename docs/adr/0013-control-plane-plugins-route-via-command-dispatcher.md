# ADR-0013: Control-plane plugins route via CommandDispatcher (defer bus-routing to Phase E)

- **Status:** Accepted
- **Date:** 2026-05-08
- **Context source:** Working plan
  `~/.claude/plans/i-m-going-to-try-partitioned-hejlsberg.md`
  (deleted after merge — describes shipped work).
- **Implemented by:** `1d46348` — Phase D — OSC receiver plugin +
  Preferences port/enable toggle.
- **Amends:** ADR-0005 (open-core dual-license + plugin scaffold).
  ADR-0005 still stands; this records the routing reality the
  first control-plane plugin actually shipped on.

## Context

ADR-0005 positioned the two plugin transport tiers as:

> **Control-plane plugins** (OSC, timecode, DMX, MIDI, telemetry,
> output-driver lifecycle) subscribe/publish on `entity-bus`. Already
> network-serializable; future static→dynamic upgrade is purely
> additive.

That language treats `entity-bus` as a generic publish/subscribe spine
both endpoints can use. The reality at the moment we shipped the first
control-plane plugin (`plugins/osc-receiver/`, inbound OSC over UDP) is
narrower: `entity-bus` today carries only Director↔Renderer per-tick
state (`RenderFrame`, `RequestComposeCapture`, `ResourcesProvisioned`,
…). It has no message types for "play", "pause", "fire cue N",
"section go", "seek to frame N" — the primitives an inbound OSC
plugin actually needs to publish.

Three options the OSC plugin had at register time:

1. **Add command-style messages to `entity-bus`** — define
   `RemotePlay` / `RemoteFireCue` / etc., teach Director to drain them
   and forward to `CommandDispatcher::enqueue`.
2. **Carry a generic envelope** — single `EnqueueCommand
   { typeName, paramsJson }` bus message, Director drains and
   forwards.
3. **Bypass the bus, call `CommandDispatcher::enqueue` directly** —
   the dispatcher is already thread-safe (mutex-guarded queue),
   already accepts `(typeName, json)` as input (the script runner's
   own entry point), already JSON-serialisable on both sides.

Options 1 and 2 add a translation hop (bus encode → drain → forward to
dispatcher) that does nothing the dispatcher's own JSON path doesn't
already do. The user-visible benefit ADR-0005 cited — wire-format
readiness for Phase E multi-process — is something the dispatcher's
JSON entry point already satisfies. Option 1 also requires a bus
message type per command kind, which permanently couples bus surface
to control-plane vocabulary.

## Decision

**Control-plane plugins call `CommandDispatcher::enqueue(typeName,
paramsJson)` via narrow accessors on `IPluginContext`.** They do NOT
publish onto `entity-bus`.

Plugin-API gained the following at the bottom of the
`IPluginContext` vtable (no `PLUGIN_API_VERSION` bump — the
forward-compatibility rule covers vtable extensions):

- `enqueueCommand(std::string_view typeName, std::string_view paramsJson)`
- `registerShutdownHook(PluginShutdownFn hook)` — engine fires before
  tearing down dispatcher/bus, so worker threads holding the context
  can join cleanly.
- `getBoolSetting(std::string_view, bool defaultValue) const`
- `getIntSetting(std::string_view, int defaultValue) const`

The Settings accessors are stringly-typed by design: the GPL
`Settings` struct lives in `entity/core/`, which Apache-2.0 plugin
headers can't include per the boundary rule. Two narrow accessors
keep the seam clean for the few primitives the OSC plugin needs;
adding a setting later means one new case in
`EnginePluginContext::getBoolSetting` / `getIntSetting`, not a vtable
bump.

**Bus stays Director↔Renderer-only** until Phase E forces multi-
process: at that point, control-plane commands cross a process
boundary and a wire-format translation hop is unavoidable. The
dispatcher's existing JSON envelope is the natural transport for
that translation when it arrives.

## Consequences

**Enables:**
- First control-plane plugin shipped without bus-surface churn.
  `entity-bus` stays focused on its actual job (per-tick
  Director↔Renderer state).
- Adding a new command-style plugin (timecode receiver, MIDI input,
  Companion bridge) is one register-time call to
  `ctx->enqueueCommand("FireCue", R"({"number":1.5})")`. No bus
  message type, no Director drain handler.
- Plugin-API stays narrow. Four new methods total, all forward-
  compatible.

**Forbids:**
- Plugins that need to *receive* bus messages (e.g. observe
  `RenderFrame` for telemetry) still need bus access — that's what
  `IPluginContext::bus()` already exposes. This ADR doesn't take
  that away; it just notes that the *outbound* control-plane path
  is dispatcher-not-bus.

**Forces:**
- When Phase E lands, control-plane plugins need a routing decision:
  the dispatcher itself becomes the wire boundary, OR command-style
  bus messages get added. Either is fine; this ADR doesn't
  pre-commit. The `enqueueCommand` JSON envelope is already
  transport-neutral.
- Future ADR-0005 readers may be surprised by the routing reality.
  This ADR is the canonical pointer.

## Alternatives considered

- **Add `RemoteCommand` bus messages** (option 1 above). Rejected:
  duplicates the dispatcher's JSON envelope, locks bus surface to
  control-plane vocabulary, gains nothing pre-Phase-E.
- **Generic `EnqueueCommand` envelope on the bus** (option 2). Less
  bad than option 1 but still adds a translation hop with no
  user-visible benefit until multi-process. Phase E can adopt it
  then.
- **Expose `CommandDispatcher*` directly on `IPluginContext`.**
  Rejected: same boundary problem as `Settings*`. The dispatcher
  lives in GPL core; plugins are Apache. Two narrow accessors
  preserve the boundary.

## References

- ADR-0005 — open-core dual-license + plugin scaffold (the routing
  promise this ADR records the deviation from).
- ADR-0003 — Director/Renderer service split (the bus that is
  Director↔Renderer-only).
- `plugins/osc-receiver/OscReceiverPlugin.cpp` — first plugin on
  this routing.
- `plugin-api/include/entity/plugin/PluginContext.hpp` — the four
  new vtable entries.
- `src/core/EnginePluginContext.cpp` — engine-side implementations
  forwarding to `Engine::getCommandDispatcher()` and
  `entity::core::activeSettings()`.
