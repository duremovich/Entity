# ADR-0024: getStringSetting bridge for project-scoped plugin state

- **Status:** Accepted
- **Date:** 2026-05-19
- **Implemented by:** Phase 5 of the DMX/Art-Net epic (#13 / #59). See
  the appended `IPluginContext::getStringSetting` method in
  `plugin-api/include/entity/plugin/PluginContext.hpp`, the engine-
  side resolution in `src/core/EnginePluginContext.cpp`, the
  `dmxMappingsJson` accessor on `ProjectManager`, the v22 schema bump
  in `ProjectSerializer.hpp/cpp`, the new `ShowControlWindow`, and
  the plugin-side reader in
  `plugins/dmx-artnet/MappingResolver.cpp`.
- **Relates to:** ADR-0005 (open-core / plugin-api boundary),
  ADR-0023 (engine-to-plugin outbound bridge — adjacent solution for
  the outbound DMX case).

## Context

Phase 5 of the DMX/Art-Net epic needs per-project DMX channel
mappings: editing a project's DMX table assigns lighting-console
channels to engine commands, and that table travels in the `.entity`
file alongside the rest of project state. The dmx-artnet plugin
must read those mappings; the editor's `ShowControlWindow` must
write them. The plugin is Apache-2.0 across a hard ABI boundary
(see `plugin-api/CLAUDE.md`); the editor and project files are GPL
core.

Three properties have to hold:

1. **Plugin reads project-scoped state without including project
   headers.** `plugin-api/CLAUDE.md` rule 3 forbids
   `entity/project/...` includes from any plugin.

2. **Typed mapping struct stays on the plugin side.** The
   `DmxMapping` POD with `TriggerKind`, scale ranges, etc., is
   plugin-internal authoring concept. Promoting it into core just to
   round-trip the JSON would force a parallel GPL copy.

3. **Existing accessor pattern is preserved.** `IPluginContext`
   already exposes `getBoolSetting` / `getIntSetting` against the
   `Settings` struct via stringly-typed keys. The cost of a new
   accessor should be one vtable slot, no new headers, no new
   types.

## Decision

Append one method to `IPluginContext` at the bottom of the vtable:

```cpp
virtual std::string getStringSetting(std::string_view key,
                                     std::string_view defaultValue) const noexcept = 0;
```

The implementation in `EnginePluginContext::getStringSetting` is
the bridge: certain keys resolve to fields on the `Settings` struct
(matches existing `getBoolSetting` / `getIntSetting` pattern), and
certain **synthetic** keys resolve to the active project's state.

Today the synthetic-key set is:

| Key | Source | Notes |
|---|---|---|
| `dmxMappingsJson` | `ProjectManager::dmxMappingsJson()` | Raw JSON string the dmx-artnet plugin parses on every packet |

Project state is persisted via the project file (`.entity` schema
v22+ adds a top-level `dmxMappingsJson` string field); the editor's
`ShowControlWindow` edits the string in place. The plugin reads
through the accessor and falls back to its baked default mapping
table when the string is empty.

## Why a single accessor and not a typed cross-boundary contract

We considered four options before landing here:

1. **A typed `DmxMapping` schema in core.** Forces a parallel
   GPL-side copy of an Apache-2.0 authoring struct and a typed
   serializer to match. Doubles surface area for negligible safety
   gain (the JSON is round-tripped, not the typed struct — the
   string is the contract).

2. **A new `IPluginContext` method per feature
   (`getDmxMappings()`).** Couples the plugin-api ABI to every
   per-feature plugin we ship. Every new mapping-style feature
   would bump or extend the vtable. The string accessor lets the
   bridge live entirely on the engine side, where adding a key is
   a one-line case.

3. **An `IPluginContext::getProjectStateJson()` method.** Returns
   the full project as one giant JSON blob. Wasteful and forces
   every plugin to parse state it doesn't need. The narrow
   per-key accessor preserves least-privilege.

4. **A second separate bridge header per
   feature** (`include/entity/dmx/MappingsBridge.hpp` mirroring
   ADR-0023's `OutboundBridge`). Reasonable, but it's a separate
   header and TU for what amounts to one read of a string. The
   accessor on `IPluginContext` is the existing seam — extending
   it is cheaper than a new bridge.

Outbound DMX (ADR-0023) lands on a per-feature bridge because the
contract there is "engine writes, plugin reads with cadence" — a
function pointer is the lightest possible abstraction. Mappings are
"plugin reads on demand, engine writes through a UI" — a string-
keyed read on the existing accessor is the lightest possible
abstraction. Different problem shapes, different bridges.

## Consequences

- One additional `IPluginContext` vtable slot. Bottom of vtable so
  older plugins compiled against pre-v0 headers continue to work
  (same forward-compat convention as `enqueueCommand`,
  `registerShutdownHook`, `getBoolSetting`).
- New synthetic-key namespace lives entirely in
  `EnginePluginContext::getStringSetting`. Adding a new key is a
  one-line case in that switch.
- `ProjectSerializer` bumps to v22 with one new optional string
  field. Pre-v22 projects load with empty `dmxMappingsJson` and
  the plugin falls back to its baked default mapping table — no
  user-visible difference for projects that never touched DMX.
- The pattern is reusable: a future "per-project OSC mappings"
  feature would add a `oscMappingsJson` synthetic key + project
  field with no further plugin-api surface change.
- `ShowControlWindow` is the editor surface. v1 is a multi-line
  text input bound to the raw JSON string; a typed table editor is
  a follow-up that doesn't require any schema or boundary work.
