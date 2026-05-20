# osc-receiver

<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Dylan Uremovich -->

Inbound OSC 1.0 over UDP. Binds to `0.0.0.0:<port>` (default 53000, configurable
in Preferences > OSC Receiver) and routes incoming messages to Director commands
via `IPluginContext::enqueueCommand`. Handles plain messages and `#bundle` packets
recursively. Accepts `i`, `h`, `f`, `d` numeric argument types and `s` strings.

---

## Default inbound routes

| OSC address | Director command | Notes |
|---|---|---|
| `/entity/play` | `Play` | |
| `/entity/pause` | `Pause` | |
| `/entity/stop` | `Pause` + `SeekToFrame{frame:0}` | |
| `/entity/section/next` | `SectionGo` | |
| `/entity/seek <int>` | `SeekToFrame{frame:N}` | any numeric arg type |
| `/entity/cue/{number}/go` | `FireCue{number:N}` | decimal cue numbers (e.g. 1.5, 2.10) |
| `/entity/muncher/up` | `SetInputChannel` x=0, y=-1 | |
| `/entity/muncher/down` | `SetInputChannel` x=0, y=+1 | |
| `/entity/muncher/left` | `SetInputChannel` x=-1, y=0 | |
| `/entity/muncher/right` | `SetInputChannel` x=+1, y=0 | |
| `/entity/muncher/stop` | `SetInputChannel` x=0, y=0 | |
| `/entity/muncher/input/x <f>` | `SetInputChannel` muncher.input.x | analog fader |
| `/entity/muncher/input/y <f>` | `SetInputChannel` muncher.input.y | analog fader |

When `/entity/seek` is received with no numeric argument the command is
silently dropped (warn logged). Same for `/entity/cue/{number}/go` with an
unparseable cue number.

---

## Per-project inbound mapping overrides

Routes are configurable per-project via the OSC In tab of the Show Control
window (Window > Show Control). The JSON blob is stored in the project file
under `oscInboundMappingsJson`.

Shape:

```json
[
  {
    "address": "/show/cue/{N}/go",
    "captureKey": "N",
    "commands": [
      { "type": "FireCue", "params": "{\"number\":$capturef}" }
    ]
  },
  {
    "address": "/show/play",
    "commands": [
      { "type": "Play", "params": "" }
    ]
  }
]
```

- Top-level is a bare JSON array. An empty array or a missing/invalid JSON blob
  falls back to the 13 default routes above.
- `captureKey` is optional; if omitted it is derived from the `{name}` token in
  the address pattern.
- `commands` is an array; all listed commands are dispatched in order when the
  route matches (enables multi-command routes like the default `/entity/stop`).
- `params` is a JSON string (or empty string for commands with no parameters).
  Supported substitution tokens in `params`:
  - `$arg0i` — first numeric OSC argument as integer
  - `$arg0f` — first numeric OSC argument as double
  - `$capturei` — address capture parsed as integer
  - `$capturef` — address capture parsed as double
  Any unresolvable token causes that command to be skipped with a warning.

The route table is rebuilt automatically when the project-level JSON changes
(hot-reload, no restart required).

---

## Settings (`Settings.json` / Preferences)

```json
"oscReceiverEnabled": true,
"oscReceiverPort": 53000
```

- `oscReceiverEnabled` (bool, default `true`) — master on/off.
- `oscReceiverPort` (integer 1-65535, default 53000) — UDP listen port.
  Override at runtime with `ENTITY_OSC_PORT` environment variable.

---

## Files

- `OscReceiverPlugin.cpp` — plugin entry point, UDP worker thread, dispatch
- `OscInboundMappings.hpp` — route-table types + JSON parser + `matchPattern` +
  `expandTemplate` (`namespace osc_inbound`); also included by
  `tests/unit/OscMappingTableTest.cpp`
- `manifest.json` — plugin registration metadata
