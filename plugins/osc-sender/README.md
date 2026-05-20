# osc-sender

<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright 2026 Dylan Uremovich -->

Outbound OSC 1.0 over UDP. Broadcasts Entity transport and section state to one
or more destinations at 30 Hz; diagnostic heartbeat fires at 1 Hz. Per-project
mapping overrides let individual events be disabled or redirected to a custom
address without recompiling.

---

## Default broadcast events

| Event ID | OSC address | Type | Rate |
|---|---|---|---|
| `transport.state` | `/entity/out/transport/state` | `s` | on-change |
| `transport.frame` | `/entity/out/transport/frame` | `i` | 30 Hz (playing) |
| `transport.seconds` | `/entity/out/transport/seconds` | `f` | 30 Hz (playing) |
| `section.active.index` | `/entity/out/section/active/index` | `i` | on-change |
| `section.active.frame` | `/entity/out/section/active/frame` | `i` | on-change |
| `section.next.index` | `/entity/out/section/next/index` | `i` | on-change |
| `section.next.frame` | `/entity/out/section/next/frame` | `i` | on-change |
| `project.name` | `/entity/out/project/name` | `s` | on-change |
| `heartbeat` | `/entity/out/heartbeat` | `i` | 1 Hz |

`transport.state` string values: `"stopped"`, `"playing"`, `"paused"`.

Multiple events that fire in the same 30 Hz tick are coalesced into a single
OSC bundle (`#bundle` + immediate timetag `0x0000000000000001`).

---

## Settings (`Settings.json` / Preferences)

```json
"oscSenderEnabled": true,
"oscSenderDestinationsJson": "[{\"host\":\"192.168.1.50\",\"port\":8000,\"enabled\":true}]"
```

- `oscSenderEnabled` (bool, default `true`) — master on/off. When false the
  plugin registers but does not open a socket or spawn a worker thread.
  Override at launch with the `ENTITY_OSC_SENDER_ENABLED=1` environment variable.
- `oscSenderDestinationsJson` (string) — JSON array of destination objects. Each
  entry: `host` (string), `port` (integer 1-65535), `enabled` (bool). Disabled
  entries are parsed but skipped at send time. Multiple destinations are sent
  to in sequence each tick.

---

## Per-project outbound mapping overrides

Each project can override the address and/or suppress individual events. The
JSON blob is stored in the project file under `oscOutboundMappingsJson` and
edited in the OSC Out tab of the Show Control window.

Shape:

```json
{
  "events": {
    "transport.frame": { "enabled": true, "address": "/show/frame" },
    "heartbeat":       { "enabled": false, "address": "" }
  }
}
```

- Omitted event IDs use the default address and are enabled.
- `"address": ""` means "use the default address for this event".
- `"enabled": false` suppresses the event entirely.

Valid event IDs are the nine values in the **Default broadcast events** table
above (`transport.state`, `transport.frame`, `transport.seconds`,
`section.active.index`, `section.active.frame`, `section.next.index`,
`section.next.frame`, `project.name`, `heartbeat`).

---

## Manual smoke procedure

1. Start the smoke listener in a terminal:

   ```
   python scripts/osc_smoke_listen.py 53001
   ```

2. Run the Entity editor. Confirm "osc-sender: sending to 1 destination(s)" in
   the log (Preferences > OSC Sender must be enabled with at least one
   destination pointing at `127.0.0.1:53001`).

3. Press Play in the timeline. The listener terminal should print:

   ```
   [bundle from 127.0.0.1:...]
     /entity/out/transport/state  string=playing
     /entity/out/transport/frame  int32=0
     /entity/out/transport/seconds  float32=0.0
   ```

4. Scrub the playhead. Frame and seconds values should update at ~30 Hz. The
   heartbeat counter increments roughly once per second at `int32=1`, `2`, etc.

5. Press Stop. Confirm `transport.state  string=stopped` fires once.

---

## Files

- `OscSenderPlugin.cpp` — plugin entry point, 30 Hz worker, JSON parsers
- `OscWire.hpp` — OSC 1.0 wire-format encode helpers (`namespace osc`); also
  included by `tests/unit/OscEncodingTest.cpp`
- `manifest.json` — plugin registration metadata
